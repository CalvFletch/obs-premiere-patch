/*
 * av_utils.cpp — libavformat/avcodec wrappers
 *
 * Links at build time against OBS's bundled FFmpeg import libs
 * (avformat.lib, avcodec.lib, avutil.lib).  At runtime the DLLs are
 * already loaded into OBS's process.
 *
 * NOTE: av_log output is suppressed (AV_LOG_QUIET) to keep OBS's log clean.
 */

#include "av_utils.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// One-time FFmpeg log suppression
// ---------------------------------------------------------------------------

static void suppress_av_log()
{
	static bool done = false;
	if (!done) {
		av_log_set_level(AV_LOG_QUIET);
		done = true;
	}
}

// ---------------------------------------------------------------------------
// av_get_chapters
// ---------------------------------------------------------------------------

std::vector<AvChapter> av_get_chapters(const std::string &path)
{
	suppress_av_log();
	std::vector<AvChapter> out;

	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
		return out;
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		return out;
	}

	for (unsigned i = 0; i < fmt->nb_chapters; i++) {
		AVChapter *ch = fmt->chapters[i];
		AVDictionaryEntry *t =
			av_dict_get(ch->metadata, "title", nullptr, 0);
		if (!t || !t->value || t->value[0] == '\0')
			continue;
		AvChapter c;
		c.time_sec = (double)ch->start * av_q2d(ch->time_base);
		c.name     = t->value;
		out.push_back(c);
	}

	avformat_close_input(&fmt);
	return out;
}

// ---------------------------------------------------------------------------
// av_get_video_timescale
// ---------------------------------------------------------------------------

double av_get_video_timescale(const std::string &path)
{
	suppress_av_log();

	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
		return 60.0;
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		return 60.0;
	}

	double result = 60.0;
	for (unsigned i = 0; i < fmt->nb_streams; i++) {
		AVStream *s = fmt->streams[i];
		if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			// time_base denominator = ticks per second for that stream
			if (s->time_base.den > 0 && s->time_base.num > 0) {
				result = (double)s->time_base.den /
				         (double)s->time_base.num;
			}
			break;
		}
	}

	avformat_close_input(&fmt);
	return result;
}

// ---------------------------------------------------------------------------
// av_get_durations
// ---------------------------------------------------------------------------

bool av_get_durations(const std::string &path, double &video_dur,
                      double &audio_dur)
{
	suppress_av_log();

	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
		return false;
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		avformat_close_input(&fmt);
		return false;
	}

	// Video track duration (first video stream)
	video_dur = -1.0;
	for (unsigned i = 0; i < fmt->nb_streams; i++) {
		AVStream *s = fmt->streams[i];
		if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			if (s->duration > 0 && s->time_base.den > 0) {
				video_dur = (double)s->duration *
				            av_q2d(s->time_base);
			}
			break;
		}
	}
	if (video_dur <= 0.0) {
		// Fall back to container duration
		if (fmt->duration > 0)
			video_dur = (double)fmt->duration / (double)AV_TIME_BASE;
		else {
			avformat_close_input(&fmt);
			return false;
		}
	}

	// First audio stream's presented duration = (raw_dur - priming_skip)
	// The elst media_time encodes the AAC priming skip in stream samples.
	// libavformat exposes this as s->start_time (negative = priming frames).
	audio_dur = -1.0;
	for (unsigned i = 0; i < fmt->nb_streams; i++) {
		AVStream *s = fmt->streams[i];
		if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			if (s->duration > 0 && s->time_base.den > 0) {
				double raw = (double)s->duration * av_q2d(s->time_base);
				// start_time is negative by the priming delay
				double priming = (s->start_time != AV_NOPTS_VALUE && s->start_time < 0)
				                     ? -(double)s->start_time * av_q2d(s->time_base)
				                     : 0.0;
				audio_dur = raw - priming;
			}
			break;
		}
	}

	avformat_close_input(&fmt);
	return audio_dur > 0.0;
}

// ---------------------------------------------------------------------------
// av_remux_to_mp4 — stream-copy src → dst
// ---------------------------------------------------------------------------

bool av_remux_to_mp4(const std::string &src_path, const std::string &dst_path)
{
	suppress_av_log();

	AVFormatContext *ifmt = nullptr;
	if (avformat_open_input(&ifmt, src_path.c_str(), nullptr, nullptr) < 0)
		return false;
	if (avformat_find_stream_info(ifmt, nullptr) < 0) {
		avformat_close_input(&ifmt);
		return false;
	}

	AVFormatContext *ofmt = nullptr;
	if (avformat_alloc_output_context2(&ofmt, nullptr, "mp4",
	                                   dst_path.c_str()) < 0) {
		avformat_close_input(&ifmt);
		return false;
	}

	// Map each input stream to an output stream
	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		AVStream *is = ifmt->streams[i];
		AVStream *os = avformat_new_stream(ofmt, nullptr);
		if (!os) {
			avformat_close_input(&ifmt);
			avformat_free_context(ofmt);
			return false;
		}
		avcodec_parameters_copy(os->codecpar, is->codecpar);
		os->codecpar->codec_tag = 0;
		os->time_base            = is->time_base;
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&ofmt->pb, dst_path.c_str(), AVIO_FLAG_WRITE) < 0) {
			avformat_close_input(&ifmt);
			avformat_free_context(ofmt);
			return false;
		}
	}

	AVDictionary *opts = nullptr;
	// Write chapters from source
	if (ifmt->nb_chapters > 0) {
		ofmt->chapters = (AVChapter **)av_malloc(
			sizeof(AVChapter *) * ifmt->nb_chapters);
		if (ofmt->chapters) {
			ofmt->nb_chapters = ifmt->nb_chapters;
			for (unsigned i = 0; i < ifmt->nb_chapters; i++) {
				ofmt->chapters[i] = (AVChapter *)av_mallocz(
					sizeof(AVChapter));
				if (ofmt->chapters[i]) {
					*ofmt->chapters[i] = *ifmt->chapters[i];
					ofmt->chapters[i]->metadata = nullptr;
					av_dict_copy(
						&ofmt->chapters[i]->metadata,
						ifmt->chapters[i]->metadata, 0);
				}
			}
		}
	}

	if (avformat_write_header(ofmt, &opts) < 0) {
		if (!(ofmt->oformat->flags & AVFMT_NOFILE))
			avio_closep(&ofmt->pb);
		avformat_close_input(&ifmt);
		avformat_free_context(ofmt);
		av_dict_free(&opts);
		return false;
	}
	av_dict_free(&opts);

	AVPacket *pkt = av_packet_alloc();
	bool      ok  = (pkt != nullptr);

	while (ok) {
		int ret = av_read_frame(ifmt, pkt);
		if (ret < 0)
			break;

		unsigned si = (unsigned)pkt->stream_index;
		if (si >= ifmt->nb_streams) {
			av_packet_unref(pkt);
			continue;
		}
		AVStream *is = ifmt->streams[si];
		AVStream *os = ofmt->streams[si];
		av_packet_rescale_ts(pkt, is->time_base, os->time_base);
		pkt->pos = -1;

		if (av_interleaved_write_frame(ofmt, pkt) < 0)
			ok = false;
		av_packet_unref(pkt);
	}

	if (pkt)
		av_packet_free(&pkt);

	if (ok)
		av_write_trailer(ofmt);

	if (!(ofmt->oformat->flags & AVFMT_NOFILE))
		avio_closep(&ofmt->pb);
	avformat_close_input(&ifmt);
	avformat_free_context(ofmt);

	if (!ok)
		DeleteFileA(dst_path.c_str());
	return ok;
}

// ---------------------------------------------------------------------------
// av_trim_to_audio — lossless trim to fix AAC tail drop
// ---------------------------------------------------------------------------

bool av_trim_to_audio(const std::string &mp4_path)
{
	suppress_av_log();

	double video_dur = 0.0, audio_dur = 0.0;
	if (!av_get_durations(mp4_path, video_dur, audio_dur))
		return false;

	double gap = video_dur - audio_dur;
	if (gap < 0.001 || gap > 0.250)
		return false; // nothing to fix or suspiciously large gap

	// Remux to temp, stopping packets past audio_dur
	std::string tmp = mp4_path + ".avtrim.mp4";

	AVFormatContext *ifmt = nullptr;
	if (avformat_open_input(&ifmt, mp4_path.c_str(), nullptr, nullptr) < 0)
		return false;
	if (avformat_find_stream_info(ifmt, nullptr) < 0) {
		avformat_close_input(&ifmt);
		return false;
	}

	AVFormatContext *ofmt = nullptr;
	if (avformat_alloc_output_context2(&ofmt, nullptr, "mp4",
	                                   tmp.c_str()) < 0) {
		avformat_close_input(&ifmt);
		return false;
	}

	for (unsigned i = 0; i < ifmt->nb_streams; i++) {
		AVStream *is = ifmt->streams[i];
		AVStream *os = avformat_new_stream(ofmt, nullptr);
		if (!os) {
			avformat_close_input(&ifmt);
			avformat_free_context(ofmt);
			return false;
		}
		avcodec_parameters_copy(os->codecpar, is->codecpar);
		os->codecpar->codec_tag = 0;
		os->time_base            = is->time_base;
	}

	// Copy chapters
	if (ifmt->nb_chapters > 0) {
		ofmt->chapters = (AVChapter **)av_malloc(
			sizeof(AVChapter *) * ifmt->nb_chapters);
		if (ofmt->chapters) {
			ofmt->nb_chapters = ifmt->nb_chapters;
			for (unsigned i = 0; i < ifmt->nb_chapters; i++) {
				ofmt->chapters[i] = (AVChapter *)av_mallocz(
					sizeof(AVChapter));
				if (ofmt->chapters[i]) {
					*ofmt->chapters[i] = *ifmt->chapters[i];
					ofmt->chapters[i]->metadata = nullptr;
					av_dict_copy(
						&ofmt->chapters[i]->metadata,
						ifmt->chapters[i]->metadata, 0);
				}
			}
		}
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&ofmt->pb, tmp.c_str(), AVIO_FLAG_WRITE) < 0) {
			avformat_close_input(&ifmt);
			avformat_free_context(ofmt);
			return false;
		}
	}

	if (avformat_write_header(ofmt, nullptr) < 0) {
		if (!(ofmt->oformat->flags & AVFMT_NOFILE))
			avio_closep(&ofmt->pb);
		avformat_close_input(&ifmt);
		avformat_free_context(ofmt);
		DeleteFileA(tmp.c_str());
		return false;
	}

	AVPacket *pkt = av_packet_alloc();
	bool      ok  = (pkt != nullptr);

	// Allow one extra video frame past audio_dur to avoid cutting mid-frame.
	// At 60fps one frame = ~16.7ms; use 50ms as a safe ceiling.
	const double cutoff = audio_dur + 0.050;

	while (ok) {
		int ret = av_read_frame(ifmt, pkt);
		if (ret < 0)
			break;

		unsigned si = (unsigned)pkt->stream_index;
		if (si < ifmt->nb_streams) {
			AVStream *is     = ifmt->streams[si];
			double    pts_s  = (pkt->pts != AV_NOPTS_VALUE)
			                        ? pkt->pts * av_q2d(is->time_base)
			                        : pkt->dts * av_q2d(is->time_base);

			if (pts_s <= cutoff) {
				AVStream *os = ofmt->streams[si];
				av_packet_rescale_ts(pkt, is->time_base,
				                     os->time_base);
				pkt->pos = -1;
				av_interleaved_write_frame(ofmt, pkt);
			}
		}
		av_packet_unref(pkt);
	}

	if (pkt)
		av_packet_free(&pkt);

	if (ok)
		av_write_trailer(ofmt);

	if (!(ofmt->oformat->flags & AVFMT_NOFILE))
		avio_closep(&ofmt->pb);
	avformat_close_input(&ifmt);
	avformat_free_context(ofmt);

	if (!ok) {
		DeleteFileA(tmp.c_str());
		return false;
	}

	// Swap temp → original
	DeleteFileA(mp4_path.c_str());
	MoveFileA(tmp.c_str(), mp4_path.c_str());
	return true;
}
