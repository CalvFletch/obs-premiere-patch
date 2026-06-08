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

// Patch the video tkhd duration (and mvhd) to match the audio tkhd duration.
// Returns true if a patch was applied, false if nothing needed or on error.
bool xmp_fix_tkhd_durations(const std::string &mp4_path);

// Write OBS track names into each audio trak's hdlr name field by reading
// the existing trak/udta/name boxes OBS writes at record time.
// Returns true if at least one name was written.
bool xmp_write_hdlr_names(const std::string &mp4_path);

// Write recording creation date into moov/udta/meta/ilst/©day.
// Reads creation_time from the mvhd box (already set by OBS) and converts
// it to ISO 8601. Returns true on success.
bool xmp_write_creation_date(const std::string &mp4_path);

// Patch status values written to ADS (:obs-pp) and moov/udta/OBPS.
// Each patch (trim, markers, names, date) uses one byte independently.
#define OPP_STATUS_NONE      00  // not yet started
#define OPP_STATUS_PATCHING  10  // started, not finished (crash here → retry)
#define OPP_STATUS_DONE      11  // finished successfully

// Read status bytes [trim, markers, names, date] from ADS (fast) or OBPS box (fallback).
// Returns false if neither source exists (old/non-OBS file → treat as unknown).
bool xmp_read_status(const std::string &mp4_path,
                     uint8_t *trim_st, uint8_t *markers_st,
                     uint8_t *names_st, uint8_t *date_st);

// Write status bytes [trim, markers, names, date].  ADS is always written (works before moov).
// OBPS in moov is updated if it already exists, or injected if moov is present.
bool xmp_write_status(const std::string &mp4_path,
                      uint8_t trim_st, uint8_t markers_st,
                      uint8_t names_st, uint8_t date_st);
