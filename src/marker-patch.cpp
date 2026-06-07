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

	// Derive obs root from plugin DLL path:
	// ...\obs-studio\obs-plugins\64bit\obs-marker-patch.dll
	// →  ...\obs-studio\bin\64bit\ffmpeg.exe
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
					std::string root = p.substr(0, c);
					std::string cand =
						root +
						"\\bin\\64bit\\ffmpeg.exe";
					if (GetFileAttributesA(cand.c_str()) !=
					    INVALID_FILE_ATTRIBUTES) {
						s_ffmpeg_cache = cand;
						return s_ffmpeg_cache;
					}
				}
			}
		}
	}

	// Fallback: PATH
	std::string v = run_capture("ffmpeg -version 2>&1");
	if (v.find("ffmpeg version") != std::string::npos) {
		s_ffmpeg_cache = "ffmpeg";
		return s_ffmpeg_cache;
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
// Get MP4 video timescale
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
	return 90000.0;
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

static std::string build_xmp(const std::vector<Chapter> &chapters,
                              double                      timescale)
{
	std::ostringstream x;
	x << "<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
	     "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"\n"
	     "           x:xmptk=\"obs-marker-patch\">\n"
	     " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
	     "  <rdf:Description rdf:about=\"\"\n"
	     "    xmlns:xmpDM=\"http://ns.adobe.com/xmp/1.0/DynamicMedia/\">\n"
	     "   <xmpDM:markers>\n"
	     "    <rdf:Seq>\n";

	for (const auto &ch : chapters) {
		long long ticks = static_cast<long long>(
			std::round(ch.time_sec * timescale));
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
// patch_mp4: shared pipeline — read chapters from MP4, inject XMP
// ---------------------------------------------------------------------------

static void patch_mp4(const std::string &path)
{
	std::string exiftool = find_exiftool();
	if (exiftool.empty()) {
		obs_log(LOG_WARNING,
		        "[obs-marker-patch] exiftool not found. "
		        "Install via: winget install OliverBetz.ExifTool");
		return;
	}

	std::string cmd = "\"" + exiftool +
	                  "\" -j -api LargeFileSupport=1"
	                  " -QuickTime:ChapterList"
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

	double      ts  = get_timescale(exiftool, path);
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
	patch_mp4(path);
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

	std::string      pattern = folder + "\\*.mkv";
	WIN32_FIND_DATAA fd;
	HANDLE           hf = FindFirstFileA(pattern.c_str(), &fd);
	if (hf == INVALID_HANDLE_VALUE) {
		obs_log(LOG_INFO,
		        "[obs-marker-patch] Startup scan: no MKVs found");
		return;
	}

	std::vector<std::string> orphans;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		std::string mkv_name = fd.cFileName;
		std::string mp4_name =
			mkv_name.substr(0, mkv_name.size() - 4) + ".mp4";
		std::string mp4_full = folder + "\\" + mp4_name;
		if (GetFileAttributesA(mp4_full.c_str()) ==
		    INVALID_FILE_ATTRIBUTES)
			orphans.push_back(folder + "\\" + mkv_name);
	} while (FindNextFileA(hf, &fd));
	FindClose(hf);

	if (orphans.empty()) {
		obs_log(LOG_INFO,
		        "[obs-marker-patch] Startup scan: no orphaned MKVs");
		return;
	}

	obs_log(LOG_INFO,
	        "[obs-marker-patch] Startup scan: %zu orphaned MKV(s) found",
	        orphans.size());
	for (const auto &mkv : orphans) {
		std::string mp4 = remux_mkv(mkv);
		if (!mp4.empty())
			patch_mp4(mp4);
	}
	obs_log(LOG_INFO, "[obs-marker-patch] Startup scan complete");
}

// ---------------------------------------------------------------------------
// Mode C: manual folder fix
// ---------------------------------------------------------------------------

static void fix_folder_worker(std::string folder)
{
	obs_log(LOG_INFO, "[obs-marker-patch] Fix folder: %s",
	        folder.c_str());
	int processed = 0;

	// MP4 files: inject XMP from existing chapter atoms
	{
		std::string      pat = folder + "\\*.mp4";
		WIN32_FIND_DATAA fd;
		HANDLE           hf = FindFirstFileA(pat.c_str(), &fd);
		if (hf != INVALID_HANDLE_VALUE) {
			do {
				if (fd.dwFileAttributes &
				    FILE_ATTRIBUTE_DIRECTORY)
					continue;
				patch_mp4(folder + "\\" + fd.cFileName);
				processed++;
			} while (FindNextFileA(hf, &fd));
			FindClose(hf);
		}
	}

	// MKV files: remux → MP4 → inject XMP
	{
		std::string      pat = folder + "\\*.mkv";
		WIN32_FIND_DATAA fd;
		HANDLE           hf = FindFirstFileA(pat.c_str(), &fd);
		if (hf != INVALID_HANDLE_VALUE) {
			do {
				if (fd.dwFileAttributes &
				    FILE_ATTRIBUTE_DIRECTORY)
					continue;
				std::string mkv = folder + "\\" + fd.cFileName;
				std::string mp4 = remux_mkv(mkv);
				if (!mp4.empty()) {
					patch_mp4(mp4);
					processed++;
				}
			} while (FindNextFileA(hf, &fd));
			FindClose(hf);
		}
	}

	obs_log(LOG_INFO,
	        "[obs-marker-patch] Fix folder done: %d file(s) processed",
	        processed);
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
	pfd->SetTitle(L"Select folder to patch markers");

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

void mp_open_fix_folder_dialog(void)
{
	std::string folder = pick_folder();
	if (folder.empty())
		return;

	std::thread([folder]() { fix_folder_worker(folder); }).detach();
}

void mp_install_exiftool(void)
{
	// Check if already installed
	std::string ver = run_capture("exiftool -ver");
	if (!ver.empty() && ver[0] >= '1' && ver[0] <= '9') {
		std::string msg = "exiftool is already installed (version " +
		                  ver.substr(0, ver.find('\n')) + ")";
		MessageBoxA(NULL, msg.c_str(), "Marker Patch", MB_OK | MB_ICONINFORMATION);
		return;
	}

	// winget not guaranteed — check for it
	std::string wg = run_capture("winget --version");
	if (wg.empty() || wg.find("v") == std::string::npos) {
		MessageBoxA(NULL,
		            "winget (App Installer) not found.\n"
		            "Install exiftool manually:\n"
		            "  https://exiftool.org\n"
		            "and place exiftool.exe in C:\\Windows\\",
		            "Marker Patch", MB_OK | MB_ICONWARNING);
		return;
	}

	int choice = MessageBoxA(NULL,
	                          "exiftool is not installed.\n\n"
	                          "Install it now via winget?\n"
	                          "(A console window will open briefly.)",
	                          "Marker Patch - Install exiftool",
	                          MB_YESNO | MB_ICONQUESTION);
	if (choice != IDYES)
		return;

	// Launch winget in a visible console so the user can see progress
	STARTUPINFOA si   = {};
	si.cb             = sizeof(si);
	PROCESS_INFORMATION pi = {};

	char cmd[] = "winget install --id OliverBetz.ExifTool -e --accept-package-agreements --accept-source-agreements";
	if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE,
	                    NULL, NULL, &si, &pi)) {
		MessageBoxA(NULL, "Failed to launch winget.",
		            "Marker Patch", MB_OK | MB_ICONERROR);
		return;
	}

	// Wait for install to finish then check again
	WaitForSingleObject(pi.hProcess, 120000);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	// Bust the cache so find_exiftool re-probes
	s_exiftool_cache.clear();

	std::string ver2 = find_exiftool();
	if (!ver2.empty()) {
		MessageBoxA(NULL, "exiftool installed successfully!",
		            "Marker Patch", MB_OK | MB_ICONINFORMATION);
	} else {
		MessageBoxA(NULL,
		            "Installation may have completed but exiftool was\n"
		            "not found in PATH. You may need to restart OBS.",
		            "Marker Patch", MB_OK | MB_ICONWARNING);
	}
}

