// SPDX-License-Identifier: Apache-2.0

use std::{
    ffi::c_void,
    io::{self, Write},
    panic::catch_unwind,
    slice,
};

type WriteBytes = unsafe extern "C" fn(*mut c_void, *const u8, usize) -> usize;

struct Output {
    context: *mut c_void,
    write_bytes: WriteBytes,
}

impl Write for Output {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let count = unsafe { (self.write_bytes)(self.context, bytes.as_ptr(), bytes.len()) };
        Ok(count)
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

#[no_mangle]
pub unsafe extern "C" fn gea_linux_write_png(
    pixels: *const u8,
    width: u32,
    height: u32,
    stride: usize,
    fast: bool,
    context: *mut c_void,
    write_bytes: WriteBytes,
) -> bool {
    catch_unwind(|| {
        let Some(row_bytes) = (width as usize).checked_mul(4) else {
            return false;
        };
        let Some(length) = (height as usize)
            .checked_sub(1)
            .and_then(|rows| stride.checked_mul(rows))
            .and_then(|bytes| bytes.checked_add(row_bytes))
        else {
            return false;
        };
        if pixels.is_null() || row_bytes == 0 || stride < row_bytes || length > isize::MAX as usize
        {
            return false;
        }

        // The caller keeps the canvas and callback context alive until encoding ends.
        let pixels = unsafe { slice::from_raw_parts(pixels, length) };
        let mut encoder = png::Encoder::new(
            Output {
                context,
                write_bytes,
            },
            width,
            height,
        );
        encoder.set_depth(png::BitDepth::Eight);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_source_srgb(png::SrgbRenderingIntent::Perceptual);
        encoder.set_compression(if fast {
            png::Compression::Fast
        } else {
            png::Compression::Balanced
        });

        let result = (|| -> Result<(), png::EncodingError> {
            let mut writer = encoder.write_header()?;
            if stride == row_bytes {
                writer.write_image_data(pixels)?;
            } else {
                let mut stream = writer.stream_writer()?;
                for row in pixels.chunks(stride) {
                    stream.write_all(&row[..row_bytes])?;
                }
                stream.finish()?;
            }
            writer.finish()
        })();
        result.is_ok()
    })
    .unwrap_or(false)
}
