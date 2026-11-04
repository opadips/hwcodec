// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/muxing.c

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
}
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "MUX"
#include <log.h>

namespace {
typedef struct OutputStream {
  AVStream *st;
  AVPacket *tmp_pkt;
} OutputStream;

class Muxer {
public:
  OutputStream video_st;
  AVFormatContext *oc = NULL;
  int framerate;
  int64_t start_ms;
  int64_t last_pts;
  int got_first;
  int header_written = 0;

  Muxer() {}

  void destroy() {
    OutputStream *ost = &video_st;
    if (ost && ost->tmp_pkt)
      av_packet_free(&ost->tmp_pkt);
    if (oc && oc->pb && !(oc->oformat->flags & AVFMT_NOFILE))
      avio_closep(&oc->pb);
    if (oc)
      avformat_free_context(oc);
  }

  // codec is a DataFormat: 0 H.264, 1 H.265, 4 AV1.
  bool init(const char *filename, int width, int height, int codec,
            int framerate) {
    OutputStream *ost = &video_st;
    ost->st = NULL;
    ost->tmp_pkt = NULL;
    int ret;

    if ((ret = avformat_alloc_output_context2(&oc, NULL, NULL, filename)) < 0) {
          LOG_ERROR(std::string("avformat_alloc_output_context2 failed, ret = ") +
              std::to_string(ret));
      return false;
    }

    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
      LOG_ERROR(std::string("avformat_new_stream failed"));
      return false;
    }
    ost->st->id = oc->nb_streams - 1;
    switch (codec) {
    case 0:
      ost->st->codecpar->codec_id = AV_CODEC_ID_H264;
      break;
    case 1:
      ost->st->codecpar->codec_id = AV_CODEC_ID_H265;
      break;
    case 4:
      ost->st->codecpar->codec_id = AV_CODEC_ID_AV1;
      break;
    default:
      LOG_ERROR(std::string("unsupported codec ") + std::to_string(codec));
      return false;
    }
    ost->st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    ost->st->codecpar->width = width;
    ost->st->codecpar->height = height;

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
      ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
      if (ret < 0) {
        LOG_ERROR(std::string("avio_open failed, ret = ") + std::to_string(ret));
        return false;
      }
    }

    ost->tmp_pkt = av_packet_alloc();
    if (!ost->tmp_pkt) {
      LOG_ERROR(std::string("av_packet_alloc failed"));
      return false;
    }

    if (strcmp(oc->oformat->name, "mp4") == 0) {
      // Keep incomplete recordings recoverable (2 s fragments, flushed as
      // they are cut) and finalize normal recordings as ordinary MP4 for
      // player compatibility.
      if ((ret = av_opt_set(oc->priv_data, "movflags",
                            "frag_keyframe+hybrid_fragmented", 0)) < 0 ||
          (ret = av_opt_set_int(oc->priv_data, "frag_duration", 2000000, 0)) < 0) {
        LOG_WARN(std::string("Failed to configure MP4 fragments, ret = ") +
                 std::to_string(ret));
        av_opt_set_defaults(oc->priv_data);
      } else {
        oc->flags |= AVFMT_FLAG_FLUSH_PACKETS;
      }
    }

    // The header is written with the first key frame (write_video_frame):
    // AV1's av1C box is built from codec extradata, which only that
    // frame's sequence header can supply.
    this->framerate = framerate;
    this->start_ms = 0;
    this->last_pts = 0;
    this->got_first = 0;

    return true;
  }

  int write_video_frame(const uint8_t *data, int len, int64_t pts_ms, int key) {
    OutputStream *ost = &video_st;
    AVPacket *pkt = ost->tmp_pkt;
    AVFormatContext *fmt_ctx = oc;
    int ret;

    if (framerate <= 0)
      return -3;
    if (!got_first) {
      if (key != 1)
        return -2;
      start_ms = pts_ms;
    }
    if (!header_written) {
      AVCodecParameters *par = ost->st->codecpar;
      if (par->codec_id == AV_CODEC_ID_AV1 && !par->extradata) {
        // ff_isom_write_av1c parses the OBUs for the sequence header.
        par->extradata = (uint8_t *)av_mallocz(len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!par->extradata)
          return -1;
        memcpy(par->extradata, data, len);
        par->extradata_size = len;
      }
      if ((ret = avformat_write_header(fmt_ctx, NULL)) < 0) {
        LOG_ERROR(std::string("avformat_write_header failed, ret = ") + std::to_string(ret));
        return -1;
      }
      header_written = 1;
    }
    int64_t pts = (pts_ms - start_ms); // use write timestamp
    if (pts <= last_pts && got_first) {
      pts = last_pts + 1000 / framerate;
    }
    got_first = 1;

    pkt->data = (uint8_t *)data;
    pkt->size = len;
    pkt->pts = pts;
    pkt->dts = pkt->pts; // no B-frame
    int64_t duration = pkt->pts - last_pts;
    last_pts = pkt->pts;
    pkt->duration = duration > 0 ? duration : 1000 / framerate; // predict
    AVRational rational;
    rational.num = 1;
    rational.den = 1000;
    av_packet_rescale_ts(pkt, rational,
                         ost->st->time_base); // ms -> stream timebase
    pkt->stream_index = ost->st->index;
    if (key == 1) {
      pkt->flags |= AV_PKT_FLAG_KEY;
    } else {
      pkt->flags &= ~AV_PKT_FLAG_KEY;
    }
    ret = av_write_frame(fmt_ctx, pkt);
    if (ret < 0) {
      LOG_ERROR(std::string("av_write_frame failed, ret = ") + std::to_string(ret));
      return -1;
    }
    return 0;
  }
};
} // namespace

extern "C" Muxer *hwcodec_new_muxer(const char *filename, int width, int height,
                                    int codec, int framerate) {
  Muxer *muxer = NULL;
  try {
    muxer = new Muxer();
    if (muxer) {
      if (muxer->init(filename, width, height, codec, framerate)) {
        return muxer;
      }
    }
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("new muxer exception: ") + std::string(e.what()));
  } catch (...) {
    LOG_ERROR("hwcodec_new_muxer: unknown exception");
  }
  if (muxer) {
    muxer->destroy();
    delete muxer;
    muxer = NULL;
  }
  return NULL;
}

extern "C" int hwcodec_write_video_frame(Muxer *muxer, const uint8_t *data,
                                         int len, int64_t pts_ms, int key) {
  try {
    return muxer->write_video_frame(data, len, pts_ms, key);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("write_video_frame exception: ") + std::string(e.what()));
  } catch (...) {
    LOG_ERROR("hwcodec_write_video_frame: unknown exception");
  }
  return -1;
}

extern "C" int hwcodec_write_tail(Muxer *muxer) {
  try {
    if (!muxer || !muxer->oc)
      return -1;
    // Nothing was ever written: no header, so no trailer to write.
    if (!muxer->header_written)
      return 0;
    return av_write_trailer(muxer->oc);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("write_tail exception: ") + std::string(e.what()));
  } catch (...) {
    LOG_ERROR("hwcodec_write_tail: unknown exception");
  }
  return -1;
}

extern "C" void hwcodec_free_muxer(Muxer *muxer) {
  try {
    if (!muxer)
      return;
    muxer->destroy();
    delete muxer;
    muxer = NULL;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("free_muxer exception: ") + std::string(e.what()));
  } catch (...) {
    LOG_ERROR("hwcodec_free_muxer: unknown exception");
  }
}