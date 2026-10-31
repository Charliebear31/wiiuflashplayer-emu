use ruffle_core::backend::audio::{
    AudioBackend, AudioMixer, AudioMixerProxy, DecodeError, RegisterError, SoundHandle,
    SoundInstanceHandle, SoundStreamInfo, SoundTransform, swf,
};
use ruffle_core::impl_audio_mixer_backend;

pub const SAMPLE_RATE: u32 = 48000;

/// The mixer lives in the player; the host pulls samples with `rw_audio_mix` through the proxy
/// (the wasm instance is single threaded, so the host's audio thread reads from a ring it fills).
pub struct HostAudioBackend {
    mixer: AudioMixer,
    paused: bool,
}

impl HostAudioBackend {
    pub fn new() -> (Self, AudioMixerProxy) {
        let mixer = AudioMixer::new(2, SAMPLE_RATE);
        let proxy = mixer.proxy();
        (
            Self {
                mixer,
                paused: false,
            },
            proxy,
        )
    }
}

impl AudioBackend for HostAudioBackend {
    impl_audio_mixer_backend!(mixer);

    fn play(&mut self) {
        self.paused = false;
    }

    fn pause(&mut self) {
        self.paused = true;
    }
}
