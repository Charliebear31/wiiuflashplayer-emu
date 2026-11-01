use std::io::Read;

use byteorder::{BigEndian, ReadBytesExt};
use num_derive::FromPrimitive;
use num_traits::FromPrimitive;

pub struct ATFTexture {
    pub width: u32,
    pub height: u32,
    pub cubemap: bool,
    pub format: ATFFormat,
    pub mip_count: u8,
    // A nested array of `[0..num_faces][0..mip_count]`, where each
    // entry is the texture data for that mip level and face.
    pub face_mip_data: Vec<Vec<ATFTextureData>>,
}

#[derive(Clone)]
pub enum ATFTextureData {
    Unknown(Vec<u8>),
    JpegXR(Vec<u8>),
    CompressedAlpha {
        jpegxr_alpha: Vec<u8>,
        dxt1_alpha_compressed: Vec<u8>,
        jpegxr_bgr: Vec<u8>,
        dxt5_rgb_compressed: Vec<u8>,
    },
    CompressedRawAlpha {
        dxt5: Vec<u8>,
        pvrtc: Vec<u8>,
        etc1: Vec<u8>,
        etc2: Vec<u8>,
    },
    CompressedRaw {
        dxt1: Vec<u8>,
    },
    Compressed {
        dxt1_lookup_compressed: Vec<u8>,
        jpegxr_bgr: Vec<u8>,
    },
}

#[derive(FromPrimitive, Debug)]
pub enum ATFFormat {
    RGB888 = 0,
    RGBA8888 = 1,
    Compressed = 2,
    RawCompressed = 3,
    CompressedAlpha = 4,
    RawCompressedAlpha = 5,
    CompressedLossy = 0xc,
    CompressedLossyAlpha = 0xd,
}

impl ATFTexture {
    pub fn from_bytes(mut bytes: &[u8]) -> Result<Self, Box<dyn std::error::Error>> {
        // Based on https://github.com/openfl/openfl/blob/develop/src/openfl/display3D/_internal/ATFReader.hx
        let bytes = &mut bytes;

        let mut string_bytes = [0; 3];
        bytes.read_exact(&mut string_bytes)?;

        if &string_bytes != b"ATF" {
            return Err(format!("Invalid ATF signature {string_bytes:?}").into());
        }

        let version;
        let _length;
        let mut actual_mip_count = None;

        if bytes[3] == 0xFF {
            actual_mip_count = Some(bytes[2] >> 1);
            if actual_mip_count == Some(0) {
                actual_mip_count = None;
            }
            version = bytes[4];
            *bytes = &bytes[5..];
            _length = bytes.read_u32::<byteorder::LittleEndian>()?;
        } else {
            version = 0;
            _length = read_uint24(bytes)?;
        }

        let tdata = bytes.read_u8()?;
        let cubemap = (tdata >> 7) != 0;

        let format = ATFFormat::from_u8(tdata & 0x7f).ok_or_else(|| {
            format!(
                "Invalid ATF format {format} (version {version})",
                format = tdata & 0x7f,
                version = version
            )
        })?;
        let width = 1 << bytes.read_u8()?;
        let height = 1 << bytes.read_u8()?;

        let mip_count = bytes.read_u8()?;
        let mip_count = actual_mip_count.unwrap_or(mip_count);
        let num_faces = if cubemap { 6 } else { 1 };

        let mut face_mip_data = vec![vec![]; num_faces];

        #[expect(clippy::needless_range_loop)]
        for face in 0..num_faces {
            for _ in 0..mip_count {
                let read_len = |bytes: &mut &[u8]| -> Result<u32, Box<dyn std::error::Error>> {
                    if version == 0 {
                        Ok(read_uint24(bytes)?)
                    } else {
                        Ok(bytes.read_u32::<BigEndian>()?)
                    }
                };

                match format {
                    ATFFormat::RGB888 | ATFFormat::RGBA8888 => {
                        let len = read_len(bytes)?;
                        let mut data = vec![0; len as usize];
                        bytes.read_exact(&mut data)?;
                        face_mip_data[face].push(ATFTextureData::JpegXR(data));
                    }
                    ATFFormat::CompressedAlpha => {
                        let dxt1_alpha_len = read_len(bytes)? as usize;
                        let mut dxt1_alpha_compressed = vec![0; dxt1_alpha_len];
                        bytes.read_exact(&mut dxt1_alpha_compressed)?;

                        let alpha_jpegxr_len = read_len(bytes)? as usize;
                        let mut alpha_jpegxr = vec![0; alpha_jpegxr_len];
                        bytes.read_exact(&mut alpha_jpegxr)?;

                        let dxt5_rgb = read_len(bytes)? as usize;
                        let mut dxt5_rgb_compressed = vec![0; dxt5_rgb];
                        bytes.read_exact(&mut dxt5_rgb_compressed)?;

                        let bgr_jpegxr_len = read_len(bytes)? as usize;
                        let mut bgr_jpegxr = vec![0; bgr_jpegxr_len];
                        bytes.read_exact(&mut bgr_jpegxr)?;

                        for _ in 0..12 {
                            let len = read_len(bytes)? as usize;
                            *bytes = bytes.get(len..).ok_or("ATF record past the end")?;
                        }

                        face_mip_data[face].push(ATFTextureData::CompressedAlpha {
                            jpegxr_alpha: alpha_jpegxr,
                            dxt1_alpha_compressed,
                            jpegxr_bgr: bgr_jpegxr,
                            dxt5_rgb_compressed,
                        });
                    }
                    ATFFormat::RawCompressedAlpha => {
                        let dxt5_len = read_len(bytes)? as usize;
                        let mut dxt5 = vec![0; dxt5_len];
                        bytes.read_exact(&mut dxt5)?;

                        let pvrtc_len = read_len(bytes)? as usize;
                        let mut pvrtc = vec![0; pvrtc_len];
                        bytes.read_exact(&mut pvrtc)?;

                        let etc1_len = read_len(bytes)? as usize;
                        let mut etc1 = vec![0; etc1_len];
                        bytes.read_exact(&mut etc1)?;

                        let etc2_len = read_len(bytes)? as usize;
                        let mut etc2 = vec![0; etc2_len];
                        bytes.read_exact(&mut etc2)?;

                        face_mip_data[face].push(ATFTextureData::CompressedRawAlpha {
                            dxt5,
                            pvrtc,
                            etc1,
                            etc2,
                        });
                    }
                    ATFFormat::RawCompressed => {
                        // DXT1, PVRTC, ETC1, ETC2: only the first is used here.
                        let dxt1_len = read_len(bytes)? as usize;
                        let mut dxt1 = vec![0; dxt1_len];
                        bytes.read_exact(&mut dxt1)?;
                        for _ in 0..3 {
                            let len = read_len(bytes)? as usize;
                            *bytes = bytes.get(len..).ok_or("ATF record past the end")?;
                        }
                        face_mip_data[face].push(ATFTextureData::CompressedRaw { dxt1 });
                    }
                    _ => {
                        // Records in order: the DXT ones (see adobe/dds2atf), then PVRTC, ETC1 and ETC2, which aren't used here.
                        // The lossy formats have one record more, and no public description: their DXT records
                        // are taken to come first in the same way, and the upload fails cleanly if they don't fit.
                        let num_records = match format {
                            ATFFormat::Compressed => 11,
                            ATFFormat::CompressedLossy => 12,
                            ATFFormat::CompressedLossyAlpha => 17,
                            ATFFormat::RGB888
                            | ATFFormat::RGBA8888
                            | ATFFormat::RawCompressed
                            | ATFFormat::RawCompressedAlpha
                            | ATFFormat::CompressedAlpha => unreachable!(),
                        };

                        let mut records = Vec::with_capacity(num_records);
                        for _ in 0..num_records {
                            let len = read_len(bytes)? as usize;
                            let mut record = vec![0; len];
                            bytes.read_exact(&mut record)?;
                            records.push(record);
                        }
                        let mut records = records.into_iter();
                        let mut next = || records.next().unwrap_or_default();
                        face_mip_data[face].push(if matches!(format, ATFFormat::CompressedLossyAlpha) {
                            let dxt1_alpha_compressed = next();
                            let jpegxr_alpha = next();
                            let dxt5_rgb_compressed = next();
                            let jpegxr_bgr = next();
                            ATFTextureData::CompressedAlpha {
                                jpegxr_alpha,
                                dxt1_alpha_compressed,
                                jpegxr_bgr,
                                dxt5_rgb_compressed,
                            }
                        } else {
                            let dxt1_lookup_compressed = next();
                            let jpegxr_bgr = next();
                            ATFTextureData::Compressed { dxt1_lookup_compressed, jpegxr_bgr }
                        });
                    }
                }
            }
        }
        Ok(ATFTexture {
            width,
            height,
            cubemap,
            format,
            mip_count,
            face_mip_data,
        })
    }
}

fn read_uint24<R: Read>(data: &mut R) -> Result<u32, Box<dyn std::error::Error>> {
    let ch1 = data.read_u8()? as u32;
    let ch2 = data.read_u8()? as u32;
    let ch3 = data.read_u8()? as u32;
    Ok(ch3 | (ch2 << 8) | (ch1 << 16))
}
