//! Flash save data (SharedObjects) as files: one `.sol` per key under SAVE_DIR (WASI path, so on the
//! Wii U it is sd:/wiiu/apps/flashplayeremu/saves/).

use crate::host;
use ruffle_core::backend::storage::StorageBackend;
use std::path::PathBuf;

const SAVE_DIR: &str = "/wiiu/apps/flashplayeremu/saves";

pub struct DiskStorage;

impl DiskStorage {
    pub fn new() -> Self {
        let _ = std::fs::create_dir_all(SAVE_DIR);
        Self
    }

    /// Keys look like "localhost/wiiu/apps/flashplayeremu/swf/game.swf/name"; keep letters, digits and
    /// a few safe characters, so every key is one flat file name.
    fn path(name: &str) -> PathBuf {
        let mut file: String = name
            .chars()
            .map(|c| if c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_') { c } else { '#' })
            .collect();
        if file.len() > 200 {
            // Long keys: keep the end (the save's own name) and make it unique with a hash.
            let hash = name.bytes().fold(5381u32, |h, b| h.wrapping_mul(33) ^ b as u32);
            file = format!("{:08x}{}", hash, &file[file.len() - 180..]);
        }
        PathBuf::from(SAVE_DIR).join(file + ".sol")
    }
}

impl StorageBackend for DiskStorage {
    fn get(&self, name: &str) -> Option<Vec<u8>> {
        std::fs::read(Self::path(name)).ok()
    }

    fn put(&mut self, name: &str, value: &[u8]) -> bool {
        let ok = std::fs::write(Self::path(name), value).is_ok();
        if !ok {
            host::log(1, &format!("could not save {name}"));
        }
        ok
    }

    fn remove_key(&mut self, name: &str) {
        let _ = std::fs::remove_file(Self::path(name));
    }
}
