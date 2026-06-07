/*
 * obs-marker-patch  —  marker-patch.cpp
 *
 * On OBS_FRONTEND_EVENT_RECORDING_STOPPED:
 *   1. Get MP4 path from the recording output
 *   2. Wait for hybrid-MP4 remux to finish (file size stable)
 *   3. Read QuickTime chapter atoms via exiftool
 *   4. Build Premiere Pro-compatible XMP packet
 *   5. Inject XMP in-place with exiftool (-overwrite_original)
 *
 * Requires: exiftool  (winget install OliverBetz.ExifTool)
 */

#include "marker-patch.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>
#include <vector>
#include <thread>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cctype>

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
// Wait for file to stabilise (remux done)
// ---------------------------------------------------------------------------

static void wait_for_stable(const std::string &path, int timeout_sec)
{
	LARGE_INTEGER prev  = {};
	int           same  = 0;
	int           ticks = 0;
	const int     max   = timeout_sec * 2; // 500 ms steps

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
			if (++same >= 4) // 2 seconds identical → done
				return;
		} else {
			same = 0;
		}
		prev = cur;
	}
}

// ---------------------------------------------------------------------------
// Chapter parsing  (exiftool -j -QuickTime:ChapterList output)
//
// exiftool emits ChapterList as a JSON array of strings:
//   "ChapterList": ["00:00:00 Intro", "00:05:30 Scene 2"]
// Each entry: "HH:MM:SS[.sss] Name"
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

static std::vector<Chapter> parse_chapters(const std::string &json)
{
	std::vector<Chapter> out;

	size_t key = json.find("ChapterList");
	if (key == std::string::npos)
		return out;

	size_t arr_open  = json.find('[', key);
	size_t arr_close = json.find(']', arr_open);
	if (arr_open == std::string::npos || arr_close == std::string::npos)
		return out;

	std::string arr = json.substr(arr_open + 1,
	                              arr_close - arr_open - 1);
	size_t pos = 0;
	while (pos < arr.size()) {
		size_t q1 = arr.find('"', pos);
		if (q1 == std::string::npos)
			break;
		size_t q2 = arr.find('"', q1 + 1);
		if (q2 == std::string::npos)
			break;

		std::string entry = arr.substr(q1 + 1, q2 - q1 - 1);
		size_t      sp    = entry.find(' ');

		Chapter ch;
		if (sp != std::string::npos) {
			ch.time_sec = parse_ts(entry.substr(0, sp));
			ch.name     = trim(entry.substr(sp + 1));
		} else {
			ch.time_sec = parse_ts(entry);
			ch.name = "Chapter " + std::to_string(out.size() + 1);
		}
		if (ch.time_sec >= 0.0)
			out.push_back(ch);

		pos = q2 + 1;
	}
	return out;
}

// ---------------------------------------------------------------------------
// Get MP4 video timescale  (usually 90000 for standard MP4)
// ---------------------------------------------------------------------------

static double get_timescale(const std::string &exiftool,
                             const std::string &path)
{
	std::string cmd = "\"" + exiftool +
	                  "\" -j -n -api LargeFileSupport=1"
	                  " -QuickTime:TimeScale"
	                  " \"" +
	                  path + "\"";
	std::string out = run_capture(cmd);

	size_t pos = out.find("TimeScale");
	if (pos != std::string::npos) {
		size_t colon = out.find(':', pos);
		if (colon != std::string::npos) {
			double ts = std::stod(out.c_str() + colon + 1);
			if (ts > 0.0)
				return ts;
		}
	}
	return 90000.0; // safe default
}

// ---------------------------------------------------------------------------
// Build XMP packet
//
// Premiere Pro reads xmpDM:markers with startTime as integer ticks
// at the file's native timescale (usually 90000 Hz for MP4).
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

static std::string build_xmp(const std::vector<Chapter> &chapters,
                              double                      timescale)
{
	std::ostringstream x;

	// XMP packet header
	x << "<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
	     "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"\n"
	     "           x:xmptk=\"obs-marker-patch\">\n"
	     " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
	     "  <rdf:Description rdf:about=\"\"\n"
	     "    xmlns:xmpDM=\"http://ns.adobe.com/xmp/1.0/DynamicMedia/\">\n"
	     "   <xmpDM:markers>\n"
	     "    <rdf:Seq>\n";

	for (const auto &ch : chapters) {
		long long ticks =
			static_cast<long long>(std::round(ch.time_sec * timescale));
		x << "     <rdf:li rdf:parseType=\"Resource\">\n"
		     "      <xmpDM:startTime>"
		  << ticks
		  << "</xmpDM:startTime>\n"
		     "      <xmpDM:duration>0</xmpDM:duration>\n"
		     "      <xmpDM:markerType>Chapter</xmpDM:markerType>\n"
		     "      <xmpDM:name>"
		  << xml_escape(ch.name)
		  << "</xmpDM:name>\n"
		     "     </rdf:li>\n";
	}

	x << "    </rdf:Seq>\n"
	     "   </xmpDM:markers>\n"
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
	// Write XMP to a temp file
	char tmp_dir[MAX_PATH];
	GetTempPathA(MAX_PATH, tmp_dir);
	std::string tmp_xmp =
		std::string(tmp_dir) + "obs_marker_patch_tmp.xmp";

	{
		std::ofstream f(tmp_xmp, std::ios::binary);
		if (!f)
			return false;
		f << xmp_content;
	}

	// exiftool: clear existing XMP, write ours from temp file
	// -xmp<= reads XMP from a file and embeds it
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
// Core patch logic  (runs in background thread)
// ---------------------------------------------------------------------------

static void patch_recording(std::string path)
{
	obs_log(LOG_INFO, "[obs-marker-patch] Monitoring: %s", path.c_str());

	// Hybrid MP4 remuxes after stop — wait for the file to stabilise
	wait_for_stable(path, 60);

	std::string exiftool = find_exiftool();
	if (exiftool.empty()) {
		obs_log(LOG_WARNING,
		        "[obs-marker-patch] exiftool not found. "
		        "Install via: winget install OliverBetz.ExifTool");
		return;
	}

	// Read chapters from MP4
	std::string cmd = "\"" + exiftool +
	                  "\" -j -api LargeFileSupport=1"
	                  " -QuickTime:ChapterList"
	                  " \"" +
	                  path + "\"";
	std::string json     = run_capture(cmd);
	auto        chapters = parse_chapters(json);

	if (chapters.empty()) {
		obs_log(LOG_INFO,
		        "[obs-marker-patch] No chapters in %s — nothing to do",
		        path.c_str());
		return;
	}

	obs_log(LOG_INFO, "[obs-marker-patch] Found %zu chapter(s)",
	        chapters.size());

	double      ts  = get_timescale(exiftool, path);
	std::string xmp = build_xmp(chapters, ts);

	if (inject_xmp(exiftool, path, xmp)) {
		obs_log(LOG_INFO,
		        "[obs-marker-patch] Injected %zu XMP marker(s) "
		        "(timescale=%.0f) into %s",
		        chapters.size(), ts, path.c_str());
	} else {
		obs_log(LOG_ERROR,
		        "[obs-marker-patch] exiftool injection failed for %s",
		        path.c_str());
	}
}

// ---------------------------------------------------------------------------
// Get MP4 path from the active recording output
// ---------------------------------------------------------------------------

static std::string get_recording_path()
{
	obs_output_t *rec = obs_frontend_get_recording_output();
	if (!rec)
		return "";

	obs_data_t *settings = obs_output_get_settings(rec);

	// ffmpeg_muxer uses "url"; simple output uses "path"
	const char *url  = obs_data_get_string(settings, "url");
	const char *path = obs_data_get_string(settings, "path");
	std::string file = (url && *url) ? url : (path ? path : "");

	obs_data_release(settings);
	obs_output_release(rec);
	return file;
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

void mp_on_recording_stopped(void)
{
	std::string path = get_recording_path();

	if (path.empty()) {
		obs_log(LOG_WARNING,
		        "[obs-marker-patch] Could not determine recording path");
		return;
	}

	// Only process MP4 files (hybrid_mp4 output)
	auto ends_with_mp4 = [](const std::string &s) {
		return s.size() >= 4 &&
		       _stricmp(s.c_str() + s.size() - 4, ".mp4") == 0;
	};

	if (!ends_with_mp4(path)) {
		obs_log(LOG_INFO,
		        "[obs-marker-patch] Not an MP4 (%s), skipping",
		        path.c_str());
		return;
	}

	// Detach so we don't block the OBS event thread
	std::thread([path]() { patch_recording(path); }).detach();
}
