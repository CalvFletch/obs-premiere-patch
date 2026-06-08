#pragma once
#include <string>

/*
 * Pure C++ MP4 box manipulation — no external dependencies.
 *
 * Targets the moov.udta.XMP_ box inside MP4 files.
 * OBS hybrid_mp4 output always writes moov at the end of the file,
 * so in-place truncate+append is used (no temp file needed for those).
 * Files with moov at the start fall back to a temp-file rename.
 */

// Inject (or replace) the XMP_ box inside moov.udta with xmp_content.
// Returns true on success.
bool xmp_inject(const std::string &mp4_path, const std::string &xmp_content);

// Returns true if the file already contains our obs-premiere-patch XMP.
bool xmp_has_ours(const std::string &mp4_path);
