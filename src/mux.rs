#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

include!(concat!(env!("OUT_DIR"), "/mux_ffi.rs"));

use log::{error, trace};

use crate::ffmpeg::{av_log_get_level, AV_LOG_ERROR};
use crate::common::DataFormat;
use std::ffi::{c_void, CString};

#[derive(Debug, Clone, PartialEq)]
pub struct MuxContext {
    pub filename: String,
    pub width: usize,
    pub height: usize,
    /// H.264, H.265 or AV1.
    pub codec: DataFormat,
    pub framerate: usize,
}

pub struct Muxer {
    inner: *mut c_void,
    pub ctx: MuxContext,
}

unsafe impl Send for Muxer {}
unsafe impl Sync for Muxer {}

impl Muxer {
    pub fn new(ctx: MuxContext) -> Result<Self, ()> {
        unsafe {
            let inner = hwcodec_new_muxer(
                CString::new(ctx.filename.as_str())
                    .map_err(|_| ())?
                    .as_ptr(),
                ctx.width as _,
                ctx.height as _,
                ctx.codec as i32,
                ctx.framerate as _,
            );
            if inner.is_null() {
                return Err(());
            }

            Ok(Muxer { inner, ctx })
        }
    }

    /// Writes one encoded frame shown at `pts_ms` (any clock, in
    /// milliseconds; the file starts at the first key frame's). Frames
    /// before the first key frame are refused with -2.
    pub fn write_video(&mut self, data: &[u8], key: bool, pts_ms: i64) -> Result<(), i32> {
        unsafe {
            let result = hwcodec_write_video_frame(
                self.inner,
                (*data).as_ptr(),
                data.len() as _,
                pts_ms,
                if key { 1 } else { 0 },
            );
            if result != 0 {
                if av_log_get_level() >= AV_LOG_ERROR as _ {
                    error!("Error write_video: {}", result);
                }
                return Err(result);
            }
            Ok(())
        }
    }

    pub fn write_tail(&mut self) -> Result<(), i32> {
        unsafe {
            let result = hwcodec_write_tail(self.inner);
            if result != 0 {
                if av_log_get_level() >= AV_LOG_ERROR as _ {
                    error!("Error write_tail: {}", result);
                }
                return Err(result);
            }
            Ok(())
        }
    }
}

impl Drop for Muxer {
    fn drop(&mut self) {
        unsafe {
            hwcodec_free_muxer(self.inner);
            self.inner = std::ptr::null_mut();
            trace!("Muxer dropped");
        }
    }
}
