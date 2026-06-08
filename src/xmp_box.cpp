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
	static const char needle[] = "obs-marker-patch";
	size_t            nlen     = sizeof(needle) - 1;
	for (size_t i = 0; i + nlen <= xmp_len; i++) {
		if (memcmp(xmp_payload + i, needle, nlen) == 0)
			return true;
	}
	return false;
}
