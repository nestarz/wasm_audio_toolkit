#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <functional>
#include <emscripten.h>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavcodec/codec.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
#include "libavutil/channel_layout.h"
}

template <typename T>
T throw_if_null(T result, const std::string &error_msg) { return result ? result : throw std::runtime_error(error_msg); }
template <typename T>
T throw_if_neg(T result, const std::string &error_msg) { return result < 0 ? throw std::runtime_error("(" + std::to_string(result) + ": " + std::string(av_err2str(result)) + ") " + error_msg) : result; }

std::vector<uint8_t> result;
int custom_io_write(void *opaque, uint8_t *buffer, int32_t buffer_size)
{
  result.insert(result.end(), buffer, buffer + buffer_size);
  return buffer_size;
}

extern "C"
{
  EMSCRIPTEN_KEEPALIVE
  int modify_array(uint8_t *array, int size, int profile_id)
  {
    //
    const int pcm_data_size = size;
    const uint8_t *pcm_data = array;
    //
    const int num_channels = 1;
    const AVSampleFormat input_format = AV_SAMPLE_FMT_S16;
    const int codec_id = profile_id == 0   ? AV_CODEC_ID_AAC
                         : profile_id == 1 ? AV_CODEC_ID_MP2
                         : profile_id == 2 ? AV_CODEC_ID_OPUS
                         : profile_id == 3 ? AV_CODEC_ID_MP3
                         : profile_id == 4 ? AV_CODEC_ID_OPUS
                                           : NULL;
    const char *format_short_name = profile_id == 0   ? "adts"
                                    : profile_id == 1 ? "mp2"
                                    : profile_id == 2 ? "webm"
                                    : profile_id == 3 ? "mp3"
                                    : profile_id == 4 ? "ogg"
                                                      : nullptr;
    printf("%s\n", format_short_name);
    const int sample_rate = 8000;
    const int out_sample_rate = codec_id == AV_CODEC_ID_OPUS ? 48000 : 16000;
    const int out_bit_rate = 32000;
    //
    const AVOutputFormat *out_fmt = throw_if_null(av_guess_format(format_short_name, NULL, NULL), "Output format not recognized");
    AVFormatContext *format_ctx = nullptr;
    throw_if_neg(avformat_alloc_output_context2(&format_ctx, out_fmt, "wav", nullptr), "Failed to allocate output context");
    const AVCodec *codec = throw_if_null(avcodec_find_encoder(static_cast<AVCodecID>(codec_id)), "Codec not found");
    AVStream *stream = throw_if_null(avformat_new_stream(format_ctx, codec), "Failed to create a new stream");
    AVCodecContext *codec_ctx = throw_if_null(avcodec_alloc_context3(codec), "Failed to allocate codec context");
    codec_ctx->strict_std_compliance = -2;
    AVChannelLayout src_ch_layout;
    (num_channels == 1) ? src_ch_layout = AV_CHANNEL_LAYOUT_MONO : src_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    throw_if_neg(av_channel_layout_copy(&codec_ctx->ch_layout, &src_ch_layout), "Could not select channel layout");
    codec_ctx->bit_rate = out_bit_rate;
    codec_ctx->sample_rate = out_sample_rate;
    codec_ctx->sample_fmt = codec->sample_fmts[0];
    codec_ctx->time_base = (AVRational){1, out_sample_rate};
    throw_if_neg(avcodec_open2(codec_ctx, codec, nullptr), "Failed to open codec");
    throw_if_neg(avcodec_parameters_from_context(stream->codecpar, codec_ctx), "Failed to copy codec parameters");

    std::vector<uint8_t> output_buffer(16 * 1024);
    AVIOContext *avio_ctx = avio_alloc_context(output_buffer.data(), output_buffer.size(), 1, nullptr, nullptr, &custom_io_write, nullptr);
    format_ctx->pb = avio_ctx;
    format_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    throw_if_neg(avformat_write_header(format_ctx, nullptr), "Failed to write header");

    AVFrame *frame = av_frame_alloc();
    throw_if_null(frame, "Failed to allocate frame");

    frame->nb_samples = codec_ctx->frame_size;
    frame->format = codec_ctx->sample_fmt;
    throw_if_neg(av_channel_layout_copy(&frame->ch_layout, &codec_ctx->ch_layout), "Could not copy channel layout");

    throw_if_neg(av_frame_get_buffer(frame, 0), "Failed to allocate frame data");

    SwrContext *swr_ctx = nullptr;
    swr_alloc_set_opts2(&swr_ctx,
                        &codec_ctx->ch_layout, codec_ctx->sample_fmt, codec_ctx->sample_rate,
                        &codec_ctx->ch_layout, input_format, sample_rate,
                        0, nullptr);
    throw_if_null(swr_ctx, "Failed to allocate resampler context");
    throw_if_neg(swr_init(swr_ctx), "Failed to initialize resampler context");

    int frame_size_bytes = (frame->nb_samples * num_channels * av_get_bytes_per_sample(input_format)) / 2;
    int pos = 0;
    int ret;
    while (pos + frame_size_bytes <= pcm_data_size)
    {
      const uint8_t *tmp_ptr = pcm_data + pos;
      throw_if_neg(swr_convert(swr_ctx, frame->data, frame->nb_samples,
                               &tmp_ptr, frame->nb_samples),
                   "Failed to convert samples");

      frame->pts = pos / (num_channels * av_get_bytes_per_sample(input_format));

      AVPacket *pkt = throw_if_null(av_packet_alloc(), "av_packet_alloc");
      pkt->data = nullptr;
      pkt->size = 0;
      throw_if_neg(avcodec_send_frame(codec_ctx, frame), "Failed to send frame");
      while (true)
      {
        const int ret = avcodec_receive_packet(codec_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
          break;
        throw_if_neg(ret, "Failed to receive packet.");
        av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
        pkt->stream_index = stream->index;
        throw_if_neg(av_write_frame(format_ctx, pkt), "Failed to write frame");
        av_packet_unref(pkt);
      }

      pos += frame_size_bytes;
    }

    throw_if_neg(av_write_trailer(format_ctx), "Failed to write trailer");

    swr_free(&swr_ctx);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    avformat_free_context(format_ctx);

    output_buffer.resize(avio_ctx->pos);
    av_free(avio_ctx);

    std::memcpy(array, result.data(), result.size());
    return result.size();
  }
}