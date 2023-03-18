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
  printf("ok\n");
  result.insert(result.end(), buffer, buffer + buffer_size);
  return buffer_size;
}

extern "C"
{
  EMSCRIPTEN_KEEPALIVE
  int transcode(uint8_t *array, const int size, const char *src_format_short_name, const char *dst_format_short_name, const char *dst_codec_name)
  {
    // Create input AVIOContext
    auto avio_ctx = avio_alloc_context(array, size, 0, nullptr, nullptr, nullptr, nullptr);

    // Initialize input format context
    AVFormatContext *input_ctx = avformat_alloc_context();
    input_ctx->pb = avio_ctx;

    const AVInputFormat *input_format = throw_if_null(av_find_input_format(src_format_short_name), "Could not find input format");
    throw_if_neg(avformat_open_input(&input_ctx, nullptr, input_format, nullptr), "Could not open input format context");
    throw_if_neg(avformat_find_stream_info(input_ctx, nullptr), "Could not find stream information");

    // Initialize output format context
    AVFormatContext *output_ctx = nullptr;
    const AVOutputFormat *output_format = throw_if_null(av_guess_format(dst_format_short_name, nullptr, nullptr), "Output format not recognized");
    throw_if_neg(avformat_alloc_output_context2(&output_ctx, output_format, nullptr, nullptr), "Failed to allocate output context");

    // Copy streams and setup codecs
    AVStream *in_stream = input_ctx->streams[0];
    const AVCodec *dec_codec = throw_if_null(avcodec_find_decoder(in_stream->codecpar->codec_id), "Source codec not found/supported");
    AVCodecContext *dec_ctx = throw_if_null(avcodec_alloc_context3(dec_codec), "Failed to allocate source codec context");
    throw_if_neg(avcodec_parameters_to_context(dec_ctx, in_stream->codecpar), "Failed to copy source codec parameters to source codec context");
    throw_if_neg(avcodec_open2(dec_ctx, dec_codec, nullptr), "Failed to open source codec");

    printf("%s\n", dst_codec_name);
    AVStream *out_stream = throw_if_null(avformat_new_stream(output_ctx, nullptr), "Failed to create a new stream");
    const AVCodec *enc_codec = throw_if_null(avcodec_find_encoder_by_name(dst_codec_name), "Destination codec not found");
    AVCodecContext *enc_ctx = throw_if_null(avcodec_alloc_context3(enc_codec), "Failed to allocate destination codec context");
    if (dec_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
      av_channel_layout_default(&dec_ctx->ch_layout, dec_ctx->ch_layout.nb_channels);
    throw_if_neg(av_channel_layout_copy(&enc_ctx->ch_layout, &dec_ctx->ch_layout), "Could not select channel layout");
    enc_ctx->sample_rate = dec_ctx->sample_rate;
    enc_ctx->sample_fmt = enc_codec->sample_fmts[0];
    enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
    enc_ctx->bit_rate = dec_ctx->bit_rate;
    enc_ctx->strict_std_compliance = -2;
    throw_if_neg(avcodec_open2(enc_ctx, enc_codec, nullptr), "Failed to open destination codec");
    throw_if_neg(avcodec_parameters_from_context(out_stream->codecpar, enc_ctx), "Failed to copy destination codec parameters");

    // Create a custom IO context for writing the output
    std::vector<uint8_t> output_buffer(1);
    AVIOContext *output_avio_ctx = avio_alloc_context(output_buffer.data(), output_buffer.size(), 1, nullptr, nullptr, &custom_io_write, nullptr);
    output_ctx->pb = output_avio_ctx;
    output_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    // Write header
    throw_if_neg(avformat_write_header(output_ctx, nullptr), "Failed to write header");

    // Prepare resampler
    SwrContext *swr_ctx = nullptr;
    swr_alloc_set_opts2(&swr_ctx,
                        &enc_ctx->ch_layout, enc_ctx->sample_fmt, enc_ctx->sample_rate,
                        &dec_ctx->ch_layout, dec_ctx->sample_fmt, dec_ctx->sample_rate, 0, nullptr);
    throw_if_null(swr_ctx, "Failed to allocate resampler context");
    throw_if_neg(swr_init(swr_ctx), "Failed to initialize resampler context");

    // Transcode audio
    AVFrame *frame = av_frame_alloc();

    while (true)
    {
      AVPacket *pkt = av_packet_alloc();
      // reads an audio packet from a demuxer (muxer1/input_ctx) and stores it in an AVPacket pkt object.
      int ret = av_read_frame(input_ctx, pkt);
      if (ret == AVERROR_EOF)
        break;
      throw_if_neg(ret, "Failed to read frame");

      // checks whether the current packet belongs to the same stream as the one being processed
      if (pkt->stream_index != in_stream->index)
      {
        av_packet_unref(pkt);
        continue;
      }

      // Sends an encoded packet from the demuxer (muxer1) to the decoder for decoding.
      // The decoder uses this function to pass the compressed data to the decoding pipeline.
      // dec_ctx <-- pkt (from input)
      throw_if_neg(avcodec_send_packet(dec_ctx, pkt), "Failed to send packet to decoder");

      while (true)
      {
        // receives a decoded frame from the decoder, contains uncompressed audio
        // frame <-- dec_ctx
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
          break;
        throw_if_neg(ret, "Failed to receive frame from decoder");
        AVFrame *out_frame = av_frame_clone(frame);
        throw_if_neg(swr_convert_frame(swr_ctx, out_frame, frame), "Failed to resample frame");
        out_frame->nb_samples = enc_ctx->frame_size;
        out_frame->format = enc_ctx->sample_fmt;
        // sends an uncompressed frame from the encoder to the muxer (muxer2) for encoding
        // enc_ctx <-- out_frame
        throw_if_neg(avcodec_send_frame(enc_ctx, out_frame), "Failed to send frame to encoder");

        while (true)
        {
          // receives an encoded packet from the encoder
          // pkt <-- enc_ctx
          ret = avcodec_receive_packet(enc_ctx, pkt);
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
          throw_if_neg(ret, "Failed to receive packet from encoder");
          printf("okmdr\n");

          pkt->stream_index = out_stream->index;
          throw_if_neg(av_write_frame(output_ctx, pkt), "Failed to write packet");
          av_packet_unref(pkt);
        }
        av_frame_unref(frame);
        av_frame_unref(out_frame);
      }
      av_packet_unref(pkt);
      av_packet_free(&pkt); // to externalize
    }

    // Flush encoder
    AVPacket *pkt = av_packet_alloc();
    throw_if_neg(avcodec_send_frame(enc_ctx, nullptr), "Failed to send empty frame to encoder");
    while (avcodec_receive_packet(enc_ctx, pkt) == 0)
    {
      throw_if_neg(av_interleaved_write_frame(output_ctx, pkt), "Failed to write packet");
      av_packet_unref(pkt);
    }

    // Write trailer and cleanup
    throw_if_neg(av_write_trailer(output_ctx), "Failed to write trailer");
    swr_free(&swr_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    avcodec_close(dec_ctx);
    avcodec_free_context(&dec_ctx);
    avcodec_close(enc_ctx);
    avcodec_free_context(&enc_ctx);

    avformat_close_input(&input_ctx);
    avformat_free_context(input_ctx);

    avio_flush(output_avio_ctx);
    av_free(output_avio_ctx);

    // Copy the transcoded audio data back to the input array
    int output_size = output_buffer.size();
    std::memcpy(array, output_buffer.data(), output_size);

    // Cleanup
    avformat_close_input(&output_ctx);
    avformat_free_context(output_ctx);

    return output_size;
  }
}