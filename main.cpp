#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <emscripten.h>
#include <emscripten/bind.h>

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

struct WriteBufferData
{
  uint8_t *ptr;
  size_t size;
  size_t offset;
};
struct ReadBufferData
{
  const uint8_t *ptr;
  const size_t size;
  size_t offset;
};

int custom_io_write(void *opaque, uint8_t *buffer, int32_t buffer_size)
{
  struct WriteBufferData *bd = (struct WriteBufferData *)opaque;
  size_t new_size = bd->offset + buffer_size;
  if (new_size > bd->size)
  {
    bd->ptr = (uint8_t *)av_realloc(bd->ptr, new_size);
    if (!bd->ptr)
      return AVERROR(ENOMEM);
    bd->size = new_size;
  }
  memcpy(bd->ptr + bd->offset, buffer, buffer_size);
  bd->offset += buffer_size;
  return buffer_size;
}

static int custom_io_read(void *opaque, uint8_t *buf, int buf_size)
{
  struct ReadBufferData *bd = (struct ReadBufferData *)opaque;
  buf_size = FFMIN(buf_size, bd->size - bd->offset);
  if (!buf_size)
    return AVERROR_EOF;
  memcpy(buf, bd->ptr + bd->offset, buf_size);
  bd->offset += buf_size;
  return buf_size;
}

static int64_t custom_seek(void *opaque, int64_t offset, int whence)
{
  struct ReadBufferData *bd = (struct ReadBufferData *)opaque;
  if (whence == AVSEEK_SIZE)
    return bd->size;

  int64_t new_position = -1;
  switch (whence)
  {
  case SEEK_SET:
    new_position = offset;
    break;
  case SEEK_CUR:
    new_position = bd->offset + offset;
    break;
  case SEEK_END:
    new_position = bd->size - offset;
    break;
  default:
    return AVERROR(EINVAL);
  }

  if (new_position < 0 || new_position > bd->size)
    return AVERROR(EINVAL);

  bd->offset = new_position;
  return new_position;
}

int find_nearest(const int *raw_arr, const int target)
{
  int nearest = *std::min_element(raw_arr, std::find(raw_arr, raw_arr + 100, 0),
                                  [&](int a, int b)
                                  { return std::abs(a - target) < std::abs(b - target); });
  return nearest;
}

struct Result
{
  uint8_t *out_ptr;
  uint32_t size;
  uint16_t sample_rate;
  uint16_t bit_depth;
  uint16_t num_channels;
  uint16_t duration;
  uint16_t frame_size;
};

extern "C"
{
  EMSCRIPTEN_KEEPALIVE
  Result *probe(const uint8_t *array, const size_t size, const char *src_format_short_name, const int verbose)
  {
    struct ReadBufferData in_bd = {array, size, 0};
    size_t in_ctx_buf_size = 1;
    uint8_t *in_ctx_buf = (uint8_t *)malloc(in_ctx_buf_size);
    auto avio_ctx = avio_alloc_context(in_ctx_buf, in_ctx_buf_size, 0, &in_bd, &custom_io_read, nullptr, &custom_seek);
    AVFormatContext *input_ctx = avformat_alloc_context();
    input_ctx->pb = avio_ctx;
    const AVInputFormat *input_format = throw_if_null(av_find_input_format(src_format_short_name), "Could not find input format");
    throw_if_neg(avformat_open_input(&input_ctx, nullptr, input_format, nullptr), "Could not open input format context");
    throw_if_neg(avformat_find_stream_info(input_ctx, nullptr), "Could not find stream information");
    AVCodecParameters *codecParams = input_ctx->streams[0]->codecpar;
    struct Result *r = static_cast<Result *>(malloc(sizeof(struct Result)));
    r->size = size;
    r->num_channels = codecParams->ch_layout.nb_channels;
    r->bit_depth = codecParams->bits_per_raw_sample > 0 ? codecParams->bits_per_raw_sample : 16;
    r->frame_size = codecParams->frame_size;
    r->sample_rate = codecParams->sample_rate;
    r->duration = (int)(input_ctx->duration * av_q2d(AV_TIME_BASE_Q) * 1000);
    return r;
  };
  EMSCRIPTEN_KEEPALIVE
  Result *transcode(const uint8_t *array, const size_t size, const char *src_format_short_name,
                    const char *dst_format_short_name, const char *dst_codec_name,
                    const char *dst_container_options, const int verbose)
  {
    if (verbose > 0)
      printf("dst_format_short_name: %s %s dst_codec_name: %s\n", dst_format_short_name,
             dst_container_options, dst_codec_name);

    // Create input AVIOContext
    struct ReadBufferData in_bd = {array, size, 0};
    size_t in_ctx_buf_size = 1;
    uint8_t *in_ctx_buf = (uint8_t *)malloc(in_ctx_buf_size);
    auto avio_ctx = avio_alloc_context(in_ctx_buf, in_ctx_buf_size, 0, &in_bd, &custom_io_read, nullptr, &custom_seek);

    // Initialize input format context
    AVFormatContext *input_ctx = avformat_alloc_context();
    input_ctx->pb = avio_ctx;

    const AVInputFormat *input_format = throw_if_null(av_find_input_format(src_format_short_name), "Could not find input format");
    throw_if_neg(avformat_open_input(&input_ctx, nullptr, input_format, nullptr), "Could not open input format context");
    throw_if_neg(avformat_find_stream_info(input_ctx, nullptr), "Could not find stream information");
    throw_if_neg(input_ctx->pb->error, "Error in PB IO");

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

    AVStream *out_stream = throw_if_null(avformat_new_stream(output_ctx, nullptr), "Failed to create a new stream");
    const AVCodec *enc_codec = throw_if_null(avcodec_find_encoder_by_name(dst_codec_name), "Destination codec not found");
    AVCodecContext *enc_ctx = throw_if_null(avcodec_alloc_context3(enc_codec), "Failed to allocate destination codec context");
    if (dec_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
      av_channel_layout_default(&dec_ctx->ch_layout, dec_ctx->ch_layout.nb_channels);
    throw_if_neg(av_channel_layout_copy(&enc_ctx->ch_layout, &dec_ctx->ch_layout), "Could not select channel layout");
    enc_ctx->sample_rate = find_nearest(enc_codec->supported_samplerates, dec_ctx->sample_rate);
    enc_ctx->sample_fmt = enc_codec->sample_fmts[0];
    enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
    enc_ctx->strict_std_compliance = -2;
    AVDictionary *codec_opts = NULL;
    if (dst_container_options != NULL)
      for (char *line = strtok(strdup(dst_container_options), ","); line != NULL;
           line = strtok(line + strlen(line) + 1, ","))
      {
        char *key = strtok(strdup(line), ":");
        char *value = strtok(key + strlen(key) + 1, ":");
        if (verbose > 0)
          printf("key: %s\tvalue: %s\n", key, value);
        av_dict_set(&codec_opts, key, value, 0);
      }
    throw_if_neg(avcodec_open2(enc_ctx, enc_codec, nullptr), "Failed to open destination codec");
    throw_if_neg(avcodec_parameters_from_context(out_stream->codecpar, enc_ctx), "Failed to copy destination codec parameters");

    // Create a custom IO context for writing the output
    size_t out_ctx_buf_size = 1;
    uint8_t *out_ctx_buf = (uint8_t *)malloc(out_ctx_buf_size);
    struct WriteBufferData out_bd = {nullptr, 0, 0};
    AVIOContext *output_avio_ctx = avio_alloc_context(out_ctx_buf, out_ctx_buf_size, 1, &out_bd, nullptr, &custom_io_write, &custom_seek);
    output_ctx->pb = output_avio_ctx;

    // Write header
    throw_if_neg(avformat_write_header(output_ctx, &codec_opts), "Failed to write header");
    output_ctx->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_AUTO_BSF;

    // Prepare resampler
    SwrContext *swr_ctx = nullptr;
    swr_alloc_set_opts2(&swr_ctx,
                        &enc_ctx->ch_layout, enc_ctx->sample_fmt, enc_ctx->sample_rate,
                        &dec_ctx->ch_layout, dec_ctx->sample_fmt, dec_ctx->sample_rate, 0, nullptr);
    swr_set_compensation(swr_ctx, (enc_ctx->frame_size - dec_ctx->frame_size) * enc_ctx->sample_rate / dec_ctx->sample_rate, enc_ctx->frame_size);
    throw_if_null(swr_ctx, "Failed to allocate resampler context");
    throw_if_neg(swr_init(swr_ctx), "Failed to initialize resampler context");

    // Transcode audio
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = throw_if_null(av_packet_alloc(), "av_packet_alloc failed");

    while (true)
    {
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
        if (ret < 0) // (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) // not working on the end of file
          break;
        throw_if_neg(ret, "Failed to receive frame from decoder");
        AVFrame *out_frame = av_frame_alloc();
        out_frame->nb_samples = enc_ctx->frame_size;
        out_frame->format = enc_ctx->sample_fmt;
        out_frame->pts = av_rescale_q(frame->pts, in_stream->time_base, out_stream->time_base);
        throw_if_neg(av_channel_layout_copy(&out_frame->ch_layout, &enc_ctx->ch_layout), "Could not select channel layout");
        throw_if_neg(av_frame_get_buffer(out_frame, 0), "Failed to allocate output frame buffer");
        if (verbose > 1)
          printf("enc_ctx->frame_size: %d, frame->nb_samples: %d\n", enc_ctx->frame_size, frame->nb_samples);
        throw_if_neg(swr_convert(swr_ctx, out_frame->data, enc_ctx->frame_size, (const uint8_t **)frame->data, frame->nb_samples), "Failed to resample frame");

        // sends an uncompressed frame from the encoder to the muxer (muxer2) for encoding
        // enc_ctx <-- out_frame
        pkt->data = nullptr;
        pkt->size = 0;
        throw_if_neg(avcodec_send_frame(enc_ctx, out_frame), "Failed to send frame to encoder");
        while (true)
        {
          const int ret = avcodec_receive_packet(enc_ctx, pkt);
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
          throw_if_neg(ret, "Failed to receive packet.");
          av_packet_rescale_ts(pkt, enc_ctx->time_base, out_stream->time_base);
          pkt->stream_index = out_stream->index;
          throw_if_neg(av_interleaved_write_frame(output_ctx, pkt), "Failed to write frame");
          av_packet_unref(pkt);
        }
        av_frame_unref(frame);
        av_frame_unref(out_frame);
      }
      av_packet_unref(pkt);
    }

    if (verbose > 0)
      av_dump_format(input_ctx, 0, "<input>", 0);
    if (verbose > 0)
      av_dump_format(output_ctx, 0, "<output>", 1);

    // Flush encoder
    throw_if_neg(avcodec_send_frame(enc_ctx, nullptr), "Failed to send empty frame to encoder");
    while (avcodec_receive_packet(enc_ctx, pkt) == 0)
    {
      throw_if_neg(av_write_frame(output_ctx, pkt), "Failed to write packet");
      av_packet_unref(pkt);
    }

    // Write trailer and cleanup
    throw_if_neg(av_write_trailer(output_ctx), "Failed to write trailer");

    struct Result *r = static_cast<Result *>(malloc(sizeof(struct Result)));
    r->out_ptr = out_bd.ptr;
    r->size = out_bd.size;
    r->sample_rate = enc_ctx->sample_rate;
    r->duration = (int)(input_ctx->duration * av_q2d(AV_TIME_BASE_Q) * 1000);

    // Cleanup
    swr_free(&swr_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    avcodec_close(dec_ctx);
    avcodec_free_context(&dec_ctx);
    avcodec_close(enc_ctx);
    avcodec_free_context(&enc_ctx);

    avformat_close_input(&input_ctx);
    avformat_free_context(input_ctx);

    avformat_close_input(&output_ctx);
    avformat_free_context(output_ctx);
    return r;
  }
}