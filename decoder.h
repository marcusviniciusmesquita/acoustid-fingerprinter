/*
 * Chromaprint -- Audio fingerprinting toolkit
 * Copyright (C) 2010  Lukas Lalinsky <lalinsky@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#ifndef FPSUBMIT_DECODER_H_
#define FPSUBMIT_DECODER_H_

#include <QMutex>
#include <string>
#include <algorithm>
#include <stdint.h>
extern "C" {
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libavutil/frame.h>
	#include <libavutil/opt.h>
	#include <libswresample/swresample.h>
}
#include "fingerprintcalculator.h"

class Decoder
{
public:
	Decoder(const std::string &fileName);
	~Decoder();

	bool Open();
	void Decode(FingerprintCalculator *consumer, int maxLength = 0);

	int Channels()
	{
		return m_codec_ctx->ch_layout.nb_channels;
	}

	int SampleRate()
	{
		return m_codec_ctx->sample_rate;
	}

	std::string LastError()
	{
		return m_error;
	}

	static void initialize();

private:
	uint8_t *m_buffer2;
	std::string m_file_name;
	std::string m_error;
	AVFormatContext *m_format_ctx;
	AVCodecContext *m_codec_ctx;
	bool m_codec_open;
	AVStream *m_stream;
	SwrContext *m_swr_ctx;
	static QMutex m_mutex;
	AVFrame *m_frame;
};

inline void Decoder::initialize()
{
	av_log_set_level(AV_LOG_ERROR);
}

inline Decoder::Decoder(const std::string &file_name)
: m_file_name(file_name),
m_format_ctx(nullptr),
m_codec_ctx(nullptr),
m_stream(nullptr),
m_codec_open(false),
m_swr_ctx(nullptr),
m_buffer2(nullptr)
{
	m_buffer2 = (uint8_t *)av_malloc(192000 * 2 + 16);
	m_frame = av_frame_alloc();
}

inline Decoder::~Decoder()
{
	if (m_codec_ctx) {
		QMutexLocker locker(&m_mutex);
		avcodec_free_context(&m_codec_ctx);
	}
	if (m_format_ctx) {
		avformat_close_input(&m_format_ctx);
	}
	if (m_swr_ctx) {
		swr_free(&m_swr_ctx);
	}
	if (m_buffer2) {
		av_free(m_buffer2);
	}
	av_frame_free(&m_frame);
}

inline bool Decoder::Open()
{
	QMutexLocker locker(&m_mutex);

	if (avformat_open_input(&m_format_ctx, m_file_name.c_str(), NULL, NULL) != 0) {
		m_error = "Couldn't open the file: " + m_file_name;
		return false;
	}

	if (avformat_find_stream_info(m_format_ctx, NULL) < 0) {
		m_error = "Couldn't find stream information in the file.";
		return false;
	}

	for (int i = 0; i < (int)m_format_ctx->nb_streams; i++) {
		AVCodecParameters *par = m_format_ctx->streams[i]->codecpar;
		if (par && par->codec_type == AVMEDIA_TYPE_AUDIO) {
			m_stream = m_format_ctx->streams[i];
			break;
		}
	}
	if (!m_stream) {
		m_error = "Couldn't find any audio stream in the file.";
		return false;
	}

	const AVCodec *codec = avcodec_find_decoder(m_stream->codecpar->codec_id);
	if (!codec) {
		m_error = "Unknown codec.";
		return false;
	}

	m_codec_ctx = avcodec_alloc_context3(codec);
	if (!m_codec_ctx) {
		m_error = "Couldn't allocate codec context.";
		return false;
	}

	if (avcodec_parameters_to_context(m_codec_ctx, m_stream->codecpar) < 0) {
		m_error = "Couldn't copy codec parameters to context.";
		return false;
	}

	if (avcodec_open2(m_codec_ctx, codec, NULL) < 0) {
		m_error = "Couldn't open the codec.";
		return false;
	}
	m_codec_open = true;

	if (m_codec_ctx->sample_fmt != AV_SAMPLE_FMT_S16) {
		m_swr_ctx = swr_alloc();
		if (!m_swr_ctx) {
			m_error = "Couldn't allocate resampler context.";
			return false;
		}
		av_opt_set_chlayout  (m_swr_ctx, "in_chlayout",   &m_codec_ctx->ch_layout, 0);
		av_opt_set_chlayout  (m_swr_ctx, "out_chlayout",  &m_codec_ctx->ch_layout, 0);
		av_opt_set_int       (m_swr_ctx, "in_sample_rate",  m_codec_ctx->sample_rate, 0);
		av_opt_set_int       (m_swr_ctx, "out_sample_rate", m_codec_ctx->sample_rate, 0);
		av_opt_set_sample_fmt(m_swr_ctx, "in_sample_fmt",  m_codec_ctx->sample_fmt, 0);
		av_opt_set_sample_fmt(m_swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16,       0);
		if (swr_init(m_swr_ctx) < 0) {
			m_error = "Couldn't initialize sample format converter.";
			return false;
		}
	}

	if (Channels() <= 0) {
		m_error = "Invalid audio stream (no channels).";
		return false;
	}

	if (SampleRate() <= 0) {
		m_error = "Invalid sample rate.";
		return false;
	}

	return true;
}

inline void Decoder::Decode(FingerprintCalculator *consumer, int max_length)
{
	AVPacket *packet      = av_packet_alloc();
	AVPacket *packet_temp = av_packet_alloc();

	int remaining = max_length * SampleRate() * Channels();
	int stop = 0;

	while (!stop) {
		if (av_read_frame(m_format_ctx, packet) < 0) {
			break;
		}

		packet_temp->data = packet->data;
		packet_temp->size = packet->size;

		while (packet_temp->size > 0) {
			int got_output = 0;
			int consumed   = 0;

			int ret = avcodec_send_packet(m_codec_ctx, packet_temp);
			if (ret == 0) {
				ret = avcodec_receive_frame(m_codec_ctx, m_frame);
				if (ret == 0) {
					got_output = 1;
					consumed   = packet_temp->size;
				} else if (ret == AVERROR(EAGAIN)) {
					got_output = 0;
					consumed   = packet_temp->size;
				} else {
					got_output = 0;
					consumed   = 0;
				}
			} else {
				got_output = 0;
				consumed   = 0;
			}

			if (consumed < 0) {
				break;
			}

			packet_temp->data += consumed;
			packet_temp->size -= consumed;

			if (!got_output) {
				continue;
			}

			int16_t *audio_buffer;

			if (m_swr_ctx) {
				uint8_t *out_buf = m_buffer2;
				int out_samples  = m_frame->nb_samples;
				if (swr_convert(m_swr_ctx,
					&out_buf,                        out_samples,
					(const uint8_t **)m_frame->data, m_frame->nb_samples) < 0) {
					break;
					}
					audio_buffer = (int16_t *)m_buffer2;
			} else {
				audio_buffer = (int16_t *)m_frame->data[0];
			}

			int length = m_frame->nb_samples;
			if (max_length) {
				length = std::min(remaining, length);
			}

			consumer->feed(audio_buffer, length);

			if (max_length) {
				remaining -= length;
				if (remaining <= 0) {
					stop = 1;
					break;
				}
			}
		}

		av_packet_unref(packet);
	}

	av_packet_free(&packet);
	av_packet_free(&packet_temp);
}

#endif
