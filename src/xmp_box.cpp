/*
 * xmp_box.cpp  —  MP4 moov.udta.XMP_ box injection
 *
 * No external dependencies. All MP4 I/O via Win32 HANDLE.
 *
 * Box format (ISO 14496-12):
 *   [4 bytes size big-endian] [4 bytes type ASCII] [payload...]
 *   If size==1: next 8 bytes are extended uint64 size.
 *   If size==0: extends to EOF.
 *
 * For OBS hybrid_mp4 output the layout is always:
 *   ftyp  →  mdat (huge)  →  moov (at EOF)
 * so moov-at-end truncate+append is used.
 * Moov-at-start files fall back to write-temp+rename.
 */

#include "xmp_box.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

static uint32_t u32be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static void pu32be(uint8_t *p, uint32_t v)
{
	p[0] = (v >> 24) & 0xFF;
	p[1] = (v >> 16) & 0xFF;
	p[2] = (v >> 8) & 0xFF;
	p[3] = v & 0xFF;
}

static bool type_eq(const uint8_t *p, const char *t)
{
	return p[4] == (uint8_t)t[0] && p[5] == (uint8_t)t[1] &&
	       p[6] == (uint8_t)t[2] && p[7] == (uint8_t)t[3];
}

// Build an MP4 box from a 4-char type + payload bytes.
static std::vector<uint8_t> make_box(const char *type,
                                      const std::vector<uint8_t> &payload)
{
	uint32_t            total = (uint32_t)(8 + payload.size());
	std::vector<uint8_t> box(total);
	pu32be(box.data(), total);
	box[4] = type[0];
	box[5] = type[1];
	box[6] = type[2];
	box[7] = type[3];
	if (!payload.empty())
		memcpy(box.data() + 8, payload.data(), payload.size());
	return box;
}

// ---------------------------------------------------------------------------
// Win32 file helpers
// ---------------------------------------------------------------------------

static bool file_read_at(HANDLE h, int64_t offset, void *buf, DWORD n)
{
	LARGE_INTEGER li;
	li.QuadPart = offset;
	if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
		return false;
	DWORD got = 0;
	return ReadFile(h, buf, n, &got, nullptr) && got == n;
}

static bool file_write_at(HANDLE h, int64_t offset, const void *buf, DWORD n)
{
	LARGE_INTEGER li;
	li.QuadPart = offset;
	if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
		return false;
	DWORD written = 0;
	return WriteFile(h, buf, n, &written, nullptr) && written == n;
}

static int64_t file_size(HANDLE h)
{
	LARGE_INTEGER sz = {};
	GetFileSizeEx(h, &sz);
	return sz.QuadPart;
}

// ---------------------------------------------------------------------------
// Box scanning: find a top-level or child box
// ---------------------------------------------------------------------------

struct BoxInfo {
	int64_t  offset;      // byte offset of the first header byte
	uint64_t total_size;  // including 8-byte header
	bool     found;
};

// Scan boxes within [scan_start, scan_start+scan_len) inside buffer `data`.
static BoxInfo find_box_in_buf(const uint8_t *data, size_t data_len,
                                const char *type, size_t scan_start = 8)
{
	BoxInfo result = {0, 0, false};
	size_t  pos    = scan_start;
	while (pos + 8 <= data_len) {
		uint32_t sz = u32be(data + pos);
		if (sz < 8 || pos + sz > data_len)
			break;
		if (type_eq(data + pos, type)) {
			result.offset     = (int64_t)pos;
			result.total_size = sz;
			result.found      = true;
			return result;
		}
		pos += sz;
	}
	return result;
}

// Scan boxes in a FILE starting from file_start bytes, up to file_size bytes.
static BoxInfo find_box_in_file(HANDLE h, int64_t file_start,
                                 int64_t limit, const char *type)
{
	BoxInfo result = {0, 0, false};
	int64_t pos    = file_start;
	while (pos + 8 <= file_start + limit) {
		uint8_t hdr[8];
		if (!file_read_at(h, pos, hdr, 8))
			break;
		uint32_t sz = u32be(hdr);
		if (sz == 0 || sz < 8)
			break;
		if (type_eq(hdr, type)) {
			result.offset     = pos;
			result.total_size = sz;
			result.found      = true;
			return result;
		}
		pos += sz;
	}
	return result;
}

// ---------------------------------------------------------------------------
// Build patched udta bytes (without the 8-byte udta header)
// ---------------------------------------------------------------------------

static std::vector<uint8_t> build_new_udta_payload(
	const uint8_t *udta_inner, size_t udta_inner_len,
	const std::string &xmp_content)
{
	// Build new XMP_ box
	std::vector<uint8_t> xmp_bytes(xmp_content.begin(), xmp_content.end());
	std::vector<uint8_t> xmp_box = make_box("XMP_", xmp_bytes);

	// Copy all existing udta children, skip any pre-existing XMP_
	std::vector<uint8_t> children;
	size_t               pos = 0;
	while (pos + 8 <= udta_inner_len) {
		uint32_t sz = u32be(udta_inner + pos);
		if (sz < 8 || pos + sz > udta_inner_len)
			break;
		bool is_xmp = (udta_inner[pos + 4] == 'X' &&
		               udta_inner[pos + 5] == 'M' &&
		               udta_inner[pos + 6] == 'P' &&
		               udta_inner[pos + 7] == '_');
		if (!is_xmp)
			children.insert(children.end(), udta_inner + pos,
			                udta_inner + pos + sz);
		pos += sz;
	}
	// Append our XMP_ box
	children.insert(children.end(), xmp_box.begin(), xmp_box.end());
	return children;
}

// ---------------------------------------------------------------------------
// Build patched moov bytes (full box including 8-byte moov header)
// ---------------------------------------------------------------------------

static std::vector<uint8_t>
build_new_moov(const uint8_t *old_moov, size_t old_moov_len,
               const std::string &xmp_content)
{
	// Find udta within moov (skip 8-byte moov header)
	BoxInfo udta = find_box_in_buf(old_moov, old_moov_len, "udta", 8);

	std::vector<uint8_t> new_udta_payload;
	if (udta.found) {
		const uint8_t *udta_inner = old_moov + udta.offset + 8;
		size_t         udta_inner_len = (size_t)udta.total_size - 8;
		new_udta_payload =
			build_new_udta_payload(udta_inner, udta_inner_len,
			                       xmp_content);
	} else {
		// No udta at all — create one with just our XMP_
		std::vector<uint8_t> xmp_bytes(xmp_content.begin(),
		                               xmp_content.end());
		std::vector<uint8_t> xmp_box = make_box("XMP_", xmp_bytes);
		new_udta_payload = xmp_box;
	}
	std::vector<uint8_t> new_udta = make_box("udta", new_udta_payload);

	// Reassemble moov: copy all children except the old udta, then append new udta
	std::vector<uint8_t> moov_children;
	size_t               pos = 8; // skip moov header
	while (pos + 8 <= old_moov_len) {
		uint32_t sz = u32be(old_moov + pos);
		if (sz < 8 || pos + sz > old_moov_len)
			break;
		bool is_udta = (old_moov[pos + 4] == 'u' &&
		                old_moov[pos + 5] == 'd' &&
		                old_moov[pos + 6] == 't' &&
		                old_moov[pos + 7] == 'a');
		if (!is_udta)
			moov_children.insert(moov_children.end(),
			                     old_moov + pos, old_moov + pos + sz);
		pos += sz;
	}
	moov_children.insert(moov_children.end(), new_udta.begin(),
	                     new_udta.end());

	return make_box("moov", moov_children);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool xmp_inject(const std::string &mp4_path, const std::string &xmp_content)
{
	HANDLE h = CreateFileA(mp4_path.c_str(),
	                        GENERIC_READ | GENERIC_WRITE, 0, nullptr,
	                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	int64_t fsize   = file_size(h);
	int64_t scan_sz = min(fsize, (int64_t)64 * 1024 * 1024); // scan up to 64 MB of headers

	// Find moov by scanning from start (stop once we've covered all box headers)
	BoxInfo moov = find_box_in_file(h, 0, fsize, "moov");
	if (!moov.found) {
		CloseHandle(h);
		return false;
	}

	// Read moov into memory (moov is typically < 10 MB for OBS recordings)
	if (moov.total_size > 256 * 1024 * 1024) {
		// Unreasonably large moov — bail
		CloseHandle(h);
		return false;
	}
	std::vector<uint8_t> old_moov((size_t)moov.total_size);
	if (!file_read_at(h, moov.offset, old_moov.data(),
	                  (DWORD)moov.total_size)) {
		CloseHandle(h);
		return false;
	}

	// Build new moov
	std::vector<uint8_t> new_moov =
		build_new_moov(old_moov.data(), old_moov.size(), xmp_content);

	bool moov_at_end =
		(moov.offset + (int64_t)moov.total_size >= fsize - 8);

	if (moov_at_end) {
		// Write new moov at same offset, then truncate
		if (!file_write_at(h, moov.offset, new_moov.data(),
		                   (DWORD)new_moov.size())) {
			CloseHandle(h);
			return false;
		}
		// Truncate to moov_offset + new_moov_size
		LARGE_INTEGER li;
		li.QuadPart = moov.offset + (int64_t)new_moov.size();
		SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
		SetEndOfFile(h);
		CloseHandle(h);
		return true;
	}

	// moov not at end — write via temp file
	CloseHandle(h);

	std::string tmp = mp4_path + ".xmptmp.mp4";
	// Open source for reading
	HANDLE hs = CreateFileA(mp4_path.c_str(), GENERIC_READ,
	                         FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                         FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hs == INVALID_HANDLE_VALUE)
		return false;
	HANDLE ht = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
	                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (ht == INVALID_HANDLE_VALUE) {
		CloseHandle(hs);
		return false;
	}

	// Copy [0 .. moov_offset)
	int64_t       copy_pos = 0;
	const DWORD   BUF      = 1024 * 1024;
	std::vector<uint8_t> buf(BUF);
	bool          ok       = true;
	while (ok && copy_pos < moov.offset) {
		DWORD  to_read = (DWORD)min((int64_t)BUF, moov.offset - copy_pos);
		DWORD  written;
		ok = file_read_at(hs, copy_pos, buf.data(), to_read) &&
		     WriteFile(ht, buf.data(), to_read, &written, nullptr) &&
		     written == to_read;
		copy_pos += to_read;
	}
	// Write new moov
	if (ok) {
		DWORD written = 0;
		ok = WriteFile(ht, new_moov.data(), (DWORD)new_moov.size(),
		               &written, nullptr) &&
		     written == (DWORD)new_moov.size();
	}
	// Copy [moov_end .. EOF)
	int64_t moov_end = moov.offset + (int64_t)moov.total_size;
	copy_pos         = moov_end;
	while (ok && copy_pos < fsize) {
		DWORD to_read = (DWORD)min((int64_t)BUF, fsize - copy_pos);
		DWORD written;
		ok = file_read_at(hs, copy_pos, buf.data(), to_read) &&
		     WriteFile(ht, buf.data(), to_read, &written, nullptr) &&
		     written == to_read;
		copy_pos += to_read;
	}
	CloseHandle(hs);
	CloseHandle(ht);

	if (!ok) {
		DeleteFileA(tmp.c_str());
		return false;
	}
	DeleteFileA(mp4_path.c_str());
	MoveFileA(tmp.c_str(), mp4_path.c_str());
	return true;
}

bool xmp_has_ours(const std::string &mp4_path)
{
	HANDLE h = CreateFileA(mp4_path.c_str(), GENERIC_READ,
	                        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                        FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	int64_t fsize = file_size(h);
	BoxInfo moov  = find_box_in_file(h, 0, fsize, "moov");
	if (!moov.found || moov.total_size > 256 * 1024 * 1024) {
		CloseHandle(h);
		return false;
	}

	std::vector<uint8_t> moov_data((size_t)moov.total_size);
	bool                 ok = file_read_at(h, moov.offset, moov_data.data(),
                                               (DWORD)moov.total_size);
	CloseHandle(h);
	if (!ok)
		return false;

	// Find udta in moov
	BoxInfo udta = find_box_in_buf(moov_data.data(), moov_data.size(),
	                               "udta", 8);
	if (!udta.found)
		return false;

	// Find XMP_ in udta
	const uint8_t *udta_base = moov_data.data() + udta.offset;
	BoxInfo        xmp_b = find_box_in_buf(udta_base, (size_t)udta.total_size,
                                               "XMP_", 8);
	if (!xmp_b.found)
		return false;

	// Check content for our marker
	const uint8_t *xmp_payload = udta_base + xmp_b.offset + 8;
	size_t         xmp_len     = (size_t)xmp_b.total_size - 8;
	static const char needle[] = "obs-premiere-patch";
	size_t            nlen     = sizeof(needle) - 1;
	for (size_t i = 0; i + nlen <= xmp_len; i++) {
		if (memcmp(xmp_payload + i, needle, nlen) == 0)
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// OBPS status box  (moov/udta/OBPS, 12 bytes fixed)
//
//   offset  size  meaning
//   0       4     box size = 12 (big-endian)
//   4       4     type = 'O','B','P','S'
//   8       1     trim_done    (0 = pending, 1 = done)
//   9       1     markers_done (0 = pending, 1 = done)
//   10      2     reserved = 0
//
// Because size is fixed, the status bytes can be updated with a 2-byte
// in-place write — no moov rewrite needed after the initial injection.
// ---------------------------------------------------------------------------

// Return the file offset of the trim byte (offset+8) inside OBPS,
// or -1 if no OBPS box is found.
static int64_t find_obps_off(const uint8_t *moov_buf, uint32_t moov_sz,
                              int64_t moov_file_off)
{
	size_t pos = 8; // skip moov header
	while (pos + 8 <= moov_sz) {
		uint32_t sz = u32be(moov_buf + pos);
		if (sz < 8 || pos + sz > moov_sz) break;
		if (type_eq(moov_buf + pos, "udta")) {
			size_t udta_end = pos + sz;
			size_t up       = pos + 8; // skip udta header
			while (up + 8 <= udta_end) {
				uint32_t usz = u32be(moov_buf + up);
				if (usz < 8 || up + usz > udta_end) break;
				if (type_eq(moov_buf + up, "OBPS") && usz >= 12)
					return moov_file_off + (int64_t)up + 8;
				up += usz;
			}
			return -1; // udta found, no OBPS
		}
		pos += sz;
	}
	return -1;
}

// Build udta inner payload with OBPS replaced/added (other children preserved).
static std::vector<uint8_t>
build_udta_with_obps(const uint8_t *udta_inner, size_t udta_inner_len,
                     uint8_t trim_st, uint8_t markers_st, uint8_t names_st)
{
	uint8_t              pl[4] = {trim_st, markers_st, names_st, 0};
	std::vector<uint8_t> obps_box =
		make_box("OBPS", std::vector<uint8_t>(pl, pl + 4));

	std::vector<uint8_t> out;
	size_t               pos = 0;
	while (pos + 8 <= udta_inner_len) {
		uint32_t sz = u32be(udta_inner + pos);
		if (sz < 8 || pos + sz > udta_inner_len) break;
		if (!type_eq(udta_inner + pos, "OBPS"))
			out.insert(out.end(), udta_inner + pos, udta_inner + pos + sz);
		pos += sz;
	}
	out.insert(out.end(), obps_box.begin(), obps_box.end());
	return out;
}

// Helper: scan box headers (8 bytes each) to find moov, then read it.
// Returns INVALID_HANDLE_VALUE on error; sets moov_off/moov_sz.
static bool read_moov_only(HANDLE h, int64_t *moov_off, uint32_t *moov_sz,
                            std::vector<uint8_t> *mbuf)
{
	*moov_off = -1;
	{
		int64_t pos = 0;
		uint8_t hdr[8];
		for (;;) {
			DWORD        got = 0;
			LARGE_INTEGER li{};
			li.QuadPart = pos;
			SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
			if (!ReadFile(h, hdr, 8, &got, nullptr) || got < 8) break;
			uint32_t sz = u32be(hdr);
			if (sz < 8) break;
			if (type_eq(hdr, "moov")) { *moov_off = pos; *moov_sz = sz; break; }
			pos += sz;
		}
	}
	if (*moov_off < 0 || *moov_sz > 256u * 1024 * 1024) return false;
	mbuf->resize(*moov_sz);
	DWORD        got = 0;
	LARGE_INTEGER li{};
	li.QuadPart = *moov_off;
	SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
	ReadFile(h, mbuf->data(), *moov_sz, &got, nullptr);
	return got == *moov_sz;
}

bool xmp_read_status(const std::string &mp4_path,
                     uint8_t *trim_st, uint8_t *markers_st, uint8_t *names_st)
{
	*trim_st = *markers_st = *names_st = OPP_STATUS_NONE;

	// --- Fast path: NTFS Alternate Data Stream ---
	std::string ads = mp4_path + ":obs-pp";
	HANDLE ha = CreateFileA(ads.c_str(), GENERIC_READ,
	                        FILE_SHARE_READ | FILE_SHARE_WRITE,
	                        nullptr, OPEN_EXISTING,
	                        FILE_ATTRIBUTE_NORMAL, nullptr);
	if (ha != INVALID_HANDLE_VALUE) {
		uint8_t flags[3] = {}; DWORD got = 0;
		ReadFile(ha, flags, 3, &got, nullptr);
		CloseHandle(ha);
		if (got >= 2) {
			*trim_st    = flags[0];
			*markers_st = flags[1];
			*names_st   = (got >= 3) ? flags[2] : OPP_STATUS_NONE;
			return true;
		}
	}

	// --- Fallback: OBPS box inside moov/udta ---
	HANDLE h = CreateFileA(mp4_path.c_str(), GENERIC_READ,
	                       FILE_SHARE_READ | FILE_SHARE_WRITE,
	                       nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;

	int64_t              moov_off; uint32_t moov_sz;
	std::vector<uint8_t> mbuf;
	if (!read_moov_only(h, &moov_off, &moov_sz, &mbuf)) {
		CloseHandle(h); return false;
	}
	int64_t status_off = find_obps_off(mbuf.data(), moov_sz, moov_off);
	if (status_off < 0) { CloseHandle(h); return false; }

	uint8_t flags[3] = {}; DWORD got2 = 0;
	LARGE_INTEGER li{}; li.QuadPart = status_off;
	SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
	ReadFile(h, flags, 3, &got2, nullptr);
	CloseHandle(h);
	if (got2 < 2) return false;
	*trim_st    = flags[0];
	*markers_st = flags[1];
	*names_st   = (got2 >= 3) ? flags[2] : OPP_STATUS_NONE;
	return true;
}

bool xmp_write_status(const std::string &mp4_path,
                      uint8_t trim_st, uint8_t markers_st, uint8_t names_st)
{
	uint8_t flags[3] = {trim_st, markers_st, names_st};

	// --- Always write ADS (works before moov exists, O(1) seek+write) ---
	{
		std::string ads = mp4_path + ":obs-pp";
		HANDLE ha = CreateFileA(ads.c_str(), GENERIC_WRITE,
		                        FILE_SHARE_READ, nullptr,
		                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (ha != INVALID_HANDLE_VALUE) {
			DWORD w; WriteFile(ha, flags, 3, &w, nullptr);
			CloseHandle(ha);
		}
	}

	// --- Also update OBPS box in moov if it exists (durable fallback) ---
	HANDLE h = CreateFileA(mp4_path.c_str(), GENERIC_READ | GENERIC_WRITE,
	                       FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return true; // ADS write succeeded, that's enough

	int64_t              moov_off; uint32_t moov_sz;
	std::vector<uint8_t> mbuf;
	if (!read_moov_only(h, &moov_off, &moov_sz, &mbuf)) {
		CloseHandle(h); return true; // no moov yet (recording in progress), ok
	}

	// If OBPS already exists: 3-byte in-place write.
	int64_t status_off = find_obps_off(mbuf.data(), moov_sz, moov_off);
	if (status_off >= 0) {
		file_write_at(h, status_off, flags, 3);
		CloseHandle(h); return true;
	}

	// OBPS doesn't exist — rebuild moov/udta to inject it.
	BoxInfo udta = find_box_in_buf(mbuf.data(), moov_sz, "udta", 8);
	std::vector<uint8_t> new_udta_inner;
	if (udta.found) {
		new_udta_inner = build_udta_with_obps(
			mbuf.data() + udta.offset + 8,
			(size_t)udta.total_size - 8,
			trim_st, markers_st, names_st);
	} else {
		uint8_t pl[4] = {trim_st, markers_st, names_st, 0};
		new_udta_inner = make_box("OBPS", std::vector<uint8_t>(pl, pl + 4));
	}
	std::vector<uint8_t> new_udta  = make_box("udta", new_udta_inner);
	std::vector<uint8_t> moov_children;
	size_t pos = 8;
	while (pos + 8 <= moov_sz) {
		uint32_t sz = u32be(mbuf.data() + pos);
		if (sz < 8 || pos + sz > moov_sz) break;
		if (!type_eq(mbuf.data() + pos, "udta"))
			moov_children.insert(moov_children.end(),
			                     mbuf.data() + pos, mbuf.data() + pos + sz);
		pos += sz;
	}
	moov_children.insert(moov_children.end(), new_udta.begin(), new_udta.end());
	std::vector<uint8_t> new_moov = make_box("moov", moov_children);

	int64_t fsize        = file_size(h);
	bool    moov_at_end  = (moov_off + (int64_t)moov_sz >= fsize - 8);
	if (moov_at_end) {
		file_write_at(h, moov_off, new_moov.data(), (DWORD)new_moov.size());
		LARGE_INTEGER li{}; li.QuadPart = moov_off + (int64_t)new_moov.size();
		SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
		SetEndOfFile(h);
	}
	CloseHandle(h);
	return true;
}

// ---------------------------------------------------------------------------
// xmp_fix_tkhd_durations
// ---------------------------------------------------------------------------
// Aligns all tracks to the last complete video frame within the audio content.
//
// Strategy:
//   1. Compute target_frames = floor(audio_presented_samples / samples_per_frame)
//   2. target_dur = target_frames * samples_per_frame  (exact integer in audio ts)
//   3. Change movie timescale to audio_ts (e.g. 48000) so target_dur is
//      representable without rounding — this is the ONLY way to make Premiere
//      show both video (frame-snapped) and audio (sample-exact) at the same point.
//   4. Patch in-place: mvhd ts+dur, video tkhd, all audio tkhd+elst_seg,
//      all other tkhd+elst_seg scaled by (new_ts / old_ts).
//
// If the file is locked by another app a warning MessageBox is shown.

static uint64_t u64be(const uint8_t *p)
{
	return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
	       ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
	       ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
	       ((uint64_t)p[6] << 8)  |  (uint64_t)p[7];
}

static void pu64be(uint8_t *p, uint64_t v)
{
	for (int i = 7; i >= 0; --i, v >>= 8)
		p[i] = (uint8_t)(v & 0xFF);
}

static bool write_at(HANDLE h, int64_t offset, const void *buf, DWORD n)
{
	LARGE_INTEGER li;
	li.QuadPart = offset;
	if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
		return false;
	DWORD wrote = 0;
	return WriteFile(h, buf, n, &wrote, nullptr) && wrote == n;
}

bool xmp_fix_tkhd_durations(const std::string &mp4_path)
{
	// --- Open file ---
	HANDLE h = CreateFileA(mp4_path.c_str(),
	                       GENERIC_READ | GENERIC_WRITE,
	                       FILE_SHARE_READ,
	                       nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		if (GetLastError() == ERROR_SHARING_VIOLATION) {
			MessageBoxA(nullptr,
			            "OBS Premiere Patch could not patch this recording "
			            "because it is open in another application.\n\n"
			            "Close the file and try again via "
			            "Tools \xE2\x86\x92 Premiere Patch \xE2\x86\x92 Patch File.",
			            "OBS Premiere Patch \xE2\x80\x94 File In Use",
			            MB_OK | MB_ICONWARNING);
		}
		return false;
	}

	// --- Scan box headers to find moov (reads 8 bytes per box, skips mdat) ---
	int64_t  moov_off = -1;
	uint32_t moov_sz  = 0;
	{
		int64_t pos = 0;
		uint8_t hdr[8];
		while (true) {
			DWORD    got = 0;
			LARGE_INTEGER li{}; li.QuadPart = pos;
			SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
			if (!ReadFile(h, hdr, 8, &got, nullptr) || got < 8) break;
			uint32_t sz = u32be(hdr);
			if (sz < 8) break;
			if (type_eq(hdr, "moov")) { moov_off = pos; moov_sz = sz; break; }
			pos += sz;
		}
	}
	if (moov_off < 0) { CloseHandle(h); return false; }

	// --- Read only the moov box (typically ~100 KB, not the whole file) ---
	std::vector<uint8_t> mbuf(moov_sz);
	{
		DWORD    got = 0;
		LARGE_INTEGER li{}; li.QuadPart = moov_off;
		SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
		if (!ReadFile(h, mbuf.data(), moov_sz, &got, nullptr) || got != moov_sz) {
			CloseHandle(h); return false;
		}
	}
	const uint8_t *moov = mbuf.data();

	// --- Find mvhd ---
	int64_t  mvhd_ts_off  = -1;
	int64_t  mvhd_dur_off = -1;
	int      mvhd_ver     = -1;
	uint32_t movie_ts     = 0;
	{
		int64_t pos = 8;
		while (pos + 8 <= (int64_t)moov_sz) {
			uint32_t sz = u32be(moov + pos);
			if (sz < 8) break;
			if (type_eq(moov + pos, "mvhd")) {
				mvhd_ver = moov[pos + 8];
				if (mvhd_ver == 0) {
					movie_ts     = u32be(moov + pos + 20);
					mvhd_ts_off  = moov_off + pos + 20;
					mvhd_dur_off = moov_off + pos + 24;
				} else {
					movie_ts     = u32be(moov + pos + 28);
					mvhd_ts_off  = moov_off + pos + 28;
					mvhd_dur_off = moov_off + pos + 32;
				}
				break;
			}
			pos += sz;
		}
	}
	if (mvhd_ts_off < 0 || movie_ts == 0) { CloseHandle(h); return false; }

	// --- Collect trak info ---
	struct TI {
		char     hdlr[5];
		int      tkhd_ver, elst_ver;
		int64_t  tkhd_dur_off;   // -1 if absent
		int64_t  elst_seg_off;   // -1 if absent
		uint64_t tkhd_dur, elst_seg, elst_mt;
		uint32_t mdhd_ts;
		uint64_t mdhd_dur;
	};
	std::vector<TI> traks;

	{
		int64_t pos = 8;
		while (pos + 8 <= (int64_t)moov_sz) {
			uint32_t sz = u32be(moov + pos);
			if (sz < 8 || pos + (int64_t)sz > (int64_t)moov_sz) break;
			if (!type_eq(moov + pos, "trak")) { pos += sz; continue; }

			TI ti{};
			ti.hdlr[0] = '?'; ti.hdlr[4] = '\0';
			ti.tkhd_ver = -1; ti.elst_ver = -1;
			ti.tkhd_dur_off = -1; ti.elst_seg_off = -1;
			ti.tkhd_dur = 0; ti.elst_seg = 0; ti.elst_mt = 0;
			ti.mdhd_ts = 0; ti.mdhd_dur = 0;

			int64_t trak_end = pos + (int64_t)sz;
			int64_t cp = pos + 8;
			while (cp + 8 <= trak_end) {
				uint32_t csz = u32be(moov + cp);
				if (csz < 8 || cp + (int64_t)csz > trak_end) break;

				if (type_eq(moov + cp, "tkhd")) {
					ti.tkhd_ver = moov[cp + 8];
					if (ti.tkhd_ver == 0) {
						ti.tkhd_dur     = u32be(moov + cp + 28);
						ti.tkhd_dur_off = moov_off + cp + 28;
					} else {
						ti.tkhd_dur     = u64be(moov + cp + 36);
						ti.tkhd_dur_off = moov_off + cp + 36;
					}
				}
				if (type_eq(moov + cp, "edts")) {
					int64_t ep = cp + 8, edts_end = cp + (int64_t)csz;
					while (ep + 8 <= edts_end) {
						uint32_t esz = u32be(moov + ep);
						if (esz < 8 || ep + (int64_t)esz > edts_end) break;
						if (type_eq(moov + ep, "elst")) {
							ti.elst_ver = moov[ep + 8];
							if (ti.elst_ver == 0) {
								ti.elst_seg     = u32be(moov + ep + 16);
								ti.elst_mt      = u32be(moov + ep + 20);
								ti.elst_seg_off = moov_off + ep + 16;
							} else {
								ti.elst_seg     = u64be(moov + ep + 16);
								ti.elst_mt      = u64be(moov + ep + 24);
								ti.elst_seg_off = moov_off + ep + 16;
							}
						}
						ep += esz;
					}
				}
				if (type_eq(moov + cp, "mdia")) {
					int64_t dp = cp + 8, mdia_end = cp + (int64_t)csz;
					while (dp + 8 <= mdia_end) {
						uint32_t dsz = u32be(moov + dp);
						if (dsz < 8) break;
						if (type_eq(moov + dp, "hdlr") && dp + 20 <= trak_end) {
							ti.hdlr[0] = (char)moov[dp + 16]; ti.hdlr[1] = (char)moov[dp + 17];
							ti.hdlr[2] = (char)moov[dp + 18]; ti.hdlr[3] = (char)moov[dp + 19];
						}
						if (type_eq(moov + dp, "mdhd")) {
							int dver = moov[dp + 8];
							if (dver == 0) {
								ti.mdhd_ts  = u32be(moov + dp + 20);
								ti.mdhd_dur = u32be(moov + dp + 24);
							} else {
								ti.mdhd_ts  = u32be(moov + dp + 28);
								ti.mdhd_dur = u64be(moov + dp + 32);
							}
						}
						dp += dsz;
					}
				}
				cp += csz;
			}
			if (ti.tkhd_dur_off >= 0)
				traks.push_back(ti);
			pos += sz;
		}
	}

	// --- Find first video and first audio trak ---
	TI *vide = nullptr, *soun = nullptr;
	for (auto &ti : traks) {
		if (!vide && ti.hdlr[0]=='v' && ti.hdlr[1]=='i') vide = &ti;
		if (!soun && ti.hdlr[0]=='s' && ti.hdlr[1]=='o' && ti.elst_seg_off >= 0) soun = &ti;
	}
	if (!vide || !soun || soun->mdhd_ts == 0 || vide->mdhd_ts == 0) {
		CloseHandle(h); return false;
	}

	// --- Compute target duration ---
	// audio_ts = audio sample rate (e.g. 48000)
	// video_fps = video mdhd timescale (OBS uses 1 tick per frame, ts = fps)
	uint32_t audio_ts  = soun->mdhd_ts;
	uint32_t video_fps = vide->mdhd_ts;
	if (audio_ts % video_fps != 0) { CloseHandle(h); return false; }
	uint64_t spf = audio_ts / video_fps; // samples per frame, e.g. 800

	// Priming skip = elst media_time (in audio samples, already in audio ts)
	uint64_t priming = soun->elst_mt;
	if (priming >= soun->mdhd_dur) { CloseHandle(h); return false; }
	uint64_t audio_presented = soun->mdhd_dur - priming; // content samples

	uint64_t target_frames = audio_presented / spf;       // floor
	uint64_t target_dur    = target_frames * spf;         // exact, in audio_ts units

	// We change movie timescale to audio_ts so target_dur is exact (no rounding).
	// E.g. movie_ts 1000 → 48000: 431 frames × 800 = 344800 (representable exactly).
	uint32_t new_movie_ts = audio_ts;
	if (new_movie_ts % movie_ts != 0) { CloseHandle(h); return false; }
	uint64_t scale = new_movie_ts / movie_ts; // e.g. 48

	// Sanity: video track (in new ts) must be longer than target_dur
	uint64_t vide_in_new = vide->tkhd_dur * scale;
	if (vide_in_new <= target_dur) { CloseHandle(h); return false; }
	double gap_s = (double)(vide_in_new - target_dur) / (double)new_movie_ts;
	if (gap_s < 0.001 || gap_s > 0.500) { CloseHandle(h); return false; }

	// --- Patch in-place ---
	bool ok = true;

	// mvhd timescale → new_movie_ts
	if (scale != 1) {
		uint8_t ts_buf[4];
		pu32be(ts_buf, new_movie_ts);
		ok = ok && write_at(h, mvhd_ts_off, ts_buf, 4);
	}
	// mvhd duration → target_dur
	{
		uint8_t d[8]; DWORD n;
		if (mvhd_ver == 0) { pu32be(d, (uint32_t)target_dur); n = 4; }
		else               { pu64be(d, target_dur);            n = 8; }
		ok = ok && write_at(h, mvhd_dur_off, d, n);
	}

	for (auto &ti : traks) {
		bool is_av   = (ti.hdlr[0]=='v' && ti.hdlr[1]=='i') ||
		               (ti.hdlr[0]=='s' && ti.hdlr[1]=='o');
		bool is_soun = (ti.hdlr[0]=='s' && ti.hdlr[1]=='o');

		uint64_t new_tkhd = is_av ? target_dur : (ti.tkhd_dur * scale);

		// tkhd duration
		if (ti.tkhd_dur_off >= 0) {
			uint8_t d[8]; DWORD n;
			if (ti.tkhd_ver == 0) { pu32be(d, (uint32_t)new_tkhd); n = 4; }
			else                  { pu64be(d, new_tkhd);            n = 8; }
			ok = ok && write_at(h, ti.tkhd_dur_off, d, n);
		}
		// elst seg_duration: video+audio → target_dur; others → scale old value
		if (ti.elst_seg_off >= 0) {
			uint64_t new_seg = is_av ? target_dur : (ti.elst_seg * scale);
			uint8_t d[8]; DWORD n;
			if (ti.elst_ver == 0) { pu32be(d, (uint32_t)new_seg); n = 4; }
			else                  { pu64be(d, new_seg);            n = 8; }
			ok = ok && write_at(h, ti.elst_seg_off, d, n);
		}
	}

	CloseHandle(h);
	return ok;
}

// ---------------------------------------------------------------------------
// xmp_write_hdlr_names
// ---------------------------------------------------------------------------
// Replaces the name field in each audio trak's hdlr box with the supplied
// track names.  names[0] = first audio track, names[1] = second, etc.
// Empty or out-of-range entries are left unchanged.
// ---------------------------------------------------------------------------

// Build a new hdlr box with the name field replaced.
static std::vector<uint8_t>
rebuild_hdlr_box(const uint8_t *hdlr, uint32_t sz, const std::string &new_name)
{
	if (sz < 32) return std::vector<uint8_t>(hdlr, hdlr + sz); // malformed, keep as-is
	// Fixed fields: version/flags(4) + pre_defined(4) + handler_type(4) + reserved(12) = 24 bytes
	// Total fixed after box header (8): 24 bytes, starting at offset 8.
	std::vector<uint8_t> payload(hdlr + 8, hdlr + 32); // 24 fixed bytes
	// Append new name as null-terminated UTF-8
	for (char c : new_name) payload.push_back((uint8_t)c);
	payload.push_back(0); // null terminator
	return make_box("hdlr", payload);
}

// Rebuild a mdia box replacing its hdlr name.
static std::vector<uint8_t>
rebuild_mdia_box(const uint8_t *mdia, uint32_t sz, const std::string &new_name)
{
	std::vector<uint8_t> children;
	size_t p = 8;
	while (p + 8 <= sz) {
		uint32_t csz = u32be(mdia + p);
		if (csz < 8 || p + csz > sz) break;
		if (type_eq(mdia + p, "hdlr")) {
			auto new_hdlr = rebuild_hdlr_box(mdia + p, csz, new_name);
			children.insert(children.end(), new_hdlr.begin(), new_hdlr.end());
		} else {
			children.insert(children.end(), mdia + p, mdia + p + csz);
		}
		p += csz;
	}
	return make_box("mdia", children);
}

// Rebuild a trak box replacing its mdia's hdlr name.
static std::vector<uint8_t>
rebuild_trak_box(const uint8_t *trak, uint32_t sz, const std::string &new_name)
{
	std::vector<uint8_t> children;
	size_t p = 8;
	while (p + 8 <= sz) {
		uint32_t csz = u32be(trak + p);
		if (csz < 8 || p + csz > sz) break;
		if (type_eq(trak + p, "mdia")) {
			auto new_mdia = rebuild_mdia_box(trak + p, csz, new_name);
			children.insert(children.end(), new_mdia.begin(), new_mdia.end());
		} else {
			children.insert(children.end(), trak + p, trak + p + csz);
		}
		p += csz;
	}
	return make_box("trak", children);
}

bool xmp_write_hdlr_names(const std::string &mp4_path,
                           const std::vector<std::string> &names)
{
	if (names.empty()) return false;

	HANDLE h = CreateFileA(mp4_path.c_str(), GENERIC_READ | GENERIC_WRITE,
	                       FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;

	int64_t              moov_off; uint32_t moov_sz;
	std::vector<uint8_t> mbuf;
	if (!read_moov_only(h, &moov_off, &moov_sz, &mbuf)) {
		CloseHandle(h); return false;
	}

	// Walk moov children; rebuild soun trak boxes with new names.
	int                  audio_idx = 0;
	bool                 any_changed = false;
	std::vector<uint8_t> new_moov_children;

	size_t pos = 8;
	while (pos + 8 <= moov_sz) {
		uint32_t sz = u32be(mbuf.data() + pos);
		if (sz < 8 || pos + sz > moov_sz) break;

		if (type_eq(mbuf.data() + pos, "trak")) {
			// Determine if this trak is audio (soun)
			bool is_soun = false;
			const uint8_t *trak = mbuf.data() + pos;
			size_t tp = 8;
			while (tp + 8 <= sz) {
				uint32_t tsz = u32be(trak + tp);
				if (tsz < 8 || tp + tsz > sz) break;
				if (type_eq(trak + tp, "mdia")) {
					size_t mp = 8;
					while (mp + 8 <= tsz) {
						uint32_t msz = u32be(trak + tp + mp);
						if (msz < 8 || mp + msz > tsz) break;
						if (type_eq(trak + tp + mp, "hdlr") && msz >= 24 &&
						    type_eq(trak + tp + mp + 16, "soun"))
							is_soun = true;
						mp += msz;
					}
				}
				tp += tsz;
			}

			if (is_soun && audio_idx < (int)names.size() &&
			    !names[audio_idx].empty()) {
				auto new_trak = rebuild_trak_box(trak, sz, names[audio_idx]);
				new_moov_children.insert(new_moov_children.end(),
				                         new_trak.begin(), new_trak.end());
				any_changed = true;
			} else {
				new_moov_children.insert(new_moov_children.end(),
				                         trak, trak + sz);
			}
			if (is_soun) audio_idx++;
		} else {
			new_moov_children.insert(new_moov_children.end(),
			                         mbuf.data() + pos, mbuf.data() + pos + sz);
		}
		pos += sz;
	}

	if (!any_changed) { CloseHandle(h); return false; }

	std::vector<uint8_t> new_moov = make_box("moov", new_moov_children);

	int64_t fsize       = file_size(h);
	bool    moov_at_end = (moov_off + (int64_t)moov_sz >= fsize - 8);
	if (moov_at_end) {
		file_write_at(h, moov_off, new_moov.data(), (DWORD)new_moov.size());
		LARGE_INTEGER li{}; li.QuadPart = moov_off + (int64_t)new_moov.size();
		SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
		SetEndOfFile(h);
	} else {
		// moov not at end — use temp file (rare for OBS recordings)
		CloseHandle(h);
		std::string tmp = mp4_path + ".opp_tmp";
		// Read media data (everything before moov)
		HANDLE hr = CreateFileA(mp4_path.c_str(), GENERIC_READ,
		                        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		                        FILE_ATTRIBUTE_NORMAL, nullptr);
		HANDLE hw = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
		                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hr == INVALID_HANDLE_VALUE || hw == INVALID_HANDLE_VALUE) {
			if (hr != INVALID_HANDLE_VALUE) CloseHandle(hr);
			if (hw != INVALID_HANDLE_VALUE) CloseHandle(hw);
			DeleteFileA(tmp.c_str());
			return false;
		}
		// Copy everything except old moov
		std::vector<uint8_t> buf(1 << 20);
		LARGE_INTEGER li{}; SetFilePointerEx(hr, li, nullptr, FILE_BEGIN);
		int64_t copied = 0;
		while (copied < fsize) {
			int64_t remaining = fsize - copied;
			if (copied == moov_off) { // skip old moov
				li.QuadPart = moov_off + moov_sz;
				SetFilePointerEx(hr, li, nullptr, FILE_BEGIN);
				copied = moov_off + moov_sz;
				continue;
			}
			DWORD to_read = (DWORD)(std::min)((int64_t)buf.size(), remaining);
			if (copied < moov_off && copied + to_read > moov_off)
				to_read = (DWORD)(moov_off - copied);
			DWORD got = 0, written = 0;
			ReadFile(hr, buf.data(), to_read, &got, nullptr);
			if (!got) break;
			WriteFile(hw, buf.data(), got, &written, nullptr);
			copied += got;
		}
		// Append new moov
		DWORD ww = 0;
		WriteFile(hw, new_moov.data(), (DWORD)new_moov.size(), &ww, nullptr);
		CloseHandle(hr); CloseHandle(hw);
		MoveFileExA(tmp.c_str(), mp4_path.c_str(),
		            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	}

	CloseHandle(h);
	return true;
}
