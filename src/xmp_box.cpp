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
                     uint8_t trim_st, uint8_t markers_st, uint8_t names_st, uint8_t date_st)
{
	uint8_t              pl[4] = {trim_st, markers_st, names_st, date_st};
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
			uint32_t sz32 = u32be(hdr);
			int64_t  sz   = sz32;
			if (sz32 == 1) {
				// Extended 64-bit size: next 8 bytes hold the real size
				uint8_t ext[8] = {};
				if (!ReadFile(h, ext, 8, &got, nullptr) || got < 8) break;
				sz = (int64_t)(((uint64_t)ext[0] << 56) | ((uint64_t)ext[1] << 48) |
				               ((uint64_t)ext[2] << 40) | ((uint64_t)ext[3] << 32) |
				               ((uint64_t)ext[4] << 24) | ((uint64_t)ext[5] << 16) |
				               ((uint64_t)ext[6] <<  8) |  (uint64_t)ext[7]);
			}
			if (sz < 8) break;
			if (type_eq(hdr, "moov")) { *moov_off = pos; *moov_sz = sz32; break; }
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
                     uint8_t *trim_st, uint8_t *markers_st,
                     uint8_t *names_st, uint8_t *date_st)
{
	*trim_st = *markers_st = *names_st = *date_st = OPP_STATUS_NONE;

	// --- Fast path: NTFS Alternate Data Stream ---
	std::string ads = mp4_path + ":obs-pp";
	HANDLE ha = CreateFileA(ads.c_str(), GENERIC_READ,
	                        FILE_SHARE_READ | FILE_SHARE_WRITE,
	                        nullptr, OPEN_EXISTING,
	                        FILE_ATTRIBUTE_NORMAL, nullptr);
	if (ha != INVALID_HANDLE_VALUE) {
		uint8_t flags[4] = {}; DWORD got = 0;
		ReadFile(ha, flags, 4, &got, nullptr);
		CloseHandle(ha);
		if (got >= 2) {
			*trim_st    = flags[0];
			*markers_st = flags[1];
			*names_st   = (got >= 3) ? flags[2] : OPP_STATUS_NONE;
			*date_st    = (got >= 4) ? flags[3] : OPP_STATUS_NONE;
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

	uint8_t flags[4] = {}; DWORD got2 = 0;
	LARGE_INTEGER li{}; li.QuadPart = status_off;
	SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
	ReadFile(h, flags, 4, &got2, nullptr);
	CloseHandle(h);
	if (got2 < 2) return false;
	*trim_st    = flags[0];
	*markers_st = flags[1];
	*names_st   = (got2 >= 3) ? flags[2] : OPP_STATUS_NONE;
	*date_st    = (got2 >= 4) ? flags[3] : OPP_STATUS_NONE;
	return true;
}

bool xmp_write_status(const std::string &mp4_path,
                      uint8_t trim_st, uint8_t markers_st,
                      uint8_t names_st, uint8_t date_st)
{
	uint8_t flags[4] = {trim_st, markers_st, names_st, date_st};

	// --- Always write ADS (works before moov exists, O(1) seek+write) ---
	{
		std::string ads = mp4_path + ":obs-pp";
		HANDLE ha = CreateFileA(ads.c_str(), GENERIC_WRITE,
		                        FILE_SHARE_READ, nullptr,
		                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (ha != INVALID_HANDLE_VALUE) {
			DWORD w; WriteFile(ha, flags, 4, &w, nullptr);
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

	// If OBPS already exists: 4-byte in-place write.
	int64_t status_off = find_obps_off(mbuf.data(), moov_sz, moov_off);
	if (status_off >= 0) {
		file_write_at(h, status_off, flags, 4);
		CloseHandle(h); return true;
	}

	// OBPS doesn't exist — rebuild moov/udta to inject it.
	BoxInfo udta = find_box_in_buf(mbuf.data(), moov_sz, "udta", 8);
	std::vector<uint8_t> new_udta_inner;
	if (udta.found) {
		new_udta_inner = build_udta_with_obps(
			mbuf.data() + udta.offset + 8,
			(size_t)udta.total_size - 8,
			trim_st, markers_st, names_st, date_st);
	} else {
		uint8_t pl[4] = {trim_st, markers_st, names_st, date_st};
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
// For each audio trak, reads the name stored in trak/udta/name (written by
// OBS) and copies it into the trak/mdia/hdlr name field so Premiere Pro
// shows the real source name instead of "OBS Audio Handler".
// Works on any OBS hybrid_mp4 recording — no runtime capture needed.
// ---------------------------------------------------------------------------

// Helper: find a direct child box of given type inside a buffer slice.
// Returns pointer into buf and sets *found_sz, or nullptr if not found.
static const uint8_t *find_child(const uint8_t *buf, size_t len,
                                 const char *type4, uint32_t *found_sz)
{
	size_t p = 0;
	while (p + 8 <= len) {
		uint32_t sz = u32be(buf + p);
		if (sz < 8 || p + sz > len) break;
		if (type_eq(buf + p, type4)) { if (found_sz) *found_sz = sz; return buf + p; }
		p += sz;
	}
	return nullptr;
}

// Read the null-terminated string from a trak/udta/name box.
// box points to the 8-byte "name" box header; box_sz is full box size.
static std::string read_name_box(const uint8_t *box, uint32_t box_sz)
{
	// name box layout: [4 size][4 "name"][4 locale][string...]  (QuickTime style)
	// OR simply:       [4 size][4 "name"][string...]             (bare, no locale)
	// OBS writes the bare form.
	if (box_sz <= 8) return {};
	const uint8_t *data = box + 8;
	size_t         data_len = box_sz - 8;
	// If first 4 bytes look like a locale int (high byte 0), skip them.
	// OBS does NOT write a locale, so we don't skip.
	size_t end = 0;
	while (end < data_len && data[end] != 0) end++;
	return std::string(reinterpret_cast<const char *>(data), end);
}

// Build a new hdlr box with the name field replaced.
static std::vector<uint8_t>
rebuild_hdlr_box(const uint8_t *hdlr, uint32_t sz, const std::string &new_name)
{
	if (sz < 32) return std::vector<uint8_t>(hdlr, hdlr + sz);
	std::vector<uint8_t> payload(hdlr + 8, hdlr + 32); // 24 fixed bytes
	for (char c : new_name) payload.push_back((uint8_t)c);
	payload.push_back(0);
	return make_box("hdlr", payload);
}

bool xmp_write_hdlr_names(const std::string &mp4_path)
{
	HANDLE h = CreateFileA(mp4_path.c_str(), GENERIC_READ | GENERIC_WRITE,
	                       FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;

	int64_t              moov_off; uint32_t moov_sz;
	std::vector<uint8_t> mbuf;
	if (!read_moov_only(h, &moov_off, &moov_sz, &mbuf)) {
		CloseHandle(h); return false;
	}

	bool                 any_changed = false;
	std::vector<uint8_t> new_moov_children;

	size_t pos = 8;
	while (pos + 8 <= moov_sz) {
		uint32_t sz = u32be(mbuf.data() + pos);
		if (sz < 8 || pos + sz > moov_sz) break;

		if (type_eq(mbuf.data() + pos, "trak")) {
			const uint8_t *trak = mbuf.data() + pos;

			// Read trak/udta/name
			std::string udta_name;
			uint32_t udta_sz = 0;
			const uint8_t *udta = find_child(trak + 8, sz - 8, "udta", &udta_sz);
			if (udta) {
				uint32_t name_sz = 0;
				const uint8_t *name_box = find_child(udta + 8, udta_sz - 8, "name", &name_sz);
				if (name_box) udta_name = read_name_box(name_box, name_sz);
			}

			// Find mdia/hdlr to check handler type
			bool is_soun = false;
			uint32_t mdia_sz = 0;
			const uint8_t *mdia = find_child(trak + 8, sz - 8, "mdia", &mdia_sz);
			if (mdia) {
				uint32_t hdlr_sz = 0;
				const uint8_t *hdlr = find_child(mdia + 8, mdia_sz - 8, "hdlr", &hdlr_sz);
				if (hdlr && hdlr_sz >= 24 && type_eq(hdlr + 12, "soun"))
					is_soun = true;
			}

			if (is_soun && !udta_name.empty()) {
				// Rebuild trak: replace hdlr name inside mdia, keep everything else
				std::vector<uint8_t> new_trak_children;
				size_t tp = 8;
				while (tp + 8 <= sz) {
					uint32_t csz = u32be(trak + tp);
					if (csz < 8 || tp + csz > sz) break;
					if (type_eq(trak + tp, "mdia")) {
						// Rebuild mdia: replace hdlr, keep everything else
						std::vector<uint8_t> new_mdia_children;
						size_t mp = 8;
						while (mp + 8 <= csz) {
							uint32_t msz = u32be(trak + tp + mp);
							if (msz < 8 || mp + msz > csz) break;
							if (type_eq(trak + tp + mp, "hdlr")) {
								auto new_hdlr = rebuild_hdlr_box(trak + tp + mp, msz, udta_name);
								new_mdia_children.insert(new_mdia_children.end(),
								                          new_hdlr.begin(), new_hdlr.end());
							} else {
								new_mdia_children.insert(new_mdia_children.end(),
								                          trak + tp + mp,
								                          trak + tp + mp + msz);
							}
							mp += msz;
						}
						auto new_mdia = make_box("mdia", new_mdia_children);
						new_trak_children.insert(new_trak_children.end(),
						                         new_mdia.begin(), new_mdia.end());
					} else {
						new_trak_children.insert(new_trak_children.end(),
						                         trak + tp, trak + tp + csz);
					}
					tp += csz;
				}
				auto new_trak = make_box("trak", new_trak_children);
				new_moov_children.insert(new_moov_children.end(),
				                         new_trak.begin(), new_trak.end());
				any_changed = true;
			} else {
				new_moov_children.insert(new_moov_children.end(),
				                         trak, trak + sz);
			}
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
		CloseHandle(h);
	} else {
		// moov not at end — use temp file (rare for OBS recordings)
		CloseHandle(h);
		std::string tmp = mp4_path + ".opp_tmp";
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
		std::vector<uint8_t> buf(1 << 20);
		LARGE_INTEGER li{}; SetFilePointerEx(hr, li, nullptr, FILE_BEGIN);
		int64_t copied = 0;
		while (copied < fsize) {
			int64_t remaining = fsize - copied;
			if (copied == moov_off) {
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
		DWORD ww = 0;
		WriteFile(hw, new_moov.data(), (DWORD)new_moov.size(), &ww, nullptr);
		CloseHandle(hr); CloseHandle(hw);
		MoveFileExA(tmp.c_str(), mp4_path.c_str(),
		            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	}

	return true;
}

// ---------------------------------------------------------------------------
// xmp_write_creation_date
// ---------------------------------------------------------------------------
// Reads the creation_time field from the mvhd box (set by OBS to the
// wall-clock start time), converts it to ISO 8601, and writes it into
// moov/udta/meta/ilst/©day so Premiere Pro shows the correct date.
// Works on any existing OBS hybrid_mp4 recording — no capture needed.
// ---------------------------------------------------------------------------

#include <ctime>

// Convert Mac epoch (1904-01-01) to ISO 8601 UTC string.
static std::string mac_time_to_iso8601(uint64_t mac_secs)
{
	const uint64_t MAC_TO_UNIX = 2082844800ULL;
	if (mac_secs < MAC_TO_UNIX) return {};
	time_t unix_t = (time_t)(mac_secs - MAC_TO_UNIX);
	struct tm t;
#if defined(_WIN32)
	if (gmtime_s(&t, &unix_t) != 0) return {};
#else
	if (!gmtime_r(&unix_t, &t)) return {};
#endif
	char buf[32];
	strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
	return buf;
}

// Build an iTunes-style ©day box (0xA9 + "day") with a data sub-box.
static std::vector<uint8_t> make_cday_box(const std::string &iso8601)
{
	// data box: [size][4 "data"][4 type=1 UTF-8][4 locale=0][string]
	std::vector<uint8_t> data_payload;
	data_payload.push_back(0); data_payload.push_back(0);
	data_payload.push_back(0); data_payload.push_back(1); // type = UTF-8
	data_payload.push_back(0); data_payload.push_back(0);
	data_payload.push_back(0); data_payload.push_back(0); // locale = 0
	for (char c : iso8601) data_payload.push_back((uint8_t)c);
	auto data_box = make_box("data", data_payload);

	// ©day key: 0xA9 'd' 'a' 'y'
	uint32_t total = 8 + (uint32_t)data_box.size();
	std::vector<uint8_t> box;
	box.push_back((total >> 24) & 0xFF); box.push_back((total >> 16) & 0xFF);
	box.push_back((total >>  8) & 0xFF); box.push_back( total        & 0xFF);
	box.push_back(0xA9); box.push_back('d'); box.push_back('a'); box.push_back('y');
	box.insert(box.end(), data_box.begin(), data_box.end());
	return box;
}

// Rebuild an ilst payload: drop existing ©day, append new one.
static std::vector<uint8_t>
rebuild_ilst(const uint8_t *ilst, uint32_t ilst_sz, const std::vector<uint8_t> &cday)
{
	std::vector<uint8_t> out;
	size_t p = 8; // skip ilst header
	while (p + 8 <= ilst_sz) {
		uint32_t csz = u32be(ilst + p);
		if (csz < 8 || p + csz > ilst_sz) break;
		// Skip existing ©day (0xA9 'd' 'a' 'y')
		bool is_cday = (ilst[p+4] == 0xA9 && ilst[p+5] == 'd' &&
		                ilst[p+6] == 'a'  && ilst[p+7] == 'y');
		if (!is_cday)
			out.insert(out.end(), ilst + p, ilst + p + csz);
		p += csz;
	}
	out.insert(out.end(), cday.begin(), cday.end());
	return out;
}

bool xmp_write_creation_date(const std::string &mp4_path)
{
	HANDLE h = CreateFileA(mp4_path.c_str(), GENERIC_READ | GENERIC_WRITE,
	                       FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;

	int64_t              moov_off; uint32_t moov_sz;
	std::vector<uint8_t> mbuf;
	if (!read_moov_only(h, &moov_off, &moov_sz, &mbuf)) {
		CloseHandle(h); return false;
	}
	const uint8_t *moov = mbuf.data();

	// --- Read creation_time from mvhd ---
	std::string iso_date;
	{
		size_t p = 8;
		while (p + 8 <= moov_sz) {
			uint32_t sz = u32be(moov + p);
			if (sz < 8 || p + sz > moov_sz) break;
			if (type_eq(moov + p, "mvhd") && sz >= 28) {
				uint8_t ver = moov[p + 8];
				uint64_t mac_time = (ver == 1 && sz >= 36)
				                  ? u64be(moov + p + 12)
				                  : (uint64_t)u32be(moov + p + 12);
				iso_date = mac_time_to_iso8601(mac_time);
				break;
			}
			p += sz;
		}
	}
	if (iso_date.empty()) { CloseHandle(h); return false; }

	auto cday_box = make_cday_box(iso_date);

	// --- Rebuild moov: udta → meta → ilst with ©day ---
	std::vector<uint8_t> new_moov_children;
	bool found_udta = false;

	size_t pos = 8;
	while (pos + 8 <= moov_sz) {
		uint32_t sz = u32be(moov + pos);
		if (sz < 8 || pos + sz > moov_sz) break;

		if (type_eq(moov + pos, "udta")) {
			found_udta = true;
			const uint8_t *udta = moov + pos;

			// Rebuild udta: find/create meta
			std::vector<uint8_t> new_udta_children;
			bool found_meta = false;
			size_t up = 8;
			while (up + 8 <= sz) {
				uint32_t usz = u32be(udta + up);
				if (usz < 8 || up + usz > sz) break;

				if (type_eq(udta + up, "meta")) {
					found_meta = true;
					const uint8_t *meta = udta + up;
					// meta has 4-byte version/flags before children
					size_t meta_children_start = 12;

					// Rebuild meta: find/create ilst
					std::vector<uint8_t> new_meta_children(
					    meta + 8, meta + 12); // keep ver/flags
					bool found_ilst = false;
					size_t mp = meta_children_start;
					while (mp + 8 <= usz) {
						uint32_t msz = u32be(meta + mp);
						if (msz < 8 || mp + msz > usz) break;
						if (type_eq(meta + mp, "ilst")) {
							found_ilst = true;
							auto new_ilst_children = rebuild_ilst(
							    meta + mp, msz, cday_box);
							auto new_ilst = make_box("ilst", new_ilst_children);
							new_meta_children.insert(new_meta_children.end(),
							    new_ilst.begin(), new_ilst.end());
						} else {
							new_meta_children.insert(new_meta_children.end(),
							    meta + mp, meta + mp + msz);
						}
						mp += msz;
					}
					if (!found_ilst) {
						// Create ilst with just ©day
						auto new_ilst = make_box("ilst", cday_box);
						new_meta_children.insert(new_meta_children.end(),
						    new_ilst.begin(), new_ilst.end());
					}
					// Build new meta box (size + "meta" + children)
					uint32_t new_meta_sz = 8 + (uint32_t)new_meta_children.size();
					std::vector<uint8_t> new_meta_box;
					new_meta_box.push_back((new_meta_sz>>24)&0xFF);
					new_meta_box.push_back((new_meta_sz>>16)&0xFF);
					new_meta_box.push_back((new_meta_sz>> 8)&0xFF);
					new_meta_box.push_back( new_meta_sz     &0xFF);
					new_meta_box.push_back('m'); new_meta_box.push_back('e');
					new_meta_box.push_back('t'); new_meta_box.push_back('a');
					new_meta_box.insert(new_meta_box.end(),
					    new_meta_children.begin(), new_meta_children.end());
					new_udta_children.insert(new_udta_children.end(),
					    new_meta_box.begin(), new_meta_box.end());
				} else {
					new_udta_children.insert(new_udta_children.end(),
					    udta + up, udta + up + usz);
				}
				up += usz;
			}
			if (!found_meta) {
				// Create meta/hdlr/ilst from scratch
				std::vector<uint8_t> meta_children;
				// ver/flags = 0
				meta_children.push_back(0); meta_children.push_back(0);
				meta_children.push_back(0); meta_children.push_back(0);
				// hdlr: mdir/appl
				uint8_t hdlr_pl[21] = {0,0,0,0, 0,0,0,0,
				                       'm','d','i','r',
				                       0,0,0,0, 0,0,0,0, 0};
				auto hdlr = make_box("hdlr", std::vector<uint8_t>(hdlr_pl, hdlr_pl+21));
				meta_children.insert(meta_children.end(), hdlr.begin(), hdlr.end());
				auto new_ilst = make_box("ilst", cday_box);
				meta_children.insert(meta_children.end(), new_ilst.begin(), new_ilst.end());
				uint32_t new_meta_sz = 8 + (uint32_t)meta_children.size();
				std::vector<uint8_t> new_meta_box;
				new_meta_box.push_back((new_meta_sz>>24)&0xFF);
				new_meta_box.push_back((new_meta_sz>>16)&0xFF);
				new_meta_box.push_back((new_meta_sz>> 8)&0xFF);
				new_meta_box.push_back( new_meta_sz     &0xFF);
				new_meta_box.push_back('m'); new_meta_box.push_back('e');
				new_meta_box.push_back('t'); new_meta_box.push_back('a');
				new_meta_box.insert(new_meta_box.end(), meta_children.begin(), meta_children.end());
				new_udta_children.insert(new_udta_children.end(),
				    new_meta_box.begin(), new_meta_box.end());
			}
			auto new_udta = make_box("udta", new_udta_children);
			new_moov_children.insert(new_moov_children.end(),
			    new_udta.begin(), new_udta.end());
		} else {
			new_moov_children.insert(new_moov_children.end(),
			    moov + pos, moov + pos + sz);
		}
		pos += sz;
	}

	if (!found_udta) {
		// No udta at all — copy existing moov children then append new udta
		new_moov_children.clear();
		size_t p2 = 8;
		while (p2 + 8 <= moov_sz) {
			uint32_t sz = u32be(moov + p2);
			if (sz < 8 || p2 + sz > moov_sz) break;
			new_moov_children.insert(new_moov_children.end(),
			    moov + p2, moov + p2 + sz);
			p2 += sz;
		}
		// Build fresh udta/meta/ilst/©day
		std::vector<uint8_t> meta_children;
		meta_children.push_back(0); meta_children.push_back(0);
		meta_children.push_back(0); meta_children.push_back(0);
		uint8_t hdlr_pl[21] = {0,0,0,0, 0,0,0,0,
		                       'm','d','i','r',
		                       0,0,0,0, 0,0,0,0, 0};
		auto hdlr = make_box("hdlr", std::vector<uint8_t>(hdlr_pl, hdlr_pl+21));
		meta_children.insert(meta_children.end(), hdlr.begin(), hdlr.end());
		auto new_ilst = make_box("ilst", cday_box);
		meta_children.insert(meta_children.end(), new_ilst.begin(), new_ilst.end());
		uint32_t new_meta_sz = 8 + (uint32_t)meta_children.size();
		std::vector<uint8_t> new_meta_box;
		new_meta_box.push_back((new_meta_sz>>24)&0xFF);
		new_meta_box.push_back((new_meta_sz>>16)&0xFF);
		new_meta_box.push_back((new_meta_sz>> 8)&0xFF);
		new_meta_box.push_back( new_meta_sz     &0xFF);
		new_meta_box.push_back('m'); new_meta_box.push_back('e');
		new_meta_box.push_back('t'); new_meta_box.push_back('a');
		new_meta_box.insert(new_meta_box.end(), meta_children.begin(), meta_children.end());
		auto new_udta = make_box("udta", new_meta_box);
		new_moov_children.insert(new_moov_children.end(),
		    new_udta.begin(), new_udta.end());
	}

	std::vector<uint8_t> new_moov = make_box("moov", new_moov_children);
	int64_t fsize       = file_size(h);
	bool    moov_at_end = (moov_off + (int64_t)moov_sz >= fsize - 8);
	if (moov_at_end) {
		file_write_at(h, moov_off, new_moov.data(), (DWORD)new_moov.size());
		LARGE_INTEGER li{}; li.QuadPart = moov_off + (int64_t)new_moov.size();
		SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
		SetEndOfFile(h);
		CloseHandle(h);
	} else {
		CloseHandle(h);
		std::string tmp = mp4_path + ".opp_tmp";
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
		std::vector<uint8_t> buf(1 << 20);
		LARGE_INTEGER li{}; SetFilePointerEx(hr, li, nullptr, FILE_BEGIN);
		int64_t copied = 0;
		while (copied < fsize) {
			int64_t remaining = fsize - copied;
			if (copied == moov_off) {
				li.QuadPart = moov_off + moov_sz;
				SetFilePointerEx(hr, li, nullptr, FILE_BEGIN);
				copied = moov_off + moov_sz; continue;
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
		DWORD ww = 0;
		WriteFile(hw, new_moov.data(), (DWORD)new_moov.size(), &ww, nullptr);
		CloseHandle(hr); CloseHandle(hw);
		MoveFileExA(tmp.c_str(), mp4_path.c_str(),
		            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	}
	return true;
}

// ---------------------------------------------------------------------------
// xmp_normalize_stts
// ---------------------------------------------------------------------------
// Hardware encoders (NVENC/AMF/QSV) write variable frame intervals into the
// stts (sample-to-time) box even in CBR mode.  Premiere Pro stutters when
// scrubbing at 2x/4x speed because it uses stts to seek to frame positions.
//
// Fix: replace the multi-entry stts with a single uniform-delta entry snapped
// to the nearest standard frame rate (e.g. 30000/1001 ≈ 29.97 fps).
// Also updates mdhd.duration = n_frames × ideal_delta for consistency.
// Safe to run multiple times — returns false immediately if already CFR.
// ---------------------------------------------------------------------------

struct VfrFpsCand { uint32_t num, den; };
static const VfrFpsCand VFR_FPS_CANDS[] = {
	{24000, 1001}, {24, 1}, {25, 1},
	{30000, 1001}, {30, 1},
	{48000, 1001}, {48, 1}, {50, 1},
	{60000, 1001}, {60, 1},
	{120, 1},
};

// Return ideal delta (timescale ticks per frame) snapped to nearest standard
// FPS, or 0 if the error exceeds 0.5% (likely genuinely non-standard).
static uint32_t snap_cfr_delta(uint32_t ts, uint64_t dur, uint64_t n_frames)
{
	if (n_frames == 0 || ts == 0) return 0;
	double avg = (double)dur / (double)n_frames;
	uint32_t best = 0;
	double   best_err = 1.0;
	for (const auto &c : VFR_FPS_CANDS) {
		double   ideal = (double)ts * (double)c.den / (double)c.num;
		if (ideal < 1.0) continue;
		uint32_t delta = (uint32_t)(ideal + 0.5);
		double   err   = fabs((double)delta - avg) / avg;
		if (err < best_err) { best_err = err; best = delta; }
	}
	return (best_err <= 0.005) ? best : 0u;
}

static uint64_t stts_count_frames(const uint8_t *stts, uint32_t sz)
{
	if (sz < 16) return 0;
	uint32_t n = u32be(stts + 12);
	if ((uint64_t)16 + (uint64_t)n * 8 > sz) return 0;
	uint64_t total = 0;
	for (uint32_t i = 0; i < n; i++)
		total += u32be(stts + 16 + i * 8);
	return total;
}

static std::vector<uint8_t> make_stts_cfr(uint64_t n_frames, uint32_t delta)
{
	// stts payload: [4 ver+flags=0][4 entry_count=1][4 sample_count][4 delta]
	std::vector<uint8_t> p(16);
	pu32be(p.data(),      0);
	pu32be(p.data() + 4,  1);
	pu32be(p.data() + 8,  (uint32_t)n_frames);
	pu32be(p.data() + 12, delta);
	return make_box("stts", p);
}

bool xmp_normalize_stts(const std::string &mp4_path)
{
	HANDLE h = CreateFileA(mp4_path.c_str(), GENERIC_READ | GENERIC_WRITE,
	                       FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;

	int64_t              moov_off; uint32_t moov_sz;
	std::vector<uint8_t> mbuf;
	if (!read_moov_only(h, &moov_off, &moov_sz, &mbuf)) {
		CloseHandle(h); return false;
	}
	const uint8_t *moov = mbuf.data();

	bool                 any_changed = false;
	std::vector<uint8_t> new_moov_children;

	size_t pos = 8;
	while (pos + 8 <= moov_sz) {
		uint32_t sz = u32be(moov + pos);
		if (sz < 8 || pos + sz > moov_sz) break;

		if (!type_eq(moov + pos, "trak")) {
			new_moov_children.insert(new_moov_children.end(),
			                         moov + pos, moov + pos + sz);
			pos += sz; continue;
		}

		const uint8_t *trak = moov + pos;

		// Check for video trak
		bool           is_vide  = false;
		uint32_t       mdia_sz  = 0;
		const uint8_t *mdia = find_child(trak + 8, sz - 8, "mdia", &mdia_sz);
		if (mdia) {
			uint32_t       hdlr_sz = 0;
			const uint8_t *hdlr = find_child(mdia + 8, mdia_sz - 8, "hdlr", &hdlr_sz);
			if (hdlr && hdlr_sz >= 24 && type_eq(hdlr + 12, "vide"))
				is_vide = true;
		}

		if (!is_vide || !mdia) {
			new_moov_children.insert(new_moov_children.end(), trak, trak + sz);
			pos += sz; continue;
		}

		// Navigate to stts
		uint32_t       minf_sz = 0;
		const uint8_t *minf = find_child(mdia + 8, mdia_sz - 8, "minf", &minf_sz);
		uint32_t       stbl_sz = 0;
		const uint8_t *stbl = minf ? find_child(minf + 8, minf_sz - 8, "stbl", &stbl_sz) : nullptr;
		uint32_t       stts_sz = 0;
		const uint8_t *stts = stbl ? find_child(stbl + 8, stbl_sz - 8, "stts", &stts_sz) : nullptr;

		if (!stts || (stts_sz >= 16 && u32be(stts + 12) == 1)) {
			// No stts or already a single-entry (CFR) stts — copy as-is
			new_moov_children.insert(new_moov_children.end(), trak, trak + sz);
			pos += sz; continue;
		}

		// Read mdhd timescale + duration
		uint32_t mdhd_ts = 0; uint64_t mdhd_dur = 0; int mdhd_ver = -1;
		{
			uint32_t       mdhd_sz = 0;
			const uint8_t *mdhd = find_child(mdia + 8, mdia_sz - 8, "mdhd", &mdhd_sz);
			if (mdhd && mdhd_sz >= 24) {
				mdhd_ver = mdhd[8];
				if (mdhd_ver == 0) { mdhd_ts = u32be(mdhd + 20); mdhd_dur = u32be(mdhd + 24); }
				else               { mdhd_ts = u32be(mdhd + 28); mdhd_dur = u64be(mdhd + 32); }
			}
		}
		if (mdhd_ts == 0 || mdhd_dur == 0) {
			new_moov_children.insert(new_moov_children.end(), trak, trak + sz);
			pos += sz; continue;
		}

		uint64_t n_frames    = stts_count_frames(stts, stts_sz);
		uint32_t ideal_delta = n_frames ? snap_cfr_delta(mdhd_ts, mdhd_dur, n_frames) : 0;
		if (ideal_delta == 0) {
			new_moov_children.insert(new_moov_children.end(), trak, trak + sz);
			pos += sz; continue;
		}

		uint64_t             new_mdhd_dur = n_frames * (uint64_t)ideal_delta;
		std::vector<uint8_t> new_stts     = make_stts_cfr(n_frames, ideal_delta);

		// ── Rebuild stbl: replace stts, keep everything else ──────────────
		std::vector<uint8_t> new_stbl_ch;
		for (size_t sp = 8; sp + 8 <= stbl_sz;) {
			uint32_t csz = u32be(stbl + sp);
			if (csz < 8 || sp + csz > stbl_sz) break;
			if (type_eq(stbl + sp, "stts"))
				new_stbl_ch.insert(new_stbl_ch.end(), new_stts.begin(), new_stts.end());
			else
				new_stbl_ch.insert(new_stbl_ch.end(), stbl + sp, stbl + sp + csz);
			sp += csz;
		}
		auto new_stbl = make_box("stbl", new_stbl_ch);

		// ── Rebuild minf: replace stbl ────────────────────────────────────
		std::vector<uint8_t> new_minf_ch;
		for (size_t mp2 = 8; mp2 + 8 <= minf_sz;) {
			uint32_t csz = u32be(minf + mp2);
			if (csz < 8 || mp2 + csz > minf_sz) break;
			if (type_eq(minf + mp2, "stbl"))
				new_minf_ch.insert(new_minf_ch.end(), new_stbl.begin(), new_stbl.end());
			else
				new_minf_ch.insert(new_minf_ch.end(), minf + mp2, minf + mp2 + csz);
			mp2 += csz;
		}
		auto new_minf = make_box("minf", new_minf_ch);

		// ── Rebuild mdia: replace minf + patch mdhd.duration ─────────────
		std::vector<uint8_t> new_mdia_ch;
		for (size_t dp = 8; dp + 8 <= mdia_sz;) {
			uint32_t csz = u32be(mdia + dp);
			if (csz < 8 || dp + csz > mdia_sz) break;
			if (type_eq(mdia + dp, "minf")) {
				new_mdia_ch.insert(new_mdia_ch.end(), new_minf.begin(), new_minf.end());
			} else if (type_eq(mdia + dp, "mdhd")) {
				std::vector<uint8_t> new_mdhd(mdia + dp, mdia + dp + csz);
				if (mdhd_ver == 0 && csz >= 28)
					pu32be(new_mdhd.data() + 24, (uint32_t)new_mdhd_dur);
				else if (mdhd_ver == 1 && csz >= 44)
					pu64be(new_mdhd.data() + 32, new_mdhd_dur);
				new_mdia_ch.insert(new_mdia_ch.end(), new_mdhd.begin(), new_mdhd.end());
			} else {
				new_mdia_ch.insert(new_mdia_ch.end(), mdia + dp, mdia + dp + csz);
			}
			dp += csz;
		}
		auto new_mdia = make_box("mdia", new_mdia_ch);

		// ── Rebuild trak: replace mdia ────────────────────────────────────
		std::vector<uint8_t> new_trak_ch;
		for (size_t tp = 8; tp + 8 <= sz;) {
			uint32_t csz = u32be(trak + tp);
			if (csz < 8 || tp + csz > sz) break;
			if (type_eq(trak + tp, "mdia"))
				new_trak_ch.insert(new_trak_ch.end(), new_mdia.begin(), new_mdia.end());
			else
				new_trak_ch.insert(new_trak_ch.end(), trak + tp, trak + tp + csz);
			tp += csz;
		}
		auto new_trak = make_box("trak", new_trak_ch);
		new_moov_children.insert(new_moov_children.end(), new_trak.begin(), new_trak.end());
		any_changed = true;
		pos += sz;
	}

	if (!any_changed) { CloseHandle(h); return false; }

	std::vector<uint8_t> new_moov    = make_box("moov", new_moov_children);
	int64_t              fsize       = file_size(h);
	bool                 moov_at_end = (moov_off + (int64_t)moov_sz >= fsize - 8);

	if (moov_at_end) {
		file_write_at(h, moov_off, new_moov.data(), (DWORD)new_moov.size());
		LARGE_INTEGER li{}; li.QuadPart = moov_off + (int64_t)new_moov.size();
		SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
		SetEndOfFile(h);
		CloseHandle(h);
	} else {
		CloseHandle(h);
		std::string tmp = mp4_path + ".opp_tmp";
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
		std::vector<uint8_t> copybuf(1 << 20);
		LARGE_INTEGER li{}; SetFilePointerEx(hr, li, nullptr, FILE_BEGIN);
		int64_t copied = 0;
		while (copied < fsize) {
			if (copied == moov_off) {
				li.QuadPart = moov_off + moov_sz;
				SetFilePointerEx(hr, li, nullptr, FILE_BEGIN);
				copied = moov_off + moov_sz; continue;
			}
			int64_t remaining = fsize - copied;
			DWORD   to_read   = (DWORD)(std::min)((int64_t)copybuf.size(), remaining);
			if (copied < moov_off && copied + to_read > moov_off)
				to_read = (DWORD)(moov_off - copied);
			DWORD got = 0, written = 0;
			ReadFile(hr, copybuf.data(), to_read, &got, nullptr);
			if (!got) break;
			WriteFile(hw, copybuf.data(), got, &written, nullptr);
			copied += got;
		}
		DWORD ww = 0;
		WriteFile(hw, new_moov.data(), (DWORD)new_moov.size(), &ww, nullptr);
		CloseHandle(hr); CloseHandle(hw);
		MoveFileExA(tmp.c_str(), mp4_path.c_str(),
		            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	}
	return true;
}
