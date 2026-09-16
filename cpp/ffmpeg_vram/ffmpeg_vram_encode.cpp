extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}

#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif

#include <memory>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "callback.h"
#include "common.h"
#include "system.h"

#define LOG_MODULE "FFMPEG_VRAM_ENC"
#include <log.h>
#include <util.h>

namespace {

void lockContext(void *lock_ctx);
void unlockContext(void *lock_ctx);

enum class EncoderDriver {
  NVENC,
  AMF,
  QSV,
};

class Encoder {
public:
  Encoder(EncoderDriver driver, const char *name, AVHWDeviceType device_type,
          AVHWDeviceType derived_device_type, AVPixelFormat hw_pixfmt,
          AVPixelFormat sw_pixfmt) {
    driver_ = driver;
    name_ = name;
    device_type_ = device_type;
    derived_device_type_ = derived_device_type;
    hw_pixfmt_ = hw_pixfmt;
    sw_pixfmt_ = sw_pixfmt;
  };
  EncoderDriver driver_;
  std::string name_;
  AVHWDeviceType device_type_;
  AVHWDeviceType derived_device_type_;
  AVPixelFormat hw_pixfmt_;
  AVPixelFormat sw_pixfmt_;
};

class FFmpegVRamEncoder {
public:
  AVCodecContext *c_ = NULL;
  AVBufferRef *hw_device_ctx_ = NULL;
  AVFrame *frame_ = NULL;
  AVFrame *mapped_frame_ = NULL;
  ID3D11Texture2D *encode_texture_ = NULL; // no free
  AVPacket *pkt_ = NULL;
  std::unique_ptr<NativeDevice> native_ = nullptr;
  ID3D11Device *d3d11Device_ = NULL;
  ID3D11DeviceContext *d3d11DeviceContext_ = NULL;
  std::unique_ptr<Encoder> encoder_ = nullptr;

  void *handle_ = nullptr;
  int64_t luid_;
  DataFormat dataFormat_;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t kbs_;
  int32_t framerate_;
  int32_t gop_;
  // Set by ffmpeg_vram_set_force_idr, consumed by the next do_encode.
  // Lets a caller get a fresh IDR on demand without destroying and
  // rebuilding the encoder (which costs a full re-probe and hundreds of
  // milliseconds of dropped capture).
  bool force_idr_ = false;

  const int align_ = 0;
  const bool full_range_ = false;
  const bool bt709_ = false;
  FFmpegVRamEncoder(void *handle, int64_t luid, DataFormat dataFormat,
                    int32_t width, int32_t height, int32_t kbs,
                    int32_t framerate, int32_t gop) {
    handle_ = handle;
    luid_ = luid;
    dataFormat_ = dataFormat;
    width_ = width;
    height_ = height;
    kbs_ = kbs;
    framerate_ = framerate;
    gop_ = gop;
  }

  ~FFmpegVRamEncoder() {}

  bool init() {
    const AVCodec *codec = NULL;
    int ret;

    native_ = std::make_unique<NativeDevice>();
    if (!native_->Init(luid_, (ID3D11Device *)handle_)) {
      LOG_ERROR(std::string("NativeDevice init failed"));
      return false;
    }
    d3d11Device_ = native_->device_.Get();
    d3d11Device_->AddRef();
    d3d11DeviceContext_ = native_->context_.Get();
    d3d11DeviceContext_->AddRef();

    AdapterVendor vendor = native_->GetVendor();
    if (!choose_encoder(vendor)) {
      return false;
    }
          LOG_INFO(std::string("encoder name: ") + encoder_->name_);
    if (!(codec = avcodec_find_encoder_by_name(encoder_->name_.c_str()))) {
      LOG_ERROR(std::string("Codec ") + encoder_->name_ + " not found");
      return false;
    }

    if (!(c_ = avcodec_alloc_context3(codec))) {
      LOG_ERROR(std::string("Could not allocate video codec context"));
      return false;
    }

    /* resolution must be a multiple of two */
    c_->width = width_;
    c_->height = height_;
    c_->pix_fmt = encoder_->hw_pixfmt_;
    c_->sw_pix_fmt = encoder_->sw_pixfmt_;
    util_encode::set_av_codec_ctx(c_, encoder_->name_, kbs_, gop_, framerate_);
    if (!util_encode::set_lantency_free(c_->priv_data, encoder_->name_)) {
      return false;
    }
    // util_encode::set_quality(c_->priv_data, encoder_->name_, Quality_Default);
    util_encode::set_rate_control(c_, encoder_->name_, RC_CBR, -1);
    util_encode::set_others(c_->priv_data, encoder_->name_);

    hw_device_ctx_ = av_hwdevice_ctx_alloc(encoder_->device_type_);
    if (!hw_device_ctx_) {
      LOG_ERROR(std::string("av_hwdevice_ctx_create failed"));
      return false;
    }

    AVHWDeviceContext *deviceContext =
        (AVHWDeviceContext *)hw_device_ctx_->data;
    AVD3D11VADeviceContext *d3d11vaDeviceContext =
        (AVD3D11VADeviceContext *)deviceContext->hwctx;
    d3d11vaDeviceContext->device = d3d11Device_;
    d3d11vaDeviceContext->device_context = d3d11DeviceContext_;
    d3d11vaDeviceContext->lock = lockContext;
    d3d11vaDeviceContext->unlock = unlockContext;
    d3d11vaDeviceContext->lock_ctx = this;
    ret = av_hwdevice_ctx_init(hw_device_ctx_);
    if (ret < 0) {
      LOG_ERROR(std::string("av_hwdevice_ctx_init failed, ret = ") + av_err2str(ret));
      return false;
    }
    if (encoder_->derived_device_type_ != AV_HWDEVICE_TYPE_NONE) {
      AVBufferRef *derived_context = nullptr;
      ret = av_hwdevice_ctx_create_derived(
          &derived_context, encoder_->derived_device_type_, hw_device_ctx_, 0);
      if (ret) {
            LOG_ERROR(std::string("av_hwdevice_ctx_create_derived failed, err = ") +
              av_err2str(ret));
        return false;
      }
      av_buffer_unref(&hw_device_ctx_);
      hw_device_ctx_ = derived_context;
    }
    c_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    if (!set_hwframe_ctx()) {
      return false;
    }

    if (!(pkt_ = av_packet_alloc())) {
      LOG_ERROR(std::string("Could not allocate video packet"));
      return false;
    }

    if ((ret = avcodec_open2(c_, codec, NULL)) < 0) {
      LOG_ERROR(std::string("avcodec_open2 failed, ret = ") + av_err2str(ret) +
                ", name: " + encoder_->name_);
      return false;
    }

    if (!(frame_ = av_frame_alloc())) {
      LOG_ERROR(std::string("Could not allocate video frame"));
      return false;
    }
    frame_->format = c_->pix_fmt;
    frame_->width = c_->width;
    frame_->height = c_->height;
    frame_->color_range = c_->color_range;
    frame_->color_primaries = c_->color_primaries;
    frame_->color_trc = c_->color_trc;
    frame_->colorspace = c_->colorspace;
    frame_->chroma_location = c_->chroma_sample_location;

    if ((ret = av_hwframe_get_buffer(c_->hw_frames_ctx, frame_, 0)) < 0) {
      LOG_ERROR(std::string("av_frame_get_buffer failed, ret = ") + av_err2str(ret));
      return false;
    }
    if (frame_->format == AV_PIX_FMT_QSV) {
      mapped_frame_ = av_frame_alloc();
      if (!mapped_frame_) {
        LOG_ERROR(std::string("Could not allocate mapped video frame"));
        return false;
      }
      mapped_frame_->format = AV_PIX_FMT_D3D11;
      ret = av_hwframe_map(mapped_frame_, frame_,
                           AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
      if (ret) {
        LOG_ERROR(std::string("av_hwframe_map failed, err = ") + av_err2str(ret));
        return false;
      }
      encode_texture_ = (ID3D11Texture2D *)mapped_frame_->data[0];
    } else {
      encode_texture_ = (ID3D11Texture2D *)frame_->data[0];
    }

    return true;
  }

  int encode(void *texture, EncodeCallback callback, void *obj, int64_t ms) {

    if (!convert(texture))
      return -1;

    return do_encode(callback, obj, ms);
  }

  void destroy() {
    if (c_ && avcodec_is_open(c_)) {
      // avcodec_free_context() used to run directly here, with no flush
      // step first. FFmpeg's own documented shutdown sequence for an
      // encoder --send a NULL frame to signal end-of-stream, then drain
      // every packet still buffered inside the encoder via
      // avcodec_receive_packet() until it returns AVERROR_EOF -- was
      // skipped entirely. With max_b_frames=0 and this project's own
      // encode()/do_encode() already draining fully after every send
      // (see do_encode() above) there is normally nothing left queued in
      // steady state, but destroy() can run at any point relative to
      // do_encode(), including immediately after a send that hit EAGAIN
      // (see do_encode()'s own retry loop) or simply whenever this
      // project's "Recreating video encoder" forced-keyframe path
      // decides to tear this encoder down (see peer/lan.rs). Whatever
      // the hardware encoder still had buffered internally at that exact
      // instant was silently discarded by freeing the context out from
      // under it -- on a real Intel/QSV machine, a field log showed
      // exactly this sequence (a burst of forced recreations under load)
      // immediately followed by the whole agent process going silent.
      //
      // The flushed packets are deliberately discarded rather than
      // forwarded to the client: this destroy()/recreate cycle exists
      // specifically to hand the client a brand new, self-contained IDR
      // next, so anything still buffered from the outgoing encoder
      // generation is already obsolete.
      int send_ret = avcodec_send_frame(c_, nullptr);
      if (send_ret == 0 || send_ret == AVERROR_EOF) {
        bool discarded_encoded = false;
        drain_available_packets(nullptr, nullptr, discarded_encoded);
      } else if (send_ret != AVERROR(EAGAIN)) {
        // Genuinely can't flush (e.g. the context was never successfully
        // opened, or the device is already gone) -- nothing more to do
        // here, fall through to freeing the context regardless.
        LOG_ERROR(std::string("avcodec_send_frame(NULL) for flush failed, ret = ") +
                  av_err2str(send_ret));
      }
    }
    if (pkt_)
      av_packet_free(&pkt_);
    if (frame_)
      av_frame_free(&frame_);
    if (mapped_frame_)
      av_frame_free(&mapped_frame_);
    if (c_)
      avcodec_free_context(&c_);
    if (hw_device_ctx_) {
      av_buffer_unref(&hw_device_ctx_);
      // AVHWDeviceContext takes ownership of d3d11 object
      d3d11Device_ = nullptr;
      d3d11DeviceContext_ = nullptr;
    } else {
      SAFE_RELEASE(d3d11Device_);
      SAFE_RELEASE(d3d11DeviceContext_);
    }
  }

  int set_bitrate(int kbs) {
    return util_encode::change_bit_rate(c_, encoder_->name_, kbs) ? 0 : -1;
  }

  int set_framerate(int framerate) {
    if (framerate <= 0) {
      return -1;
    }
    // time_base is deliberately NOT touched. This encoder is fed
    // timestamps in milliseconds (see do_encode's `frame_->pts = ms`),
    // which is why set_av_codec_ctx opens the context with a 1/1000 time
    // base. Rewriting it to 1/framerate here reinterpreted every
    // timestamp already in flight: at 6fps a pts of 200 -- 200ms --
    // became 200/6 = 33 seconds, so rate control saw a stream that had
    // barely advanced and allocated a few hundred bytes per frame. A real
    // session measured 540KB of 1080p H.265 across two minutes before
    // this was found.
    //
    // Note that most backends read the frame rate when the context is
    // opened, so this may legitimately change nothing; the point is that
    // it must not change the wrong thing.
    framerate_ = framerate;
    c_->framerate = av_make_q(framerate, 1);
    return 0;
  }

private:
  bool choose_encoder(AdapterVendor vendor) {
    if (ADAPTER_VENDOR_NVIDIA == vendor) {
      const char *name = nullptr;
      if (dataFormat_ == H264) {
        name = "h264_nvenc";
      } else if (dataFormat_ == H265) {
        name = "hevc_nvenc";
      } else {
        LOG_ERROR(std::string("Unsupported data format: ") + std::to_string(dataFormat_));
        return false;
      }
      encoder_ = std::make_unique<Encoder>(
          EncoderDriver::NVENC, name, AV_HWDEVICE_TYPE_D3D11VA,
          AV_HWDEVICE_TYPE_NONE, AV_PIX_FMT_D3D11, AV_PIX_FMT_NV12);
      return true;
    } else if (ADAPTER_VENDOR_AMD == vendor) {
      const char *name = nullptr;
      if (dataFormat_ == H264) {
        name = "h264_amf";
      } else if (dataFormat_ == H265) {
        name = "hevc_amf";
      } else {
        LOG_ERROR(std::string("Unsupported data format: ") + std::to_string(dataFormat_));
        return false;
      }
      encoder_ = std::make_unique<Encoder>(
          EncoderDriver::AMF, name, AV_HWDEVICE_TYPE_D3D11VA,
          AV_HWDEVICE_TYPE_NONE, AV_PIX_FMT_D3D11, AV_PIX_FMT_NV12);
      return true;
    } else if (ADAPTER_VENDOR_INTEL == vendor) {
      const char *name = nullptr;
      if (dataFormat_ == H264) {
        name = "h264_qsv";
      } else if (dataFormat_ == H265) {
        name = "hevc_qsv";
      } else {
        LOG_ERROR(std::string("Unsupported data format: ") + std::to_string(dataFormat_));
        return false;
      }
      encoder_ = std::make_unique<Encoder>(
          EncoderDriver::QSV, name, AV_HWDEVICE_TYPE_D3D11VA,
          AV_HWDEVICE_TYPE_QSV, AV_PIX_FMT_QSV, AV_PIX_FMT_NV12);
      return true;
    } else {
      LOG_ERROR(std::string("Unsupported vendor: ") + std::to_string(vendor));
      return false;
    }
    return false;
  }
  // Pulls every currently-available encoded packet out via
  // avcodec_receive_packet(), invoking callback_ for each one and setting
  // encoded=true if at least one came out. AVERROR(EAGAIN)/AVERROR_EOF
  // here just mean "nothing more is ready right now" -- not a failure,
  // just the normal way this loop ends. Returns false only for a genuine
  // encode error. Always unrefs pkt_ before returning, regardless of
  // outcome -- matching the original code's own unconditional
  // av_packet_unref(pkt_) at its single _exit: label, since
  // avcodec_receive_packet() only unrefs pkt_ itself at the *start* of
  // each call, not after the last one before this function hands control
  // back.
  bool drain_available_packets(EncodeCallback callback, const void *obj,
                                bool &encoded) {
    int ret;
    while ((ret = avcodec_receive_packet(c_, pkt_)) == 0) {
      if (!pkt_->data || !pkt_->size) {
        LOG_ERROR(std::string("avcodec_receive_packet failed, pkt size is 0"));
        av_packet_unref(pkt_);
        return false;
      }
      encoded = true;
      if (callback)
        callback(pkt_->data, pkt_->size, pkt_->flags & AV_PKT_FLAG_KEY, obj,
                 pkt_->pts);
    }
    av_packet_unref(pkt_);
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
      LOG_ERROR(std::string("avcodec_receive_packet failed, ret = ") + av_err2str(ret));
      return false;
    }
    return true;
  }

  int do_encode(EncodeCallback callback, const void *obj, int64_t ms) {
    bool encoded = false;
    frame_->pts = ms;
    // FFmpeg's standard force-keyframe contract: an input frame tagged
    // AV_PICTURE_TYPE_I makes the encoder emit an IDR (with fresh
    // SPS/PPS/VPS) for it. Honoured by the hardware wrappers this file
    // drives -- amfenc maps it to FORCE_PICTURE_TYPE_IDR, nvenc to
    // NV_ENC_PIC_FLAG_FORCEIDR, qsv to MFX_FRAMETYPE_IDR. Cleared
    // immediately so exactly one frame is forced per request; AV_PICTURE
    // _TYPE_NONE is the "encoder decides" default the rest of the time.
    //
    // Set once, before the send loop below rather than inside it: an
    // EAGAIN retry re-sends this same frame, and the tag has to survive
    // that or the forced keyframe would be lost exactly when the
    // encoder is under the backpressure that made it necessary.
    frame_->pict_type = force_idr_ ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    force_idr_ = false;
    auto start = util::now();

    // avcodec_send_frame() can legitimately return AVERROR(EAGAIN): per
    // its own documentation (avcodec.h), this means "input is not
    // accepted in the current state - user must read output with
    // avcodec_receive_packet() (once all output is read, the packet
    // should be resent, and the call will not fail with EAGAIN)." That's
    // normal backpressure, not an encode error -- the same category of
    // bug already fixed on the decode side of this project
    // (ffmpeg_ram_decode.cpp's do_decode()), just here on the encode
    // side, in a file shared across every vendor whose device selection
    // picks the FFmpeg driver rather than a vendor-native SDK path (not
    // AMD- or NVIDIA- or Intel-specific). The previous version of this
    // function treated *any* ret < 0 from send_frame, including this
    // specific documented and retriable case, as an immediate hard
    // failure with no attempt to drain and resend -- which only bites
    // when the caller can't keep avcodec_receive_packet() fully drained
    // between calls, e.g. right after this project's own
    // "Recreating video encoder" forced-keyframe path fires repeatedly
    // in a burst (see peer/lan.rs's RECREATE_COOLDOWN) -- exactly the
    // condition a field log showed immediately preceding a silent agent
    // process death on real Intel/QSV hardware.
    int send_ret;
    for (;;) {
      send_ret = avcodec_send_frame(c_, frame_);
      if (send_ret == 0) {
        break; // accepted -- fall through to the unconditional drain below
      }
      if (send_ret != AVERROR(EAGAIN)) {
        LOG_ERROR(std::string("avcodec_send_frame failed, ret = ") + av_err2str(send_ret));
        return send_ret;
      }
      // EAGAIN: drain whatever's ready, then retry sending this same
      // frame, bounded by the same ENCODE_TIMEOUT_MS budget the
      // original receive loop used.
      if (!drain_available_packets(callback, obj, encoded)) {
        return -1; // drain_available_packets already logged the real error
      }
      if (util::elapsed_ms(start) >= ENCODE_TIMEOUT_MS) {
        LOG_ERROR(std::string("avcodec_send_frame kept returning EAGAIN past the ") +
                  std::to_string(ENCODE_TIMEOUT_MS) + "ms budget");
        return -1;
      }
    }

    // Frame accepted -- collect whatever it produced. With max_b_frames=0
    // (see set_av_codec_ctx) this is normally exactly one packet, but
    // draining fully here is still correct and matches ffmpeg's own
    // encode examples.
    if (!drain_available_packets(callback, obj, encoded)) {
      return -1;
    }
    return encoded ? 0 : -1;
  }

  bool convert(void *texture) {
    if (frame_->format == AV_PIX_FMT_D3D11 ||
        frame_->format == AV_PIX_FMT_QSV) {
      ID3D11Texture2D *texture2D = (ID3D11Texture2D *)encode_texture_;
      D3D11_TEXTURE2D_DESC desc;
      texture2D->GetDesc(&desc);
      if (desc.Format != DXGI_FORMAT_NV12) {
        LOG_ERROR(std::string("convert: texture format mismatch, ") +
                  std::to_string(desc.Format) +
                  " != " + std::to_string(DXGI_FORMAT_NV12));
        return false;
      }
      DXGI_COLOR_SPACE_TYPE colorSpace_in =
          DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
      DXGI_COLOR_SPACE_TYPE colorSpace_out;
      if (bt709_) {
        if (full_range_) {
          colorSpace_out = DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
        } else {
          colorSpace_out = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        }
      } else {
        if (full_range_) {
          colorSpace_out = DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601;
        } else {
          colorSpace_out = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
        }
      }
      if (!native_->BgraToNv12((ID3D11Texture2D *)texture, texture2D, width_,
                               height_, colorSpace_in, colorSpace_out)) {
        LOG_ERROR(std::string("convert: BgraToNv12 failed"));
        return false;
      }
      return true;
    } else {
      LOG_ERROR(std::string("convert: unsupported format, ") +
                std::to_string(frame_->format));
      return false;
    }
  }

  bool set_hwframe_ctx() {
    AVBufferRef *hw_frames_ref;
    AVHWFramesContext *frames_ctx = NULL;
    int err = 0;
    bool ret = true;

    if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx_))) {
      LOG_ERROR(std::string("av_hwframe_ctx_alloc failed."));
      return false;
    }
    frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    frames_ctx->format = encoder_->hw_pixfmt_;
    frames_ctx->sw_format = encoder_->sw_pixfmt_;
    frames_ctx->width = width_;
    frames_ctx->height = height_;
    frames_ctx->initial_pool_size = 0;
    if (encoder_->device_type_ == AV_HWDEVICE_TYPE_D3D11VA) {
      frames_ctx->initial_pool_size = 1;
      AVD3D11VAFramesContext *frames_hwctx =
          (AVD3D11VAFramesContext *)frames_ctx->hwctx;
      frames_hwctx->BindFlags = D3D11_BIND_RENDER_TARGET;
      frames_hwctx->MiscFlags = 0;
    }
    if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
      LOG_ERROR(std::string("av_hwframe_ctx_init failed."));
      av_buffer_unref(&hw_frames_ref);
      return false;
    }
    c_->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!c_->hw_frames_ctx) {
      LOG_ERROR(std::string("av_buffer_ref failed"));
      ret = false;
    }
    av_buffer_unref(&hw_frames_ref);

    return ret;
  }
};

void lockContext(void *lock_ctx) { (void)lock_ctx; }

void unlockContext(void *lock_ctx) { (void)lock_ctx; }

} // namespace

extern "C" {
FFmpegVRamEncoder *ffmpeg_vram_new_encoder(void *handle, int64_t luid,
                                           DataFormat dataFormat, int32_t width,
                                           int32_t height, int32_t kbs,
                                           int32_t framerate, int32_t gop) {
  FFmpegVRamEncoder *encoder = NULL;
  try {
    encoder = new FFmpegVRamEncoder(handle, luid, dataFormat, width,
                                    height, kbs, framerate, gop);
    if (encoder) {
      if (encoder->init()) {
        return encoder;
      }
    }
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("new FFmpegVRamEncoder failed, ") + std::string(e.what()));
  }
  if (encoder) {
    encoder->destroy();
    delete encoder;
    encoder = NULL;
  }
  return NULL;
}

int ffmpeg_vram_encode(FFmpegVRamEncoder *encoder, void *texture,
                       EncodeCallback callback, void *obj, int64_t ms) {
  try {
    return encoder->encode(texture, callback, obj, ms);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_vram_encode failed, ") + std::string(e.what()));
  }
  return -1;
}

void ffmpeg_vram_destroy_encoder(FFmpegVRamEncoder *encoder) {
  try {
    if (!encoder)
      return;
    encoder->destroy();
    delete encoder;
    encoder = NULL;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("free encoder failed, ") + std::string(e.what()));
  }
}

int ffmpeg_vram_set_bitrate(FFmpegVRamEncoder *encoder, int kbs) {
  try {
    return encoder->set_bitrate(kbs);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_ram_set_bitrate failed, ") + std::string(e.what()));
  }
  return -1;
}

int ffmpeg_vram_set_framerate(FFmpegVRamEncoder *encoder, int32_t framerate) {
  try {
    return encoder->set_bitrate(framerate);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("ffmpeg_vram_set_framerate failed, ") + std::string(e.what()));
  }
  return -1;
}
int ffmpeg_vram_set_force_idr(void *encoder) {
  try {
    FFmpegVRamEncoder *enc = (FFmpegVRamEncoder *)encoder;
    if (!enc) {
      return -1;
    }
    enc->force_idr_ = true;
    return 0;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("set force idr failed: ") + e.what());
  }
  return -1;
}


int ffmpeg_vram_test_encode(int64_t *outLuids, int32_t *outVendors, int64_t *outElapsedMs, int32_t maxDescNum,
                            int32_t *outDescNum, DataFormat dataFormat,
                            int32_t width, int32_t height, int32_t kbs,
                            int32_t framerate, int32_t gop,
                            const int64_t *excludedLuids, const int32_t *excludeFormats, int32_t excludeCount) {
  try {
    int count = 0;
    struct VendorMapping {
       AdapterVendor adapter_vendor;
       int driver_vendor;
    };
    VendorMapping vendors[] = {
      {ADAPTER_VENDOR_INTEL, VENDOR_INTEL},
      {ADAPTER_VENDOR_NVIDIA, VENDOR_NV},
      {ADAPTER_VENDOR_AMD, VENDOR_AMD}
    };
    
    for (auto vendorMap : vendors) {
      Adapters adapters;
      if (!adapters.Init(vendorMap.adapter_vendor))
        continue;
      for (auto &adapter : adapters.adapters_) {
        int64_t currentLuid = LUID(adapter.get()->desc1_);
        if (util::skip_test(excludedLuids, excludeFormats, excludeCount, currentLuid, dataFormat)) {
          continue;
        }
        
        FFmpegVRamEncoder *e = (FFmpegVRamEncoder *)ffmpeg_vram_new_encoder(
            (void *)adapter.get()->device_.Get(), currentLuid,
            dataFormat, width, height, kbs, framerate, gop);
        if (!e)
          continue;
        if (e->native_->EnsureTexture(e->width_, e->height_)) {
          e->native_->next();
          int32_t key_obj = 0;
          auto start = util::now();
          bool succ = ffmpeg_vram_encode(e, e->native_->GetCurrentTexture(), util_encode::vram_encode_test_callback,
                                 &key_obj, 0) == 0 && key_obj == 1;
          int64_t elapsed = util::elapsed_ms(start);
          if (succ && elapsed < TEST_TIMEOUT_MS) {
            outLuids[count] = currentLuid;
            outVendors[count] = (int32_t)vendorMap.driver_vendor;  // Map adapter vendor to driver vendor
            outElapsedMs[count] = elapsed;
            count += 1;
          }
        }
        e->destroy();
        delete e;
        e = nullptr;
        if (count >= maxDescNum)
          break;
      }
      if (count >= maxDescNum)
        break;
    }
    *outDescNum = count;
    return 0;
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("test failed: ") + e.what());
  }
  return -1;
}

} // extern "C"