//! UI backend: like `NullUiBackend`, but remembers when Ruffle asks for the on-screen keyboard (an
//! editable text field got focus, or was clicked while focused), so the host can show the Wii U's.

use ruffle_core::backend::ui::{
    DialogResultFuture, FileFilter, FontDefinition, FullscreenError, LanguageIdentifier, MouseCursor,
    MultiDialogResultFuture, NullUiBackend, UiBackend,
};
use ruffle_core::font::{FontFileData, FontQuery};
use std::sync::atomic::{AtomicU32, Ordering};
use url::Url;

/// 0 = no keyboard wanted; otherwise a number that changes with every request.
static REQUEST: AtomicU32 = AtomicU32::new(0);
static SEQ: AtomicU32 = AtomicU32::new(0);

pub fn keyboard_request() -> u32 {
    REQUEST.load(Ordering::Relaxed)
}

/// Which bundled font stands in for a system font a game asks for by name. Liberation has the same letter
/// widths as Arial, Times New Roman and Courier New, so text fits where the game's author put it.
fn font_family(lowercase_name: &str) -> Option<&'static str> {
    match lowercase_name {
        "arial" | "helvetica" | "helvetica neue" | "verdana" | "tahoma" | "trebuchet ms" | "geneva"
        | "calibri" | "segoe ui" | "lucida grande" | "lucida sans" | "sans" | "sans-serif" | "comic sans ms" | "arial bold"
        | "arial black" | "arial rounded mt bold" | "impact" => Some("Sans"),
        "times new roman" | "times" | "georgia" | "garamond" | "palatino" | "palatino linotype"
        | "book antiqua" | "cambria" | "serif" => Some("Serif"),
        "courier new" | "courier" | "consolas" | "consolas bold" | "lucida console" | "monaco" | "monospace" => {
            Some("Mono")
        }
        _ => None,
    }
}

pub struct HostUi(NullUiBackend);

impl HostUi {
    pub fn new() -> Self {
        REQUEST.store(0, Ordering::Relaxed);
        Self(NullUiBackend::new())
    }
}

impl UiBackend for HostUi {
    fn mouse_visible(&self) -> bool {
        self.0.mouse_visible()
    }
    fn set_mouse_visible(&mut self, visible: bool) {
        self.0.set_mouse_visible(visible)
    }
    fn set_mouse_cursor(&mut self, cursor: MouseCursor) {
        self.0.set_mouse_cursor(cursor)
    }
    fn clipboard_content(&mut self) -> String {
        self.0.clipboard_content()
    }
    fn set_clipboard_content(&mut self, content: String) {
        self.0.set_clipboard_content(content)
    }
    fn set_fullscreen(&mut self, is_full: bool) -> Result<(), FullscreenError> {
        self.0.set_fullscreen(is_full)
    }
    fn display_root_movie_download_failed_message(&self, invalid_swf: bool, fetched_error: String) {
        self.0.display_root_movie_download_failed_message(invalid_swf, fetched_error)
    }
    fn message(&self, message: &str) {
        self.0.message(message)
    }
    fn open_virtual_keyboard(&self) {
        let seq = SEQ.fetch_add(1, Ordering::Relaxed).wrapping_add(1).max(1);
        REQUEST.store(seq, Ordering::Relaxed);
    }
    fn close_virtual_keyboard(&self) {
        REQUEST.store(0, Ordering::Relaxed);
    }
    fn language(&self) -> LanguageIdentifier {
        self.0.language()
    }
    fn display_unsupported_video(&self, url: Url) {
        self.0.display_unsupported_video(url)
    }
    fn load_device_font(&self, query: &FontQuery, register: &mut dyn FnMut(FontDefinition)) {
        let Some(family) = font_family(&query.lowercase_name) else {
            return;
        };
        let bold = query.is_bold || query.lowercase_name.contains("bold") || query.lowercase_name.contains("black");
        let style = match (bold, query.is_italic) {
            (false, false) => "Regular",
            (true, false) => "Bold",
            (false, true) => "Italic",
            (true, true) => "BoldItalic",
        };
        // Read only when a game asks: 300-400 KB each, and most games embed their fonts.
        let path = format!("/.content/fonts/Liberation{family}-{style}.ttf");
        match std::fs::read(&path) {
            Ok(data) => register(FontDefinition::FontFile {
                name: query.name.clone(),
                is_bold: query.is_bold,
                is_italic: query.is_italic,
                data: FontFileData::new(data),
                index: 0,
            }),
            Err(e) => tracing::warn!("Couldn't read {path}: {e}"),
        }
    }
    fn sort_device_fonts(
        &self,
        query: &FontQuery,
        register: &mut dyn FnMut(FontDefinition),
    ) -> Vec<FontQuery> {
        if font_family(&query.lowercase_name).is_none() {
            return self.0.sort_device_fonts(query, register);
        }
        self.load_device_font(query, register);
        vec![query.clone()]
    }
    fn display_file_open_dialog(&mut self, filters: Vec<FileFilter>) -> Option<DialogResultFuture> {
        self.0.display_file_open_dialog(filters)
    }
    fn display_file_open_dialog_multiple(
        &mut self,
        filters: Vec<FileFilter>,
    ) -> Option<MultiDialogResultFuture> {
        self.0.display_file_open_dialog_multiple(filters)
    }
    fn display_file_save_dialog(
        &mut self,
        file_name: String,
        title: String,
    ) -> Option<DialogResultFuture> {
        self.0.display_file_save_dialog(file_name, title)
    }
    fn close_file_dialog(&mut self) {
        self.0.close_file_dialog()
    }
}
