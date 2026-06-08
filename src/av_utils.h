#pragma once
#include <string>
#include <vector>

/*
 * av_utils — wraps libavformat/avcodec for chapter reading, duration
 * measurement, MKV→MP4 remux, and audio-tail A/V trim.
 *
 * Links against OBS's bundled FFmpeg DLLs (avformat, avcodec, avutil).
 * No subprocess required.
 */

struct AvChapter {
	double      time_sec;
	std::string name;
};

// Read chapter list from any container.
std::vector<AvChapter> av_get_chapters(const std::string &path);

// Return the video stream time_base denominator (e.g. 60 for 60fps HEVC).
double av_get_video_timescale(const std::string &path);

// Get container duration + first audio stream duration (both in seconds).
// Returns false if either cannot be determined.
bool av_get_durations(const std::string &path, double &video_dur,
                      double &audio_dur);

// Remux src_path (MKV or any container) → dst_path (MP4) using stream copy.
// Returns true on success.
bool av_remux_to_mp4(const std::string &src_path, const std::string &dst_path);

// Trim the file so the video ends at the audio's natural end (fixes AAC
// frame-drop tail).  Operates losslessly (stream copy) via temp→rename.
// Returns true if a trim was performed, false if gap was out of range or
// the audio/video durations couldn't be determined.
bool av_trim_to_audio(const std::string &mp4_path);
