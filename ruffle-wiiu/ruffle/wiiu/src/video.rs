//! Video: the engine's software decoders, and H.264 through the console's own decoder.

use crate::host;
use crate::render::{Texture, as_texture};
use ruffle_render::backend::RenderBackend;
use ruffle_render::bitmap::{BitmapHandle, BitmapInfo};
use ruffle_video::VideoStreamHandle;
use ruffle_video::backend::VideoBackend;
use ruffle_video::error::Error;
use ruffle_video::frame::{EncodedFrame, FrameDependency};
use ruffle_video_software::backend::SoftwareVideoBackend;
use slotmap::SlotMap;
use std::sync::Arc;
use swf::{VideoCodec, VideoDeblocking};

struct H264Stream {
    decoder: u32,
    bitmap: BitmapHandle,
    size: (u32, u32),
    /// How many bytes hold each unit's length (set by the configuration data).
    length_size: usize,
    /// The stream's parameter sets, to send again if the decoder has to be opened again.
    sets: Vec<u8>,
    /// What to send ahead of the next frame.
    parameters: Vec<u8>,
    last_used: u64,
}

impl Drop for H264Stream {
    fn drop(&mut self) {
        unsafe { host::host_h264_close(self.decoder) }
    }
}

enum Stream {
    Software(VideoStreamHandle),
    H264(H264Stream),
}

/// The engine never says a stream has ended, so a video that replays would keep adding decoders of about 40 MB each.
const MAX_LIVE_DECODERS: usize = 2;

pub struct HostVideoBackend {
    software: SoftwareVideoBackend,
    streams: SlotMap<VideoStreamHandle, Stream>,
    clock: u64,
}

impl HostVideoBackend {
    pub fn new() -> Self {
        Self { software: SoftwareVideoBackend::new(), streams: SlotMap::with_key(), clock: 0 }
    }

    fn live_decoders(&mut self) -> impl Iterator<Item = &mut H264Stream> {
        self.streams.values_mut().filter_map(|s| match s {
            Stream::H264(h) if h.decoder != 0 => Some(h),
            _ => None,
        })
    }

    fn close_oldest_decoder(&mut self) -> bool {
        let Some(s) = self.live_decoders().min_by_key(|s| s.last_used) else { return false };
        unsafe { host::host_h264_close(s.decoder) }
        s.decoder = 0;
        s.parameters = s.sets.clone();
        true
    }

    fn open_decoder(&mut self, w: u32, h: u32) -> u32 {
        while self.live_decoders().count() >= MAX_LIVE_DECODERS {
            self.close_oldest_decoder();
        }
        loop {
            let decoder = unsafe { host::host_h264_open(w, h) };
            if decoder != 0 || !self.close_oldest_decoder() {
                return decoder;
            }
        }
    }
}

impl VideoBackend for HostVideoBackend {
    fn register_video_stream(
        &mut self,
        num_frames: u32,
        size: (u16, u16),
        codec: VideoCodec,
        filter: VideoDeblocking,
    ) -> Result<VideoStreamHandle, Error> {
        let stream = if codec == VideoCodec::H264 {
            let (w, h) = (size.0.max(1) as u32, size.1.max(1) as u32);
            let decoder = self.open_decoder(w, h);
            if decoder == 0 {
                return Err(Error::UnsupportedCodec(codec));
            }
            self.clock += 1;
            Stream::H264(H264Stream {
                decoder,
                bitmap: BitmapHandle(Arc::new(Texture::new_empty(w, h))),
                size: (w, h),
                length_size: 0,
                sets: Vec::new(),
                parameters: Vec::new(),
                last_used: self.clock,
            })
        } else {
            Stream::Software(self.software.register_video_stream(num_frames, size, codec, filter)?)
        };
        Ok(self.streams.insert(stream))
    }

    fn configure_video_stream_decoder(
        &mut self,
        stream: VideoStreamHandle,
        configuration_data: &[u8],
    ) -> Result<(), Error> {
        match self.streams.get_mut(stream).ok_or(Error::VideoStreamIsNotRegistered)? {
            Stream::Software(inner) => self.software.configure_video_stream_decoder(*inner, configuration_data),
            Stream::H264(s) => {
                // AVC configuration: lengths' size at 4, then the sets, each with a 2-byte length.
                let c = configuration_data;
                if c.len() < 7 {
                    return Ok(());
                }
                s.length_size = (c[4] & 3) as usize + 1;
                s.sets.clear();
                s.parameters.clear();
                let mut at = 5;
                for _ in 0..2 {
                    let Some(&count) = c.get(at) else { break };
                    let count = if at == 5 { count & 0x1f } else { count };
                    at += 1;
                    for _ in 0..count {
                        if at + 2 > c.len() {
                            return Ok(());
                        }
                        let len = c[at] as usize * 256 + c[at + 1] as usize;
                        at += 2;
                        if at + len > c.len() {
                            return Ok(());
                        }
                        s.sets.extend_from_slice(&[0, 0, 0, 1]);
                        s.sets.extend_from_slice(&c[at..at + len]);
                        at += len;
                    }
                }
                s.parameters = s.sets.clone();
                Ok(())
            }
        }
    }

    fn preload_video_stream_frame(
        &mut self,
        stream: VideoStreamHandle,
        encoded_frame: EncodedFrame<'_>,
    ) -> Result<FrameDependency, Error> {
        match self.streams.get(stream).ok_or(Error::VideoStreamIsNotRegistered)? {
            Stream::Software(inner) => self.software.preload_video_stream_frame(*inner, encoded_frame),
            Stream::H264(s) => {
                let nal = encoded_frame.data.get(s.length_size).map_or(0, |b| b & 0x1f);
                Ok(if nal == 5 { FrameDependency::None } else { FrameDependency::Past })
            }
        }
    }

    fn decode_video_stream_frame(
        &mut self,
        stream: VideoStreamHandle,
        encoded_frame: EncodedFrame<'_>,
        renderer: &mut dyn RenderBackend,
    ) -> Result<BitmapInfo, Error> {
        // A stream whose decoder was given up for another one gets one back (a clean picture needs the next key frame).
        if let Some(Stream::H264(s)) = self.streams.get(stream)
            && s.decoder == 0
        {
            let (w, h) = s.size;
            let decoder = self.open_decoder(w, h);
            if let Some(Stream::H264(s)) = self.streams.get_mut(stream) {
                s.decoder = decoder;
            }
        }
        self.clock += 1;
        let clock = self.clock;
        match self.streams.get_mut(stream).ok_or(Error::VideoStreamIsNotRegistered)? {
            Stream::Software(inner) => self.software.decode_video_stream_frame(*inner, encoded_frame, renderer),
            Stream::H264(s) => {
                s.last_used = clock;
                // Length-prefixed units to start-code-prefixed ones, as the decoder wants.
                let data = encoded_frame.data;
                let mut annex = std::mem::take(&mut s.parameters);
                annex.reserve(data.len() + 16);
                let mut at = 0;
                while s.length_size > 0 && at + s.length_size <= data.len() {
                    let len = data[at..at + s.length_size].iter().fold(0usize, |v, b| v << 8 | *b as usize);
                    at += s.length_size;
                    let end = (at + len).min(data.len());
                    annex.extend_from_slice(&[0, 0, 0, 1]);
                    annex.extend_from_slice(&data[at..end]);
                    at = end;
                }
                let tex = as_texture(&s.bitmap).id;
                // (No new picture yet, the decoder holding it back to reorder: the last one stays up.)
                unsafe { host::host_h264_decode(s.decoder, tex, annex.as_ptr(), annex.len() as u32) };
                Ok(BitmapInfo { handle: s.bitmap.clone(), width: s.size.0, height: s.size.1 })
            }
        }
    }
}
