// Without the `jpegxr` feature only ATF files that carry raw DXT5 blocks ("RawCompressedAlpha") can be
// uploaded; the JPEG-XR based formats need the decoder. (Changed for the Wii U port: upstream's stub
// rejected every compressed upload.)
use crate::avm2::Activation;
use crate::avm2::Error;
use crate::avm2::Object;
use crate::avm2::bytearray::ByteArrayStorage;
use crate::avm2::error::{make_error_3675, make_error_3679};
use crate::avm2::object::TextureObject;
use ruffle_render::atf::{ATFTexture, ATFTextureData};

pub fn do_compressed_upload<'gc>(
    activation: &mut Activation<'_, 'gc>,
    texture: TextureObject<'gc>,
    data: Object<'gc>,
    byte_array_offset: usize,
    is_cube: bool,
) -> Result<(), Error<'gc>> {
    let bytes = data.as_bytearray().unwrap();
    let raw_atf = &ByteArrayStorage::bytes(&bytes)[byte_array_offset..];
    let atf_texture = ATFTexture::from_bytes(raw_atf).map_err(|e| format!("Failed to parse ATF texture: {e:?}"))?;

    if atf_texture.width != texture.handle().width() || atf_texture.height != texture.handle().height() {
        return Err(make_error_3679(activation));
    }
    if is_cube != atf_texture.cubemap {
        return Err(make_error_3675(activation));
    }

    match &atf_texture.face_mip_data[0][0] {
        ATFTextureData::CompressedRawAlpha { dxt5, .. } if !dxt5.is_empty() => {
            texture.context3d().copy_pixels_to_texture(dxt5.clone(), texture.handle(), 0);
            Ok(())
        }
        _ => Err(format!("ATF format {:?} needs the JPEG-XR decoder, which is not compiled in", atf_texture.format).into()),
    }
}
