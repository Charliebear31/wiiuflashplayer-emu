//! Local-files-only navigator. Paths are WASI paths: "/" is the host's root folder (sd: on Wii U).

use crate::host;
use async_channel::{Receiver, Sender};
use indexmap::IndexMap;
use ruffle_core::backend::navigator::{
    ErrorResponse, NavigationMethod, NavigatorBackend, NullSpawner, OwnedFuture, Request,
    SuccessResponse, fetch_path, resolve_url_with_relative_base_path,
};
use ruffle_core::loader::Error;
use ruffle_core::socket::{ConnectionState, SocketAction, SocketHandle};
use std::path::PathBuf;
use std::time::Duration;
use url::{ParseError, Url};

pub struct HostNavigator {
    spawner: NullSpawner,
    /// The movie's folder: relative URLs ("levels.xml") load from next to the SWF, like desktop Ruffle.
    base_path: PathBuf,
}

impl HostNavigator {
    pub fn new(spawner: NullSpawner, movie_url: &str) -> Self {
        Self { spawner, base_path: movie_folder(movie_url) }
    }
}

/// "file:///wiiu/apps/x/swf/en/game.swf" -> "/wiiu/apps/x/swf/en" (percent-decoded); "/" if it isn't a file URL.
fn movie_folder(movie_url: &str) -> PathBuf {
    let Some(path) = movie_url.strip_prefix("file://") else {
        return PathBuf::from("/");
    };
    let path = path.split(['?', '#']).next().unwrap_or("");
    let Some(slash) = path.rfind('/') else {
        return PathBuf::from("/");
    };
    let bytes = path[..slash.max(1)].as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        let hex = |b: u8| (b as char).to_digit(16);
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let (Some(h), Some(l)) = (hex(bytes[i + 1]), hex(bytes[i + 2])) {
                out.push((h * 16 + l) as u8);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    PathBuf::from(String::from_utf8_lossy(&out).into_owned())
}

impl NavigatorBackend for HostNavigator {
    fn navigate_to_url(
        &self,
        url: &str,
        _target: &str,
        _vars_method: Option<(NavigationMethod, IndexMap<String, String>)>,
    ) {
        host::log(2, &format!("navigate_to_url ignored: {url}"));
    }

    fn fetch(&self, request: Request) -> OwnedFuture<Box<dyn SuccessResponse>, ErrorResponse> {
        fetch_path(self, "HostNavigator", request.url(), None)
    }

    fn resolve_url(&self, url: &str) -> Result<Url, ParseError> {
        resolve_url_with_relative_base_path(self, self.base_path.clone(), url)
    }

    fn spawn_future(&mut self, future: OwnedFuture<(), Error>) {
        self.spawner.spawn_local(future);
    }

    fn pre_process_url(&self, url: Url) -> Url {
        url
    }

    fn connect_socket(
        &mut self,
        _host: String,
        _port: u16,
        _timeout: Duration,
        handle: SocketHandle,
        _receiver: Receiver<Vec<u8>>,
        sender: Sender<SocketAction>,
    ) {
        let _ = sender.try_send(SocketAction::Connect(handle, ConnectionState::Failed));
    }
}
