/*
 * obs-marker-patch  —  marker-patch.cpp
 *
 * Three operation modes:
 *   A. Recording stopped  → wait for hybrid-MP4 remux, inject XMP
 *   B. Startup scan       → remux orphaned MKVs + inject XMP
 *   C. Manual folder fix  → Tools menu → folder picker → process all MP4/MKV
 *
 * Requires: exiftool  (winget install OliverBetz.ExifTool)
 *           ffmpeg    (bundled with OBS, used for MKV remux)
 */

#include "marker-patch.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/config-file.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>
#include <vector>
#include <thread>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <atomic>

// ---------------------------------------------------------------------------
// Auto-behaviour toggles (toggled from Qt UI thread, read on worker threads)
// ---------------------------------------------------------------------------

static std::atomic<bool> s_auto_markers{true};
static std::atomic<bool> s_auto_trim{true};

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------

static std::string run_capture(const std::string &cmd)
{
	SECURITY_ATTRIBUTES sa = {};
	sa.nLength              = sizeof(sa);
	sa.bInheritHandle       = TRUE;

	HANDLE hRead, hWrite;
	if (!CreatePipe(&hRead, &hWrite, &sa, 0))
		return "";
	SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOA si = {};
	si.cb           = sizeof(si);
	si.hStdOutput   = hWrite;
	si.hStdError    = hWrite;
	si.dwFlags      = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow  = SW_HIDE;

	PROCESS_INFORMATION pi   = {};
	std::string         mcmd = cmd;
	if (!CreateProcessA(NULL, mcmd.data(), NULL, NULL, TRUE,
	                    CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		CloseHandle(hRead);
		CloseHandle(hWrite);
		return "";
	}
	CloseHandle(hWrite);

	std::string result;
	char        buf[4096];
	DWORD       n;
	while (ReadFile(hRead, buf, sizeof(buf) - 1, &n, NULL) && n > 0) {
		buf[n] = '\0';
		result += buf;
	}
	WaitForSingleObject(pi.hProcess, 30000);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(hRead);
	return result;
}

static int run_wait(const std::string &cmd)
{
	STARTUPINFOA si = {};
	si.cb           = sizeof(si);
	si.dwFlags      = STARTF_USESHOWWINDOW;
	si.wShowWindow  = SW_HIDE;

	PROCESS_INFORMATION pi   = {};
	std::string         mcmd = cmd;
	if (!CreateProcessA(NULL, mcmd.data(), NULL, NULL, FALSE,
	                    CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
		return -1;

	WaitForSingleObject(pi.hProcess, 120000);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return (int)code;
}

// ---------------------------------------------------------------------------
// Find exiftool
// ---------------------------------------------------------------------------

static std::string s_exiftool_cache;

static std::string find_exiftool()
{
	if (!s_exiftool_cache.empty())
		return s_exiftool_cache;

	static const char *candidates[] = {
		"exiftool",
		"exiftool.exe",
		"C:\\Windows\\exiftool.exe",
		"C:\\Windows\\System32\\exiftool.exe",
		nullptr,
	};

	for (int i = 0; candidates[i]; i++) {
		std::string test =
			std::string("\"") + candidates[i] + "\" -ver";
		std::string out = run_capture(test);
		// Valid version starts with a digit
		if (!out.empty() && out[0] >= '1' && out[0] <= '9') {
			s_exiftool_cache = candidates[i];
			return s_exiftool_cache;
		}
	}
	return "";
}

// ---------------------------------------------------------------------------
// Find ffmpeg  (bundled with OBS at <obs_root>\bin\64bit\ffmpeg.exe)
// ---------------------------------------------------------------------------

static std::string s_ffmpeg_cache;

static std::string find_ffmpeg()
{
	if (!s_ffmpeg_cache.empty())
		return s_ffmpeg_cache;

	auto probe = [](const char *p) -> bool {
		std::string cmd = std::string("\"") + p + "\" -version";
		std::string out = run_capture(cmd);
		return out.find("ffmpeg version") != std::string::npos;
	};

	// OBS 31+ ships ffmpeg.exe alongside obs64.exe (bin\64bit)
	const char *mod_bin =
		obs_get_module_binary_path(obs_current_module());
	if (mod_bin) {
		std::string p = mod_bin;
		size_t      a = p.rfind('\\');
		if (a != std::string::npos) {
			size_t b = p.rfind('\\', a - 1);
			if (b != std::string::npos) {
				size_t c = p.rfind('\\', b - 1);
				if (c != std::string::npos) {
					std::string cand = p.substr(0, c) +
					                   "\\bin\\64bit\\ffmpeg.exe";
					if (probe(cand.c_str())) {
						s_ffmpeg_cache = cand;
						return s_ffmpeg_cache;
					}
				}
			}
		}
	}

	static const char *candidates[] = {
		"ffmpeg",
		"ffmpeg.exe",
		"C:\\Program Files\\Shutter Encoder\\Library\\ffmpeg.exe",
		nullptr,
	};
	for (int i = 0; candidates[i]; i++) {
		if (probe(candidates[i])) {
			s_ffmpeg_cache = candidates[i];
			return s_ffmpeg_cache;
		}
	}

	// WinGet links directory (not always on OBS process PATH)
	char localapp[MAX_PATH] = {};
	if (GetEnvironmentVariableA("LOCALAPPDATA", localapp,
	                            sizeof(localapp))) {
		std::string cand = std::string(localapp) +
		                   "\\Microsoft\\WinGet\\Links\\ffmpeg.exe";
		if (probe(cand.c_str())) {
			s_ffmpeg_cache = cand;
			return s_ffmpeg_cache;
		}
	}

	return "";
}

// ---------------------------------------------------------------------------
// Find ffprobe  (Shutter Encoder bundles it; also check WinGet links + PATH)
// ---------------------------------------------------------------------------

static std::string s_ffprobe_cache;

static std::string find_ffprobe()
{
	if (!s_ffprobe_cache.empty())
		return s_ffprobe_cache;

	auto probe = [](const char *p) -> bool {
		std::string cmd = std::string("\"") + p + "\" -version";
		std::string out = run_capture(cmd);
		return out.find("ffprobe version") != std::string::npos;
	};

	static const char *candidates[] = {
		"ffprobe",
		"ffprobe.exe",
		"C:\\Program Files\\Shutter Encoder\\Library\\ffprobe.exe",
		nullptr,
	};
	for (int i = 0; candidates[i]; i++) {
		if (probe(candidates[i])) {
			s_ffprobe_cache = candidates[i];
			return s_ffprobe_cache;
		}
	}

	// WinGet links directory
	char localapp[MAX_PATH] = {};
	if (GetEnvironmentVariableA("LOCALAPPDATA", localapp,
	                            sizeof(localapp))) {
		std::string cand = std::string(localapp) +
		                   "\\Microsoft\\WinGet\\Links\\ffprobe.exe";
		if (probe(cand.c_str())) {
			s_ffprobe_cache = cand;
			return s_ffprobe_cache;
		}
	}

	return "";
}

// ---------------------------------------------------------------------------
// Wait for file to stabilise (hybrid-MP4 remux running)
// ---------------------------------------------------------------------------

static void wait_for_stable(const std::string &path, int timeout_sec)
{
	LARGE_INTEGER prev  = {};
	int           same  = 0;
	int           ticks = 0;
	const int     max   = timeout_sec * 2;

	while (ticks < max) {
		Sleep(500);
		ticks++;

		WIN32_FILE_ATTRIBUTE_DATA info = {};
		if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard,
		                          &info))
			continue;

		LARGE_INTEGER cur;
		cur.HighPart = info.nFileSizeHigh;
		cur.LowPart  = info.nFileSizeLow;

		if (cur.QuadPart > 0 && cur.QuadPart == prev.QuadPart) {
			if (++same >= 4)
				return;
		} else {
			same = 0;
		}
		prev = cur;
	}
}

// ---------------------------------------------------------------------------
// Chapter parsing  (exiftool -j -QuickTime:ChapterList output)
// ---------------------------------------------------------------------------

struct Chapter {
	double      time_sec;
	std::string name;
};

static double parse_ts(const std::string &s)
{
	int    h = 0, m = 0;
	double sec = 0.0;
	sscanf_s(s.c_str(), "%d:%d:%lf", &h, &m, &sec);
	return h * 3600.0 + m * 60.0 + sec;
}

static std::string trim(std::string s)
{
	auto not_space = [](unsigned char c) { return !std::isspace(c); };
	s.erase(s.begin(),
	        std::find_if(s.begin(), s.end(), not_space));
	s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(),
	        s.end());
	return s;
}

// Parses `ffprobe -v quiet -print_format json -show_chapters` output.
// Each chapter entry: {"start_time": "2.900000", "tags": {"title": "Name"}}
static std::vector<Chapter> parse_chapters(const std::string &json)
{
	std::vector<Chapter> out;

	size_t chap_pos = json.find("\"chapters\"");
	if (chap_pos == std::string::npos)
		return out;

	size_t arr_open = json.find('[', chap_pos);
	if (arr_open == std::string::npos)
		return out;

	size_t pos = arr_open + 1;
	while (pos < json.size()) {
		size_t obj_open = json.find('{', pos);
		if (obj_open == std::string::npos)
			break;

		// Find matching close brace
		int    depth     = 1;
		size_t obj_close = obj_open + 1;
		while (obj_close < json.size() && depth > 0) {
			if (json[obj_close] == '{')
				depth++;
			else if (json[obj_close] == '}')
				depth--;
			obj_close++;
		}
		if (depth != 0)
			break;

		std::string obj = json.substr(obj_open,
		                              obj_close - obj_open);

		// Extract start_time (quoted decimal string)
		double start_sec = 0.0;
		size_t st_pos    = obj.find("\"start_time\"");
		if (st_pos != std::string::npos) {
			size_t colon = obj.find(':', st_pos);
			if (colon != std::string::npos) {
				size_t v = obj.find_first_not_of(
					" \t\r\n\"", colon + 1);
				if (v != std::string::npos)
					start_sec = std::stod(obj.substr(v));
			}
		}

		// Extract title from tags object
		std::string title;
		size_t      tags_pos = obj.find("\"tags\"");
		if (tags_pos != std::string::npos) {
			size_t tit_pos = obj.find("\"title\"", tags_pos);
			if (tit_pos != std::string::npos) {
				size_t colon = obj.find(':', tit_pos);
				if (colon != std::string::npos) {
					size_t q1 = obj.find('"', colon + 1);
					size_t q2 = (q1 != std::string::npos)
							? obj.find('"', q1 + 1)
							: std::string::npos;
					if (q2 != std::string::npos)
						title = obj.substr(q1 + 1,
						                   q2 - q1 - 1);
				}
			}
		}

		if (!title.empty()) {
			Chapter c;
			c.time_sec = start_sec;
			c.name     = title;
			out.push_back(c);
		}

		pos = obj_close;
	}
	return out;
}

// ---------------------------------------------------------------------------
// Get video track timescale via ffprobe
// Premiere Pro interprets xmpDM:startTime ticks in the VIDEO track's timescale
// (denominator of ffprobe's time_base for the video stream, e.g. 60 for 60fps)
// ---------------------------------------------------------------------------

static double get_video_timescale(const std::string &ffprobe,
                                   const std::string &path)
{
	std::string cmd = "\"" + ffprobe +
	                  "\" -v quiet -print_format json"
	                  " -show_streams"
	                  " \"" +
	                  path + "\"";
	std::string out = run_capture(cmd);

	// Find video stream and parse time_base "1/N"
	size_t pos = 0;
	while (pos < out.size()) {
		size_t vpos = out.find("\"video\"", pos);
		if (vpos == std::string::npos)
			break;
		// Look for time_base near this video entry
		size_t tb = out.find("\"time_base\"", vpos);
		if (tb == std::string::npos)
			break;
		// Only accept if time_base precedes the next codec_type entry
		size_t next_ct = out.find("\"codec_type\"", vpos + 1);
		if (next_ct != std::string::npos && tb > next_ct) {
			pos = vpos + 1;
			continue;
		}
		size_t colon = out.find(':', tb);
		if (colon == std::string::npos)
			break;
		// time_base is quoted: "1/N"
		size_t q1 = out.find('"', colon + 1);
		size_t q2 = (q1 != std::string::npos)
				? out.find('"', q1 + 1)
				: std::string::npos;
		if (q2 == std::string::npos)
			break;
		std::string tb_str = out.substr(q1 + 1, q2 - q1 - 1);
		// Parse "1/N" → return N
		size_t slash = tb_str.find('/');
		if (slash != std::string::npos) {
			double denom = std::stod(tb_str.substr(slash + 1));
			if (denom > 0.0)
				return denom;
		}
		break;
	}
	return 30.0; // safe fallback
}

// ---------------------------------------------------------------------------
// Build XMP packet  (Premiere Pro xmpDM:markers)
// ---------------------------------------------------------------------------

static std::string xml_escape(const std::string &s)
{
	std::string r;
	r.reserve(s.size());
	for (char c : s) {
		switch (c) {
		case '<': r += "&lt;"; break;
		case '>': r += "&gt;"; break;
		case '&': r += "&amp;"; break;
		case '"': r += "&quot;"; break;
		case '\'': r += "&apos;"; break;
		default: r += c;
		}
	}
	return r;
}

// Returns a simple UUID-like string (not cryptographic, good enough for XMP guids)
static std::string make_guid(size_t index, double time_sec)
{
	// Deterministic from index + time — avoids needing a real UUID library
	unsigned int a = (unsigned int)(index * 0x9e3779b9u);
	unsigned int b = (unsigned int)(time_sec * 1000.0);
	unsigned int c = (unsigned int)(index * 0x517cc1b7u);
	unsigned int d = (unsigned int)(time_sec * 7919.0);
	char buf[37];
	snprintf(buf, sizeof(buf),
	         "%08x-%04x-%04x-%04x-%08x%04x",
	         a, b & 0xffff, (c >> 16) & 0xffff,
	         d & 0xffff, c, b & 0xffff);
	return buf;
}

static std::string build_xmp(const std::vector<Chapter> &chapters,
                              double                      timescale)
{
	// frameRate string: Premiere uses "f<N>" where N is the timescale
	// e.g. timescale=60 → "f60", timescale=30 → "f30"
	std::ostringstream fr;
	fr << "f" << (long long)timescale;
	std::string frameRate = fr.str();

	std::ostringstream x;
	x << "<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
	     "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"obs-marker-patch\">\n"
	     " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
	     "  <rdf:Description rdf:about=\"\"\n"
	     "    xmlns:xmpDM=\"http://ns.adobe.com/xmp/1.0/DynamicMedia/\"\n"
	     "    xmpDM:startTimeScale=\"" << (long long)timescale << "\"\n"
	     "    xmpDM:startTimeSampleSize=\"1\">\n"
	     "   <xmpDM:Tracks>\n"
	     "    <rdf:Bag>\n"
	     "     <rdf:li>\n"
	     "      <rdf:Description\n"
	     "       xmpDM:trackName=\"Markers\"\n"
	     "       xmpDM:frameRate=\"" << frameRate << "\">\n"
	     "      <xmpDM:markers>\n"
	     "       <rdf:Seq>\n";

	for (size_t i = 0; i < chapters.size(); i++) {
		const auto &ch    = chapters[i];
		if (ch.time_sec == 0.0)
			continue;
		long long   ticks = static_cast<long long>(
			std::round(ch.time_sec * timescale));
		std::string guid    = make_guid(i, ch.time_sec);
		std::string escaped = xml_escape(ch.name);
		x << "        <rdf:li>\n"
		     "         <rdf:Description\n"
		     "          xmpDM:startTime=\"" << ticks << "\"\n"
		     "          xmpDM:type=\"Comment\"\n"
		     "          xmpDM:name=\"" << escaped << "\"\n"
		     "          xmpDM:guid=\"" << guid << "\"/>\n"
		     "        </rdf:li>\n";
	}

	x << "       </rdf:Seq>\n"
	     "      </xmpDM:markers>\n"
	     "      </rdf:Description>\n"
	     "     </rdf:li>\n"
	     "    </rdf:Bag>\n"
	     "   </xmpDM:Tracks>\n"
	     "  </rdf:Description>\n"
	     " </rdf:RDF>\n"
	     "</x:xmpmeta>\n"
	     "<?xpacket end=\"w\"?>\n";

	return x.str();
}

// ---------------------------------------------------------------------------
// Inject XMP in-place
// ---------------------------------------------------------------------------

static bool inject_xmp(const std::string &exiftool,
                        const std::string &mp4_path,
                        const std::string &xmp_content)
{
	char tmp_dir[MAX_PATH];
	GetTempPathA(MAX_PATH, tmp_dir);

	// Per-thread temp file to allow concurrent processing
	std::string tmp_xmp = std::string(tmp_dir) + "obs_mp_" +
	                      std::to_string(GetCurrentThreadId()) + ".xmp";

	{
		std::ofstream f(tmp_xmp, std::ios::binary | std::ios::trunc);
		if (!f)
			return false;
		f << xmp_content;
	}

	std::string cmd = "\"" + exiftool +
	                  "\" -overwrite_original"
	                  " -api LargeFileSupport=1"
	                  " \"-xmp<=" +
	                  tmp_xmp + "\" \"" + mp4_path + "\"";
	int ret = run_wait(cmd);

	DeleteFileA(tmp_xmp.c_str());
	return ret == 0;
}

// ---------------------------------------------------------------------------
// Check if file already carries our XMP (prevents double-injection)
// ---------------------------------------------------------------------------

static bool has_our_xmp(const std::string &exiftool, const std::string &path)
{
	std::string cmd = "\"" + exiftool +
	                  "\" -xmp -b"
	                  " -api LargeFileSupport=1"
	                  " \"" +
	                  path + "\"";
	std::string out = run_capture(cmd);
	return out.find("obs-marker-patch") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Get container and first-audio-stream durations via ffprobe
// ---------------------------------------------------------------------------

static bool get_av_durations(const std::string &ffprobe,
                              const std::string &path,
                              double            &video_dur,
                              double            &audio_dur)
{
	std::string vcmd = "\"" + ffprobe +
	                   "\" -v quiet"
	                   " -show_entries format=duration"
	                   " -of default=noprint_wrappers=1:nokey=1"
	                   " \"" +
	                   path + "\"";
	std::string vout = run_capture(vcmd);

	std::string acmd = "\"" + ffprobe +
	                   "\" -v quiet"
	                   " -select_streams a:0"
	                   " -show_entries stream=duration"
	                   " -of default=noprint_wrappers=1:nokey=1"
	                   " \"" +
	                   path + "\"";
	std::string aout = run_capture(acmd);

	if (vout.empty() || aout.empty())
		return false;
	try {
		video_dur = std::stod(trim(vout));
		audio_dur = std::stod(trim(aout));
	} catch (...) {
		return false;
	}
	return video_dur > 0.0 && audio_dur > 0.0;
}

// ---------------------------------------------------------------------------
// Trim container tail to fix AAC frame-drop A/V gap (~21 ms per dropped frame)
// Only acts when gap is 1 ms – 250 ms.  Lossless: -c copy.
// ---------------------------------------------------------------------------

static bool trim_to_audio(const std::string &ffmpeg,
                           const std::string &ffprobe,
                           const std::string &path)
{
	double video_dur = 0.0, audio_dur = 0.0;
	if (!get_av_durations(ffprobe, path, video_dur, audio_dur))
		return false;

	double gap = video_dur - audio_dur;
	if (gap < 0.001 || gap > 0.250) {
		if (gap > 0.250)
			obs_log(LOG_WARNING,
			        "[obs-marker-patch] Unusual A/V gap %.1f ms in %s"
			        " — not trimming",
			        gap * 1000.0, path.c_str());
		return false;
	}

	obs_log(LOG_INFO,
	        "[obs-marker-patch] A/V gap %.1f ms — trimming video tail: %s",
	        gap * 1000.0, path.c_str());

	std::string tmp = path + ".avtrim.mp4";
	char        dur_str[32];
	snprintf(dur_str, sizeof(dur_str), "%.6f", audio_dur);

	std::string cmd = "\"" + ffmpeg + "\" -y -i \"" + path + "\""
	                  " -t " +
	                  dur_str + " -c copy \"" + tmp + "\"";
	int ret = run_wait(cmd);
	if (ret != 0) {
		obs_log(LOG_ERROR,
		        "[obs-marker-patch] Trim failed (code %d): %s", ret,
		        path.c_str());
		DeleteFileA(tmp.c_str());
		return false;
	}

	DeleteFileA(path.c_str());
	if (!MoveFileA(tmp.c_str(), path.c_str())) {
		obs_log(LOG_ERROR,
		        "[obs-marker-patch] MoveFile failed after trim: %s",
		        path.c_str());
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Remux MKV → MP4 via bundled ffmpeg
// Returns output .mp4 path, or empty on failure
// ---------------------------------------------------------------------------

static std::string remux_mkv(const std::string &mkv_path)
{
	std::string ffmpeg = find_ffmpeg();
	if (ffmpeg.empty()) {
		obs_log(LOG_WARNING,
		        "[obs-marker-patch] ffmpeg not found, cannot remux %s",
		        mkv_path.c_str());
		return "";
	}

	std::string mp4_path =
		mkv_path.substr(0, mkv_path.size() - 4) + ".mp4";

	std::string cmd = "\"" + ffmpeg + "\" -y -i \"" + mkv_path +
	                  "\" -c copy \"" + mp4_path + "\"";

	obs_log(LOG_INFO, "[obs-marker-patch] Remuxing: %s",
	        mkv_path.c_str());
	int ret = run_wait(cmd);
	if (ret != 0) {
		obs_log(LOG_ERROR,
		        "[obs-marker-patch] ffmpeg failed (code %d): %s",
		        ret, mkv_path.c_str());
		return "";
	}
	return mp4_path;
}

// ---------------------------------------------------------------------------
// patch_mp4: shared pipeline — optional A/V trim + optional XMP injection
// ---------------------------------------------------------------------------

static void patch_mp4(const std::string &path,
                      bool               inject_markers = true,
                      bool               av_trim        = true)
{
	// ── A/V trim (lossless, idempotent — skips if gap < 1 ms) ────────────
	if (av_trim) {
		std::string fp = find_ffprobe();
		std::string fm = find_ffmpeg();
		if (!fp.empty() && !fm.empty())
			trim_to_audio(fm, fp, path);
	}

	if (!inject_markers)
		return;

	// ── Marker injection ──────────────────────────────────────────────────
	std::string exiftool = find_exiftool();
	if (exiftool.empty()) {
		obs_log(LOG_WARNING,
		        "[obs-marker-patch] exiftool not found. "
		        "Install via: winget install OliverBetz.ExifTool");
		return;
	}

	std::string ffprobe = find_ffprobe();
	if (ffprobe.empty()) {
		obs_log(LOG_WARNING,
		        "[obs-marker-patch] ffprobe not found. "
		        "Install via: winget install Gyan.FFmpeg");
		return;
	}

	// Skip files already carrying our XMP
	if (has_our_xmp(exiftool, path)) {
		obs_log(LOG_INFO,
		        "[obs-marker-patch] Already patched, skipping: %s",
		        path.c_str());
		return;
	}

	std::string cmd = "\"" + ffprobe +
	                  "\" -v quiet -print_format json"
	                  " -show_chapters"
	                  " \"" +
	                  path + "\"";
	std::string json     = run_capture(cmd);
	auto        chapters = parse_chapters(json);

	if (chapters.empty()) {
		obs_log(LOG_INFO,
		        "[obs-marker-patch] No chapters in %s — skipping",
		        path.c_str());
		return;
	}

	double      ts  = get_video_timescale(ffprobe, path);
	std::string xmp = build_xmp(chapters, ts);

	if (inject_xmp(exiftool, path, xmp)) {
		obs_log(LOG_INFO,
		        "[obs-marker-patch] Injected %zu XMP marker(s) "
		        "(timescale=%.0f) into %s",
		        chapters.size(), ts, path.c_str());
	} else {
		obs_log(LOG_ERROR,
		        "[obs-marker-patch] XMP injection failed for %s",
		        path.c_str());
	}
}

// ---------------------------------------------------------------------------
// Mode A: recording-stopped — wait for remux, patch MP4
// ---------------------------------------------------------------------------

static void patch_recording(std::string path)
{
	obs_log(LOG_INFO, "[obs-marker-patch] Monitoring: %s", path.c_str());
	wait_for_stable(path, 60);
	patch_mp4(path, s_auto_markers.load(), s_auto_trim.load());
}

static std::string get_recording_path()
{
	obs_output_t *rec = obs_frontend_get_recording_output();
	if (!rec)
		return "";

	obs_data_t *settings = obs_output_get_settings(rec);
	const char *url      = obs_data_get_string(settings, "url");
	const char *path     = obs_data_get_string(settings, "path");
	std::string file     = (url && *url) ? url : (path ? path : "");
	obs_data_release(settings);
	obs_output_release(rec);
	return file;
}

// ---------------------------------------------------------------------------
// Mode B: startup scan — remux orphaned MKVs + inject XMP
// rec_folder is collected on the main thread before spawning
// ---------------------------------------------------------------------------

static void startup_scan(const std::string &folder)
{
	obs_log(LOG_INFO, "[obs-marker-patch] Startup scan: %s",
	        folder.c_str());

	// ── Remux orphaned MKVs (no matching MP4) and patch them ─────────────
	{
		WIN32_FIND_DATAA         fd;
		std::vector<std::string> orphans;
		HANDLE hf = FindFirstFileA((folder + "\\*.mkv").c_str(), &fd);
		if (hf != INVALID_HANDLE_VALUE) {
			do {
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;
				std::string name = fd.cFileName;
				std::string mp4f =
					folder + "\\" +
					name.substr(0, name.size() - 4) + ".mp4";
				if (GetFileAttributesA(mp4f.c_str()) ==
				    INVALID_FILE_ATTRIBUTES)
					orphans.push_back(folder + "\\" + name);
			} while (FindNextFileA(hf, &fd));
			FindClose(hf);
		}
		if (!orphans.empty()) {
			obs_log(LOG_INFO,
			        "[obs-marker-patch] Startup scan: "
			        "%zu orphaned MKV(s) found",
			        orphans.size());
			for (const auto &mkv : orphans) {
				std::string mp4 = remux_mkv(mkv);
				if (!mp4.empty())
					patch_mp4(mp4, true, true);
			}
		}
	}

	// ── A/V trim check on all MP4s in the folder (fast, idempotent) ──────
	{
		std::string ffprobe_s = find_ffprobe();
		std::string ffmpeg_s  = find_ffmpeg();
		if (!ffprobe_s.empty() && !ffmpeg_s.empty()) {
			WIN32_FIND_DATAA         fd;
			std::vector<std::string> mp4s;
			HANDLE hf = FindFirstFileA((folder + "\\*.mp4").c_str(),
			                           &fd);
			if (hf != INVALID_HANDLE_VALUE) {
				do {
					if (!(fd.dwFileAttributes &
					      FILE_ATTRIBUTE_DIRECTORY))
						mp4s.push_back(folder + "\\" +
						               fd.cFileName);
				} while (FindNextFileA(hf, &fd));
				FindClose(hf);
			}
			int trimmed = 0;
			for (const auto &mp4 : mp4s) {
				if (trim_to_audio(ffmpeg_s, ffprobe_s, mp4))
					trimmed++;
			}
			if (trimmed > 0)
				obs_log(LOG_INFO,
				        "[obs-marker-patch] Startup scan: "
				        "trimmed %d file(s)",
				        trimmed);
		}
	}

	obs_log(LOG_INFO, "[obs-marker-patch] Startup scan complete");
}

// ---------------------------------------------------------------------------
// Recursive folder scan — collects all files matching ext (e.g. ".mp4")
// ---------------------------------------------------------------------------

static void scan_recursive(const std::string &folder, const std::string &ext,
                            std::vector<std::string> &out)
{
	WIN32_FIND_DATAA fd;

	// Files matching extension in this directory
	HANDLE hf = FindFirstFileA((folder + "\\*" + ext).c_str(), &fd);
	if (hf != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				out.push_back(folder + "\\" + fd.cFileName);
		} while (FindNextFileA(hf, &fd));
		FindClose(hf);
	}

	// Recurse into subdirectories
	HANDLE hd = FindFirstFileA((folder + "\\*").c_str(), &fd);
	if (hd != INVALID_HANDLE_VALUE) {
		do {
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
			    strcmp(fd.cFileName, ".") != 0 &&
			    strcmp(fd.cFileName, "..") != 0)
				scan_recursive(folder + "\\" + fd.cFileName,
				               ext, out);
		} while (FindNextFileA(hd, &fd));
		FindClose(hd);
	}
}

// ---------------------------------------------------------------------------
// Mode C: manual folder fix
// ---------------------------------------------------------------------------

static void fix_folder_markers_worker(std::string folder)
{
	obs_log(LOG_INFO, "[obs-marker-patch] Fix folder (markers): %s",
	        folder.c_str());

	std::vector<std::string> mp4s, mkvs;
	scan_recursive(folder, ".mp4", mp4s);
	scan_recursive(folder, ".mkv", mkvs);

	for (const auto &f : mp4s)
		patch_mp4(f, true, false);
	for (const auto &mkv : mkvs) {
		std::string mp4 = remux_mkv(mkv);
		if (!mp4.empty())
			patch_mp4(mp4, true, false);
	}

	obs_log(LOG_INFO,
	        "[obs-marker-patch] Fix folder (markers) done: "
	        "%zu MP4 + %zu MKV",
	        mp4s.size(), mkvs.size());
}

static void fix_file_markers_worker(std::string path)
{
	obs_log(LOG_INFO, "[obs-marker-patch] Fix file (markers): %s",
	        path.c_str());
	patch_mp4(path, true, false);
}

static void fix_folder_trim_worker(std::string folder)
{
	obs_log(LOG_INFO, "[obs-marker-patch] Fix folder (A/V trim): %s",
	        folder.c_str());

	std::vector<std::string> mp4s;
	scan_recursive(folder, ".mp4", mp4s);

	for (const auto &f : mp4s)
		patch_mp4(f, false, true);

	obs_log(LOG_INFO,
	        "[obs-marker-patch] Fix folder (A/V trim) done: "
	        "%zu file(s)",
	        mp4s.size());
}

static void fix_file_trim_worker(std::string path)
{
	obs_log(LOG_INFO, "[obs-marker-patch] Fix file (A/V trim): %s",
	        path.c_str());
	patch_mp4(path, false, true);
}

// IFileOpenDialog folder picker — runs on OBS main thread (COM already init'd)
static std::string pick_folder()
{
	std::string      result;
	IFileOpenDialog *pfd = nullptr;

	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL,
	                               CLSCTX_INPROC_SERVER,
	                               IID_PPV_ARGS(&pfd));
	if (FAILED(hr))
		return result;

	DWORD opts = 0;
	pfd->GetOptions(&opts);
	pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST |
	                FOS_FORCEFILESYSTEM);
	pfd->SetTitle(L"Select folder");

	hr = pfd->Show(NULL);
	if (SUCCEEDED(hr)) {
		IShellItem *item = nullptr;
		hr = pfd->GetResult(&item);
		if (SUCCEEDED(hr)) {
			PWSTR wpath = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,
			                                   &wpath))) {
				int n = WideCharToMultiByte(
					CP_UTF8, 0, wpath, -1, NULL, 0, NULL,
					NULL);
				if (n > 0) {
					std::vector<char> buf(n);
					WideCharToMultiByte(CP_UTF8, 0, wpath,
					                    -1, buf.data(), n,
					                    NULL, NULL);
					result = buf.data();
				}
				CoTaskMemFree(wpath);
			}
			item->Release();
		}
	}
	pfd->Release();
	return result;
}

// IFileOpenDialog single-file picker (MP4)
static std::string pick_file()
{
	std::string      result;
	IFileOpenDialog *pfd = nullptr;

	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL,
	                               CLSCTX_INPROC_SERVER,
	                               IID_PPV_ARGS(&pfd));
	if (FAILED(hr))
		return result;

	COMDLG_FILTERSPEC filter = {L"MP4 Files (*.mp4)", L"*.mp4"};
	pfd->SetFileTypes(1, &filter);
	pfd->SetFileTypeIndex(1);

	DWORD opts = 0;
	pfd->GetOptions(&opts);
	pfd->SetOptions((opts & ~FOS_PICKFOLDERS) | FOS_PATHMUSTEXIST |
	                FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
	pfd->SetTitle(L"Select MP4 file");

	hr = pfd->Show(NULL);
	if (SUCCEEDED(hr)) {
		IShellItem *item = nullptr;
		hr               = pfd->GetResult(&item);
		if (SUCCEEDED(hr)) {
			PWSTR wpath = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,
			                                   &wpath))) {
				int n = WideCharToMultiByte(
					CP_UTF8, 0, wpath, -1, NULL, 0, NULL,
					NULL);
				if (n > 0) {
					std::vector<char> buf(n);
					WideCharToMultiByte(CP_UTF8, 0, wpath,
					                    -1, buf.data(), n,
					                    NULL, NULL);
					result = buf.data();
				}
				CoTaskMemFree(wpath);
			}
			item->Release();
		}
	}
	pfd->Release();
	return result;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void mp_start(void)
{
	obs_log(LOG_INFO, "[obs-marker-patch] started");
}

void mp_stop(void)
{
	obs_log(LOG_INFO, "[obs-marker-patch] stopped");
}

int mp_get_auto_markers(void)
{
	return s_auto_markers.load() ? 1 : 0;
}

int mp_get_auto_trim(void)
{
	return s_auto_trim.load() ? 1 : 0;
}

void mp_set_auto_markers(int on)
{
	s_auto_markers.store(on != 0);
	obs_log(LOG_INFO, "[obs-marker-patch] Auto-markers: %s",
	        on ? "ON" : "OFF");
}

void mp_set_auto_trim(int on)
{
	s_auto_trim.store(on != 0);
	obs_log(LOG_INFO, "[obs-marker-patch] Auto-trim: %s",
	        on ? "ON" : "OFF");
}

void mp_on_recording_stopped(void)
{
	std::string path = get_recording_path();
	if (path.empty()) {
		obs_log(LOG_WARNING,
		        "[obs-marker-patch] Could not determine recording path");
		return;
	}

	auto ends_mp4 = [](const std::string &s) {
		return s.size() >= 4 &&
		       _stricmp(s.c_str() + s.size() - 4, ".mp4") == 0;
	};

	if (!ends_mp4(path)) {
		obs_log(LOG_INFO,
		        "[obs-marker-patch] Not an MP4 (%s), skipping",
		        path.c_str());
		return;
	}

	std::thread([path]() { patch_recording(path); }).detach();
}

void mp_on_obs_loaded(void)
{
	// Collect profile config on the main thread before spawning
	std::string rec_folder;

	config_t *profile = obs_frontend_get_profile_config();
	if (profile) {
		const char *fmt =
			config_get_string(profile, "AdvOut", "RecFormat2");
		if (fmt && _stricmp(fmt, "hybrid_mp4") == 0) {
			const char *p = config_get_string(profile, "AdvOut",
			                                   "RecFilePath");
			if (p && *p)
				rec_folder = p;
		}
	}

	if (rec_folder.empty())
		return;

	std::thread([rec_folder]() {
		Sleep(2000);
		startup_scan(rec_folder);
	}).detach();
}

void mp_fix_folder_markers(void)
{
	std::string folder = pick_folder();
	if (folder.empty())
		return;
	std::thread([folder]() { fix_folder_markers_worker(folder); }).detach();
}

void mp_fix_file_markers(void)
{
	std::string path = pick_file();
	if (path.empty())
		return;
	std::thread([path]() { fix_file_markers_worker(path); }).detach();
}

void mp_fix_folder_trim(void)
{
	std::string folder = pick_folder();
	if (folder.empty())
		return;
	std::thread([folder]() { fix_folder_trim_worker(folder); }).detach();
}

void mp_fix_file_trim(void)
{
	std::string path = pick_file();
	if (path.empty())
		return;
	std::thread([path]() { fix_file_trim_worker(path); }).detach();
}


