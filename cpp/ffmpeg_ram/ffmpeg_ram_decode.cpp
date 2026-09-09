// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/hw_decode.c
// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/decode_video.c

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <memory>
#include <stdbool.h>

#define LOG_MODULE "FFMPEG_RAM_DEC"
#include <log.h>
#include <util.h>

#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif

#include "common.h"
#include "system.h"

// #define CFG_PKG_TRACE

namespace {

// See reset() for why the hwaccel surface pool is given headroom.
constexpr int EXTRA_HW_FRAMES = 3;

typedef void (*RamDecodeCallback)(const void *obj, int width, int height,
                                  enum AVPixelFormat pixfmt,
                                  int linesize[AV_NUM_DATA_POINTERS],
                                  uint8_t *data[AV_NUM_DATA_POINTERS], int key);

class FFmpegRamDecoder {
public:
  AVCodecContext *c_ = NULL;
  AVBufferRef *hw_device_ctx_ = NULL;
  AVFrame *sw_frame_ = NULL;
  AVFrame *frame_ = NULL;
  AVPacket *pkt_ = NULL;
  bool hwaccel_ = true;

  std::string name_;
  AVHWDeviceType device_type_ = AV_HWDEVICE_TYPE_NONE;
  int thread_count_ = 1;
  RamDecodeCallback callback_ = NULL;
  DataFormat data_format_;

#ifdef CFG_PKG_TRACE
  int in_ = 0;
  int out_ = 0;
#endif

  FFmpegRamDecoder(const char *name, int device_type, int thread_count,
                   RamDecodeCallback callback) {
    this->name_ = name;
    this->device_type_ = (AVHWDeviceType)device_type;
    this->thread_count_ = thread_count;
    this->callback_ = callback;
  }

  ~FFmpegRamDecoder() {}

  void free_decoder() {
    if (frame_)
      av_frame_free(&frame_);
    if (pkt_)
      av_packet_free(&pkt_);
    if (sw_frame_)
      av_frame_free(&sw_frame_);
    if (c_)
      avcodec_free_context(&c_);
    if (hw_device_ctx_)
      av_buffer_unref(&hw_device_ctx_);

    frame_ = NULL;
    pkt_ = NULL;
    sw_frame_ = NULL;
    c_ = NULL;
    hw_device_ctx_ = NULL;
  }
  int reset() {
    if (name_.find("h264") != std::string::npos) {
      data_format_ = DataFormat::H264;
    } else if (name_.find("hevc") != std::string::npos) {
      data_format_ = DataFormat::H265;
    } else {
      LOG_ERROR(std::string("unsupported data format:") + name_);
      return -1;
    }
    free_decoder();
    const AVCodec *codec = NULL;
    hwaccel_ = device_type_ != AV_HWDEVICE_TYPE_NONE;
    int ret;
    if (!(codec = avcodec_find_decoder_by_name(name_.c_str()))) {
      LOG_ERROR(std::string("avcodec_find_decoder_by_name ") + name_ + " failed");
      return -1;
    }
    if (!(c_ = avcodec_alloc_context3(codec))) {
      LOG_ERROR(std::string("Could not allocate video codec context"));
      return -1;
    }

    c_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    c_->thread_count =
        device_type_ != AV_HWDEVICE_TYPE_NONE ? 1 : thread_count_;
    c_->thread_type = FF_THREAD_SLICE;

    if (name_.find("qsv") != std::string::npos) {
      if ((ret = av_opt_set(c_->priv_data, "async_depth", "1", 0)) < 0) {
        LOG_ERROR(std::string("qsv set opt async_depth 1 failed"));
        return -1;
      }
      // https://github.com/FFmpeg/FFmpeg/blob/c6364b711bad1fe2fbd90e5b2798f87080ddf5ea/libavcodec/qsvdec.c#L932
      // for disable warning
      c_->pkt_timebase = av_make_q(1, 30);
    }

    if (hwaccel_) {
      ret =
          av_hwdevice_ctx_create(&hw_device_ctx_, device_type_, NULL, NULL, 0);
      if (ret < 0) {
        LOG_ERROR(std::string("av_hwdevice_ctx_create failed, ret = ") + av_err2str(ret));
        return -1;
      }
      c_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
      if (!check_support()) {
        LOG_ERROR(std::string("check_support failed"));
        return -1;
      }
      if (!(sw_frame_ = av_frame_alloc())) {
        LOG_ERROR(std::string("av_frame_alloc failed"));
        return -1;
      }

      // Headroom in the hwaccel's surface pool, on top of the DPB the
      // stream itself needs. FFmpeg sizes the D3D11 texture array from
      // the SPS's reference count alone, which is the theoretical
      // minimum; a driver that holds a surface a little longer than
      // FFmpeg assumes then has nothing free to decode the next picture
      // into. What that looks like from out here is a picture that
      // simply never arrives -- avcodec_send_packet accepts the packet,
      // avcodec_receive_frame answers EAGAIN, and nothing is logged at
      // any level, because from FFmpeg's point of view nothing went
      // wrong. Moonlight and Sunshine both carry the same headroom, for
      // the same reason.
      c_->extra_hw_frames = EXTRA_HW_FRAMES;

      // Hardware encoders routinely stamp a level higher than the
      // stream actually needs (Intel's in particular), and some D3D11VA
      // drivers refuse frames on the advertised level alone even though
      // they decode them perfectly. This is the standard client-side
      // tolerance for that -- Moonlight sets it for every hwaccel it
      // opens.
      c_->hwaccel_flags |= AV_HWACCEL_FLAG_IGNORE_LEVEL;
    }

    if (!(pkt_ = av_packet_alloc())) {
      LOG_ERROR(std::string("av_packet_alloc failed"));
      return -1;
    }

    if (!(frame_ = av_frame_alloc())) {
      LOG_ERROR(std::string("av_frame_alloc failed"));
      return -1;
    }

    if ((ret = avcodec_open2(c_, codec, NULL)) != 0) {
      LOG_ERROR(std::string("avcodec_open2 failed, ret = ") + av_err2str(ret));
      return -1;
    }
#ifdef CFG_PKG_TRACE
    in_ = 0;
    out_ = 0;
#endif

    return 0;
  }

  int decode(const uint8_t *data, int length, const void *obj) {
    int ret = -1;
#ifdef CFG_PKG_TRACE
    in_++;
    LOG_DEBUG(std::string("delay DI: in:") + in_ + " out:" + out_);
#endif

    if (!data || !length) {
      LOG_ERROR(std::string("illegal decode parameter"));
      return -1;
    }
    pkt_->data = (uint8_t *)data;
    pkt_->size = length;
    ret = do_decode(obj);
    return ret;
  }

private:
  // Pulls every currently-available decoded frame out of the decoder via
  // avcodec_receive_frame(), invoking callback_ for each one and setting
  // decoded=true if at least one came out. AVERROR(EAGAIN)/AVERROR_EOF
  // here just mean "nothing more is ready right now" -- that is not a
  // failure, it's the normal way this loop ends. Returns false only for
  // a genuine decode error (bad hw frame, transfer failure, or anything
  // other than EAGAIN/EOF from avcodec_receive_frame).
  bool drain_available_frames(const void *obj, bool &decoded) {
    AVFrame *tmp_frame = NULL;
    int ret;
    while ((ret = avcodec_receive_frame(c_, frame_)) == 0) {
      if (hwaccel_) {
        if (!frame_->hw_frames_ctx) {
          LOG_ERROR(std::string("hw_frames_ctx is NULL"));
          return false;
        }
        // av_hwframe_transfer_data() allocates sw_frame_ only on the
        // first call, sizing it from that first frame; every call after
        // that transfers into whatever geometry it already has. A stream
        // that changes resolution mid-session therefore fails with
        // EINVAL on every frame from that point on -- and keeps failing,
        // since nothing here ever reconsiders the buffer. Dropping it
        // when the geometry no longer matches makes the next transfer
        // re-allocate.
        if (sw_frame_->buf[0] && (sw_frame_->width != frame_->width ||
                                  sw_frame_->height != frame_->height)) {
          av_frame_unref(sw_frame_);
        }
        if ((ret = av_hwframe_transfer_data(sw_frame_, frame_, 0)) < 0) {
          LOG_ERROR(std::string("av_hwframe_transfer_data failed, ret = ") +
                    av_err2str(ret));
          return false;
        }
        tmp_frame = sw_frame_;
      } else {
        tmp_frame = frame_;
      }
      decoded = true;
#ifdef CFG_PKG_TRACE
      out_++;
      LOG_DEBUG(std::string("delay DO: in:") + in_ + " out:" + out_);
#endif
      // FF_API_FRAME_KEY is FFmpeg's standard deprecation-guard
      // convention: it's 1 while the *old* AVFrame::key_frame field
      // still exists (deprecated but present), and 0 once that field
      // has actually been removed -- at which point AV_FRAME_FLAG_KEY
      // is the only way to ask. The two branches below were swapped in
      // the original file (using the new flag while the old field was
      // still available, and falling back to the old field -- which by
      // definition no longer exists -- once FF_API_FRAME_KEY says it's
      // gone). That compiles fine against any FFmpeg build old enough
      // to still have key_frame, and fails outright (`'key_frame': is
      // not a member of 'AVFrame'`) the moment it's built against one
      // that actually removed it (FFmpeg 7.0+).
#if FF_API_FRAME_KEY
      int key_frame = frame_->key_frame;
#else
      int key_frame = frame_->flags & AV_FRAME_FLAG_KEY;
#endif
      callback_(obj, tmp_frame->width, tmp_frame->height,
                (AVPixelFormat)tmp_frame->format, tmp_frame->linesize,
                tmp_frame->data, key_frame);
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
      LOG_ERROR(std::string("avcodec_receive_frame failed, ret = ") + av_err2str(ret));
      return false;
    }
    return true;
  }

  // Returns 0 when the packet was accepted and nothing went wrong,
  // whether or not it produced a picture, and -1 only for a genuine
  // decode failure.
  //
  // Those two were the same value until now, and conflating them is
  // expensive for a live screen-share client. A decoder that has
  // accepted a packet and not yet produced a picture is in a completely
  // ordinary state -- it is holding a frame it will emit shortly, or the
  // packet carried only parameter sets, or a hwaccel surface was
  // momentarily unavailable. Reporting that as a decode error taught the
  // caller to treat it as a broken reference chain, which it answers by
  // asking the sender for a keyframe; on a link where it happens with
  // any regularity that becomes a standing keyframe request, and every
  // keyframe granted is bitrate spent re-sending a picture that was
  // never lost. The caller can still see exactly how many pictures came
  // out -- the callback pushes each one -- so nothing is hidden by
  // this; it just stops being called an error.
  int do_decode(const void *obj) {
    bool decoded = false;
    bool failed = false;
    auto start = util::now();

    // avcodec_send_packet() can legitimately return AVERROR(EAGAIN).
    // Per its own documentation (avcodec.h): "input is not accepted in
    // the current state - user must read output with
    // avcodec_receive_frame() (once all output is read, the packet
    // should be resent, and the call will not fail with EAGAIN)." That
    // is normal backpressure, not a decode error -- it just means
    // avcodec_receive_frame() hasn't been called enough yet to make room
    // for this packet. The previous version of this function treated
    // *any* ret < 0 from send_packet, including this specific documented
    // and retriable case, as an immediate hard failure with no attempt
    // to drain and resend. That only ever bites when the caller can't
    // keep avcodec_receive_frame() fully drained between decode() calls
    // -- e.g. a software-decode fallback path that's a bit slower than
    // the incoming frame rate -- which silently turned ordinary,
    // recoverable backpressure into reported decode failures.
    int send_ret;
    for (;;) {
      send_ret = avcodec_send_packet(c_, pkt_);
      if (send_ret == 0) {
        break; // accepted -- fall through to the unconditional drain below
      }
      if (send_ret != AVERROR(EAGAIN)) {
        LOG_ERROR(std::string("avcodec_send_packet failed, ret = ") + av_err2str(send_ret));
        failed = true;
        goto _exit;
      }
      // EAGAIN: drain whatever's ready, then retry sending this same
      // packet, bounded by the same ENCODE_TIMEOUT_MS budget the
      // original receive loop used.
      if (!drain_available_frames(obj, decoded)) {
        failed = true;
        goto _exit; // drain_available_frames already logged the real error
      }
      if (util::elapsed_ms(start) >= ENCODE_TIMEOUT_MS) {
        LOG_ERROR(std::string("avcodec_send_packet kept returning EAGAIN past the ") +
                  std::to_string(ENCODE_TIMEOUT_MS) + "ms budget");
        failed = true;
        goto _exit;
      }
    }

    // Packet accepted -- collect whatever it produced. With
    // AV_CODEC_FLAG_LOW_DELAY and max_b_frames=0 (see set_av_codec_ctx)
    // this is normally exactly one frame, but draining fully here is
    // still correct and matches ffmpeg's own decode examples.
    if (!drain_available_frames(obj, decoded)) {
      failed = true;
    }

  _exit:
    av_packet_unref(pkt_);
    (void)decoded;
    return failed ? -1 : 0;
  }

  bool check_support() {
#ifdef _WIN32
    if (device_type_ == AV_HWDEVICE_TYPE_D3D11VA) {
      if (!c_->hw_device_ctx) {
        LOG_ERROR(std::string("hw_device_ctx is NULL"));
        return false;
      }
      AVHWDeviceContext *deviceContext =
          (AVHWDeviceContext *)hw_device_ctx_->data;
      if (!deviceContext) {
        LOG_ERROR(std::string("deviceContext is NULL"));
        return false;
      }
      AVD3D11VADeviceContext *d3d11vaDeviceContext =
          (AVD3D11VADeviceContext *)deviceContext->hwctx;
      if (!d3d11vaDeviceContext) {
        LOG_ERROR(std::string("d3d11vaDeviceContext is NULL"));
        return false;
      }
      ID3D11Device *device = d3d11vaDeviceContext->device;
      if (!device) {
        LOG_ERROR(std::string("device is NULL"));
        return false;
      }
      std::unique_ptr<NativeDevice> native_ = std::make_unique<NativeDevice>();
      if (!native_) {
        LOG_ERROR(std::string("Failed to create native device"));
        return false;
      }
      if (!native_->Init(0, (ID3D11Device *)device, 0)) {
        LOG_ERROR(std::string("Failed to init native device"));
        return false;
      }
      if (!native_->support_decode(data_format_)) {
        LOG_ERROR(std::string("Failed to check support ") + name_);
        return false;
      }
      return true;
    } else {
      return true;
    }
#else
    return true;
#endif
  }
};

} // namespace

extern "C" void ffmpeg_ram_free_decoder(FFmpegRamDecoder *decoder) {
  try {
    if (!decoder)
      return;
    decoder->free_decoder();
    delete decoder;
    decoder = NULL;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_ram_free_decoder exception:") + e.what());
  }
}

extern "C" FFmpegRamDecoder *
ffmpeg_ram_new_decoder(const char *name, int device_type, int thread_count,
                       RamDecodeCallback callback) {
  FFmpegRamDecoder *decoder = NULL;
  try {
    decoder = new FFmpegRamDecoder(name, device_type, thread_count, callback);
    if (decoder) {
      if (decoder->reset() == 0) {
        return decoder;
      }
    }
  } catch (std::exception &e) {
    LOG_ERROR(std::string("new decoder exception:") + e.what());
  }
  if (decoder) {
    decoder->free_decoder();
    delete decoder;
    decoder = NULL;
  }
  return NULL;
}

extern "C" int ffmpeg_ram_decode(FFmpegRamDecoder *decoder, const uint8_t *data,
                                 int length, const void *obj) {
  try {
    int ret = decoder->decode(data, length, obj);
    if (DataFormat::H265 == decoder->data_format_ && util_decode::has_flag_could_not_find_ref_with_poc()) {
      return HWCODEC_ERR_HEVC_COULD_NOT_FIND_POC;
    } else {
      return ret == 0 ? HWCODEC_SUCCESS : HWCODEC_ERR_COMMON;
    }
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_ram_decode exception:") + e.what());
  }
  return HWCODEC_ERR_COMMON;
}