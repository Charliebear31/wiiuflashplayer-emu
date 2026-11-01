use crate::avm2::activation::Activation;
use crate::avm2::bytearray::ByteArrayStorage;
use crate::avm2::error::{Error, make_error_3675, make_error_3679};
use crate::avm2::object::{Object, TextureObject};
use ruffle_render::atf::ATFTexture;
use ruffle_render::atf::ATFTextureData;
use std::io::Cursor;

use std::io::Read;
use std::io::Seek;

pub fn do_compressed_upload<'gc>(
    activation: &mut Activation<'_, 'gc>,
    texture: TextureObject<'gc>,
    data: Object<'gc>,
    byte_array_offset: usize,
    is_cube: bool,
) -> Result<(), Error<'gc>> {
    let bytes = data.as_bytearray().unwrap();
    let raw_atf = ByteArrayStorage::bytes(&bytes)
        .get(byte_array_offset..)
        .ok_or("ATF offset past the end of the data")?;

    let atf_texture = ATFTexture::from_bytes(raw_atf)
        .map_err(|e| format!("Failed to parse ATF texture: {e:?}"))?;

    if atf_texture.width != texture.handle().width()
        || atf_texture.height != texture.handle().height()
    {
        return Err(make_error_3679(activation));
    }

    if is_cube != atf_texture.cubemap {
        return Err(make_error_3675(activation));
    }

    for (face, mips) in atf_texture.face_mip_data.iter().enumerate() {
        for (level, mip) in mips.iter().enumerate() {
            let level = level as u32;
            // (Past the smallest size there's nothing to sample.)
            if level > 13 {
                break;
            }
            let width = (atf_texture.width >> level).max(1);
            let height = (atf_texture.height >> level).max(1);
            let pixels = decode_mip(mip, width, height).map_err(|e| {
                format!("ATF format {:?}, level {level}: {e}", atf_texture.format)
            })?;
            texture
                .context3d()
                .copy_pixels_to_texture(pixels, texture.handle(), face as u32, level);
        }
    }

    Ok(())
}

/// One mip level, as the bytes the texture's format uploads: RGBA for plain JPEG-XR, DXT blocks otherwise.
fn decode_mip(mip: &ATFTextureData, width: u32, height: u32) -> Result<Vec<u8>, String> {
    // Endpoint images have one row of pixels for each half block, so at least two rows.
    let image_width = (width / 4).max(1);
    let image_height = (height / 2).max(2);
    let blocks = (image_width * image_height / 2) as usize;

    match mip {
        ATFTextureData::JpegXR(bytes) => {
            Ok(jpegxr_to_tiff(width, height, &mut Cursor::new(bytes))?
                .to_rgba8()
                .pixels()
                .flat_map(|p| p.0)
                .collect())
        }
        ATFTextureData::Compressed {
            dxt1_lookup_compressed,
            jpegxr_bgr,
        } => {
            let lookup = decompress_lzma(dxt1_lookup_compressed)?;
            let bgr = jpegxr_to_raw_pixels(image_width, image_height, &mut Cursor::new(jpegxr_bgr))?;
            if lookup.len() < blocks * 4 || bgr.len() < blocks * 4 {
                return Err("DXT1 parts are too short".to_string());
            }

            let mut dxt = Vec::with_capacity(blocks * 8);
            for i in 0..blocks {
                // The two values of a block are in the upper and lower halves of the image.
                dxt.extend(&bgr[i * 2..(i + 1) * 2]);
                dxt.extend(&bgr[(blocks + i) * 2..(blocks + i + 1) * 2]);
                dxt.extend(&lookup[i * 4..(i + 1) * 4]);
            }
            Ok(dxt)
        }
        ATFTextureData::CompressedAlpha {
            dxt1_alpha_compressed,
            jpegxr_alpha,
            dxt5_rgb_compressed,
            jpegxr_bgr,
        } => {
            // 'COMPRESSED_ALPHA' images are encoded in a very strange way. The LZMA-compressed DXT1/DXT5
            // sections just hold the DXT lookup table block. The associated values for each block
            // are stored in JPEG-XR images. See
            // https://github.com/adobe/dds2atf/blob/cbc479be2e77daa273306161af571f8255aec78d/pvr2atfcore.cpp#L884
            let alpha_lookup = decompress_lzma(dxt1_alpha_compressed)?;
            let rgb_lookup = decompress_lzma(dxt5_rgb_compressed)?;
            let alpha =
                jpegxr_to_raw_pixels(image_width, image_height, &mut Cursor::new(jpegxr_alpha))?;
            let bgr = jpegxr_to_raw_pixels(image_width, image_height, &mut Cursor::new(jpegxr_bgr))?;

            if alpha_lookup.len() < blocks * 6
                || rgb_lookup.len() < blocks * 4
                || alpha.len() < blocks * 2
                || bgr.len() < blocks * 4
            {
                return Err("DXT5 parts are too short".to_string());
            }

            let mut dxt = Vec::with_capacity(blocks * 16);
            for i in 0..blocks {
                dxt.push(alpha[i]);
                dxt.push(alpha[blocks + i]);
                dxt.extend(&alpha_lookup[i * 6..(i + 1) * 6]);
                dxt.extend(&bgr[i * 2..(i + 1) * 2]);
                dxt.extend(&bgr[(blocks + i) * 2..(blocks + i + 1) * 2]);
                dxt.extend(&rgb_lookup[i * 4..(i + 1) * 4]);
            }
            Ok(dxt)
        }
        ATFTextureData::CompressedRawAlpha { dxt5, .. } => {
            if dxt5.is_empty() {
                return Err("no DXT5 data (only PVRTC or ETC)".to_string());
            }
            Ok(dxt5.clone())
        }
        ATFTextureData::CompressedRaw { dxt1 } => {
            if dxt1.is_empty() {
                return Err("no DXT1 data (only PVRTC or ETC)".to_string());
            }
            Ok(dxt1.clone())
        }
        ATFTextureData::Unknown(_) => Err("unknown layout".to_string()),
    }
}

/// See https://github.com/adobe/dds2atf/issues/5: ATF's LZMA doesn't store the uncompressed length, so
/// "unknown" goes where it should be.
fn decompress_lzma(compressed: &[u8]) -> Result<Vec<u8>, String> {
    if compressed.len() < 5 {
        return Err("no DXT data (only PVRTC or ETC)".to_string());
    }
    let mut with_length = Vec::with_capacity(compressed.len() + 8);
    with_length.extend(&compressed[..5]);
    with_length.extend(u64::MAX.to_le_bytes());
    with_length.extend(&compressed[5..]);

    let mut out = Vec::with_capacity(compressed.len());
    lzma_rs::lzma_decompress(&mut with_length.as_slice(), &mut out)
        .map_err(|e| format!("LZMA: {e:?}"))?;
    Ok(out)
}

fn open_jpegxr<R: Read + Seek>(
    atf_width: u32,
    atf_height: u32,
    bytes: R,
) -> Result<(jpegxr::ImageDecode<R>, usize, usize), String> {
    let mut decoder =
        jpegxr::ImageDecode::with_reader(bytes).map_err(|e| format!("JPEG-XR: {e:?}"))?;
    let (jpeg_width, jpeg_height) = decoder.get_size().map_err(|e| format!("JPEG-XR: {e:?}"))?;
    if jpeg_width as u32 != atf_width || jpeg_height as u32 != atf_height {
        return Err(format!(
            "JPEG-XR is {jpeg_width}x{jpeg_height}, expected {atf_width}x{atf_height}"
        ));
    }
    let pixel_format = decoder
        .get_pixel_format()
        .map_err(|e| format!("JPEG-XR: {e:?}"))?;
    let info = jpegxr::PixelInfo::from_format(pixel_format);
    let stride = jpeg_width as usize * info.bits_per_pixel() / 8;
    let size = stride * jpeg_height as usize;
    Ok((decoder, stride, size))
}

fn jpegxr_to_raw_pixels<R: Read + Seek>(
    atf_width: u32,
    atf_height: u32,
    bytes: R,
) -> Result<Vec<u8>, String> {
    let (mut decoder, stride, size) = open_jpegxr(atf_width, atf_height, bytes)?;
    let mut output = vec![0; size];
    decoder
        .copy_all(&mut output, stride)
        .map_err(|e| format!("JPEG-XR: {e:?}"))?;
    Ok(output)
}

fn jpegxr_to_tiff<R: Read + Seek>(
    atf_width: u32,
    atf_height: u32,
    bytes: R,
) -> Result<image::DynamicImage, String> {
    let (mut decoder, _, size) = open_jpegxr(atf_width, atf_height, bytes)?;

    // We convert the result to a TIFF - this makes the jpegxr library handle
    // all of the weird JPEG-XR alpha formats for us. We can then use the normal
    // `image` crate to decode the TIFF to an rgba array.
    let mut bmp_buffer = vec![0; size];
    decoder
        .convert_to_tiff(&mut Cursor::new(&mut bmp_buffer))
        .map_err(|e| format!("JPEG-XR: {e:?}"))?;

    image::ImageReader::with_format(Cursor::new(bmp_buffer), image::ImageFormat::Tiff)
        .decode()
        .map_err(|e| format!("JPEG-XR picture: {e:?}"))
}
