/*
 * obs-premiere-patch  --  marker-patch.cpp
 *
 * Three operation modes:
 *   A. Recording stopped  -> wait for hybrid-MP4 remux, inject XMP
 *   B. Startup scan       -> remux orphaned MKVs + inject XMP
 *   C. Manual folder fix  -> Tools menu -> folder picker -> process all MP4/MKV
 *
 * Standalone: uses OBS-bundled libavformat/avcodec/avutil directly.
 * No external tools required (no exiftool, ffmpeg, ffprobe binaries).
 */

#include "marker-patch.h"
#include "av_utils.h"
#include "xmp_box.h"

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
// String utilities
// ---------------------------------------------------------------------------

static std::string xml_escape(const std::string &s)
{
std::string r;
r.reserve(s.size());
for (char c : s) {
switch (c) {
case '<':  r += "&lt;";   break;
case '>':  r += "&gt;";   break;
case '&':  r += "&amp;";  break;
case '"':  r += "&quot;"; break;
case '\'': r += "&apos;"; break;
default:   r += c;
}
}
return r;
}

// Deterministic GUID-like string (no UUID library dependency)
static std::string make_guid(size_t index, double time_sec)
{
unsigned int a = (unsigned int)(index * 0x9e3779b9u);
unsigned int b = (unsigned int)(time_sec * 1000.0);
unsigned int c = (unsigned int)(index * 0x517cc1b7u);
unsigned int d = (unsigned int)(time_sec * 7919.0);
char         buf[37];
snprintf(buf, sizeof(buf),
         "%08x-%04x-%04x-%04x-%08x%04x",
         a, b & 0xffff, (c >> 16) & 0xffff,
         d & 0xffff, c, b & 0xffff);
return buf;
}

// ---------------------------------------------------------------------------
// Build XMP packet (Premiere Pro xmpDM:markers)
// ---------------------------------------------------------------------------

static std::string build_xmp(const std::vector<AvChapter> &chapters,
                              double                        timescale)
{
std::ostringstream fr;
fr << "f" << (long long)timescale;
std::string frameRate = fr.str();

std::ostringstream x;
x << "<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
     "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"obs-premiere-patch\">\n"
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
const auto &ch = chapters[i];
if (ch.time_sec == 0.0)
continue; // skip t=0 "Start" marker
long long   ticks   = static_cast<long long>(
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
// Shared pipeline -- optional A/V trim + optional XMP injection
// ---------------------------------------------------------------------------

static void patch_mp4(const std::string &path,
                      bool               inject_markers = true,
                      bool               do_av_trim     = true)
{
// Read inline status — skip work already done in a previous run.
uint8_t trim_st    = OPP_STATUS_NONE;
uint8_t markers_st = OPP_STATUS_NONE;
xmp_read_status(path, &trim_st, &markers_st);

// A/V trim
if (do_av_trim && trim_st != OPP_STATUS_DONE) {
xmp_write_status(path, OPP_STATUS_PATCHING, markers_st);
if (xmp_fix_tkhd_durations(path)) {
obs_log(LOG_INFO,
        "[obs-premiere-patch] Duration fixed: %s",
        path.c_str());
trim_st = OPP_STATUS_DONE;
xmp_write_status(path, trim_st, markers_st);
}
}

if (!inject_markers)
return;

// Markers: status box wins; fall back to XMP scan for old files.
if (markers_st == OPP_STATUS_DONE) {
obs_log(LOG_INFO,
        "[obs-premiere-patch] Markers already done (status), skipping: %s",
        path.c_str());
return;
}
if (xmp_has_ours(path)) {
obs_log(LOG_INFO,
        "[obs-premiere-patch] Already patched (XMP), skipping: %s",
        path.c_str());
return;
}

auto chapters = av_get_chapters(path);
if (chapters.empty()) {
obs_log(LOG_INFO,
        "[obs-premiere-patch] No chapters in %s -- skipping",
        path.c_str());
return;
}

double      ts  = av_get_video_timescale(path);
std::string xmp = build_xmp(chapters, ts);

xmp_write_status(path, trim_st, OPP_STATUS_PATCHING);
if (xmp_inject(path, xmp)) {
obs_log(LOG_INFO,
        "[obs-premiere-patch] Injected %zu XMP marker(s) "
        "(timescale=%.0f) into %s",
        chapters.size(), ts, path.c_str());
xmp_write_status(path, trim_st, OPP_STATUS_DONE);
} else {
obs_log(LOG_ERROR,
        "[obs-premiere-patch] XMP injection failed for %s",
        path.c_str());
}
}

// ---------------------------------------------------------------------------
// Mode A: recording-stopped -- wait for remux, patch MP4
// ---------------------------------------------------------------------------

static void patch_recording(std::string path)
{
obs_log(LOG_INFO, "[obs-premiere-patch] Monitoring: %s", path.c_str());
wait_for_stable(path, 60);
// ADS already stamped {00,00} at recording start.
// Now that moov exists, write_status also injects OBPS into moov.
xmp_write_status(path, OPP_STATUS_NONE, OPP_STATUS_NONE);
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
// Mode B: startup scan -- remux orphaned MKVs + trim all MP4s
// ---------------------------------------------------------------------------

static void startup_scan(const std::string &folder)
{
obs_log(LOG_INFO, "[obs-premiere-patch] Startup scan: %s",
        folder.c_str());

// Remux orphaned MKVs (no matching MP4) and patch them
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
        "[obs-premiere-patch] Startup scan: "
        "%zu orphaned MKV(s) found",
        orphans.size());
for (const auto &mkv : orphans) {
std::string mp4 = mkv.substr(
0, mkv.size() - 4) + ".mp4";
if (av_remux_to_mp4(mkv, mp4)) {
obs_log(LOG_INFO,
        "[obs-premiere-patch] Remuxed: %s",
        mp4.c_str());
patch_mp4(mp4, true, true);
} else {
obs_log(LOG_WARNING,
        "[obs-premiere-patch] Remux failed: %s",
        mkv.c_str());
}
}
}
}

// A/V trim pass on all MP4s in the folder
{
WIN32_FIND_DATAA         fd;
std::vector<std::string> mp4s;
HANDLE hf = FindFirstFileA((folder + "\\*.mp4").c_str(), &fd);
if (hf != INVALID_HANDLE_VALUE) {
do {
if (!(fd.dwFileAttributes &
      FILE_ATTRIBUTE_DIRECTORY))
mp4s.push_back(folder + "\\" +
               fd.cFileName);
} while (FindNextFileA(hf, &fd));
FindClose(hf);
}
int patched = 0;
for (const auto &mp4 : mp4s) {
uint8_t trim_st = OPP_STATUS_NONE, markers_st = OPP_STATUS_NONE;
xmp_read_status(mp4, &trim_st, &markers_st);
bool needs_trim    = s_auto_trim.load()    && trim_st    != OPP_STATUS_DONE;
bool needs_markers = s_auto_markers.load() && markers_st != OPP_STATUS_DONE;
if (needs_trim || needs_markers) {
patch_mp4(mp4, needs_markers, needs_trim);
patched++;
}
}
if (patched > 0)
obs_log(LOG_INFO,
        "[obs-premiere-patch] Startup scan: "
        "patched %d file(s)",
        patched);
}

obs_log(LOG_INFO, "[obs-premiere-patch] Startup scan complete");
}

// ---------------------------------------------------------------------------
// Recursive folder scan
// ---------------------------------------------------------------------------

static void scan_recursive(const std::string &folder, const std::string &ext,
                            std::vector<std::string> &out)
{
WIN32_FIND_DATAA fd;

HANDLE hf = FindFirstFileA((folder + "\\*" + ext).c_str(), &fd);
if (hf != INVALID_HANDLE_VALUE) {
do {
if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
out.push_back(folder + "\\" + fd.cFileName);
} while (FindNextFileA(hf, &fd));
FindClose(hf);
}

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
// Mode C: manual fix workers
// ---------------------------------------------------------------------------

static void fix_folder_markers_worker(std::string folder)
{
obs_log(LOG_INFO, "[obs-premiere-patch] Fix folder (markers): %s",
        folder.c_str());

std::vector<std::string> mp4s, mkvs;
scan_recursive(folder, ".mp4", mp4s);
scan_recursive(folder, ".mkv", mkvs);

for (const auto &f : mp4s)
patch_mp4(f, true, false);

for (const auto &mkv : mkvs) {
std::string mp4 = mkv.substr(0, mkv.size() - 4) + ".mp4";
if (av_remux_to_mp4(mkv, mp4))
patch_mp4(mp4, true, false);
}

obs_log(LOG_INFO,
        "[obs-premiere-patch] Fix folder (markers) done: "
        "%zu MP4 + %zu MKV",
        mp4s.size(), mkvs.size());
}

static void fix_file_markers_worker(std::string path)
{
obs_log(LOG_INFO, "[obs-premiere-patch] Fix file (markers): %s",
        path.c_str());
patch_mp4(path, true, false);
}

static void fix_folder_trim_worker(std::string folder)
{
obs_log(LOG_INFO, "[obs-premiere-patch] Fix folder (A/V trim): %s",
        folder.c_str());

std::vector<std::string> mp4s;
scan_recursive(folder, ".mp4", mp4s);

for (const auto &f : mp4s)
patch_mp4(f, false, true);

obs_log(LOG_INFO,
        "[obs-premiere-patch] Fix folder (A/V trim) done: "
        "%zu file(s)",
        mp4s.size());
}

static void fix_file_trim_worker(std::string path)
{
obs_log(LOG_INFO, "[obs-premiere-patch] Fix file (A/V trim): %s",
        path.c_str());
patch_mp4(path, false, true);
}

// ---------------------------------------------------------------------------
// IFileOpenDialog helpers (COM already initialised by OBS)
// ---------------------------------------------------------------------------

static std::string pick_folder()
{
std::string      result;
IFileOpenDialog *pfd = nullptr;

if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL,
                             CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&pfd))))
return result;

DWORD opts = 0;
pfd->GetOptions(&opts);
pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST |
                FOS_FORCEFILESYSTEM);
pfd->SetTitle(L"Select folder");

if (SUCCEEDED(pfd->Show(NULL))) {
IShellItem *item = nullptr;
if (SUCCEEDED(pfd->GetResult(&item))) {
PWSTR wpath = nullptr;
if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,
                                   &wpath))) {
int n = WideCharToMultiByte(CP_UTF8, 0, wpath,
                            -1, NULL, 0, NULL,
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

static std::string pick_file()
{
std::string      result;
IFileOpenDialog *pfd = nullptr;

if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL,
                             CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&pfd))))
return result;

COMDLG_FILTERSPEC filter = {L"MP4 Files (*.mp4)", L"*.mp4"};
pfd->SetFileTypes(1, &filter);
pfd->SetFileTypeIndex(1);

DWORD opts = 0;
pfd->GetOptions(&opts);
pfd->SetOptions((opts & ~FOS_PICKFOLDERS) | FOS_PATHMUSTEXIST |
                FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
pfd->SetTitle(L"Select MP4 file");

if (SUCCEEDED(pfd->Show(NULL))) {
IShellItem *item = nullptr;
if (SUCCEEDED(pfd->GetResult(&item))) {
PWSTR wpath = nullptr;
if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,
                                   &wpath))) {
int n = WideCharToMultiByte(CP_UTF8, 0, wpath,
                            -1, NULL, 0, NULL,
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

// ---------------------------------------------------------------------------
// ADS status helpers  (video.mp4:obs-pp — 2 bytes, writable during recording)
// The ADS is written at RECORDING_STARTED so crash detection requires zero
// extra files.  read/write wrappers delegate to xmp_read/write_status.
// ---------------------------------------------------------------------------

// Write ADS-only pending flag directly to file (moov doesn't exist yet).
// xmp_write_status also writes ADS, but at recording start we want just ADS
// (calling xmp_write_status before moov exists is fine — it skips OBPS quietly).
static void stamp_ads_pending(const std::string &mp4_path)
{
std::string ads = mp4_path + ":obs-pp";
HANDLE ha = CreateFileA(ads.c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
if (ha == INVALID_HANDLE_VALUE) return;
uint8_t flags[2] = {OPP_STATUS_NONE, OPP_STATUS_NONE};
DWORD w;
WriteFile(ha, flags, 2, &w, nullptr);
CloseHandle(ha);
}

void mp_start(void)
{
obs_log(LOG_INFO, "[obs-premiere-patch] started");
}

void mp_stop(void)
{
obs_log(LOG_INFO, "[obs-premiere-patch] stopped");
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
obs_log(LOG_INFO, "[obs-premiere-patch] Auto-markers: %s",
        on ? "ON" : "OFF");
}

void mp_set_auto_trim(int on)
{
s_auto_trim.store(on != 0);
obs_log(LOG_INFO, "[obs-premiere-patch] Auto-trim: %s",
        on ? "ON" : "OFF");
}

void mp_on_recording_started(void)
{
std::string path = get_recording_path();
if (path.empty()) return;
// Stamp ADS immediately — works while OBS holds the main file handle.
// This is the crash-recovery anchor: if OBS dies before recording stops,
// the ADS {0,0} survives and startup_scan will detect + process the file.
stamp_ads_pending(path);
obs_log(LOG_INFO, "[obs-premiere-patch] Recording started (ADS stamped): %s",
        path.c_str());
}

void mp_on_recording_stopped(void)
{
std::string path = get_recording_path();
if (path.empty()) {
obs_log(LOG_WARNING,
        "[obs-premiere-patch] Could not determine recording path");
return;
}

auto ends_mp4 = [](const std::string &s) {
return s.size() >= 4 &&
       _stricmp(s.c_str() + s.size() - 4, ".mp4") == 0;
};

if (!ends_mp4(path)) {
obs_log(LOG_INFO,
        "[obs-premiere-patch] Not an MP4 (%s), skipping",
        path.c_str());
return;
}

std::thread([path]() { patch_recording(path); }).detach();
}

void mp_on_obs_loaded(void)
{
// No separate txt file needed — crash survivors are MP4s in the rec folder
// whose ADS has trim=0 or markers=0.  startup_scan handles them.
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
std::thread([folder]() {
fix_folder_markers_worker(folder);
}).detach();
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
std::thread([folder]() {
fix_folder_trim_worker(folder);
}).detach();
}

void mp_fix_file_trim(void)
{
std::string path = pick_file();
if (path.empty())
return;
std::thread([path]() { fix_file_trim_worker(path); }).detach();
}
