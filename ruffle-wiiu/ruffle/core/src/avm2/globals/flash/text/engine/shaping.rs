//! The little shaping the text engine's lines get before an `EditText` draws them: Arabic letters take their
//! joined forms and `fi` / `fl` become the font's ligature glyphs. Everything stays one unit per character
//! (a ligature's second letter becomes a zero-width space), so atoms, mirror regions and char positions keep
//! working on the original text. Only forms the font actually has are used.

use crate::avm2::activation::Activation;
use crate::avm2::globals::flash::text::engine::element_format::resolve_font;
use crate::avm2::object::ElementFormatObject;
use crate::font::Font;
use crate::fte::LigatureLevelValue;
use crate::string::{Units, WStr, WString};
use gc_arena::Gc;

const ZWSP: u16 = 0x200B;

fn same<'gc>(a: ElementFormatObject<'gc>, b: ElementFormatObject<'gc>) -> bool {
    Gc::ptr_eq(a.0, b.0)
}

fn same_opt<'gc>(a: Option<ElementFormatObject<'gc>>, b: Option<ElementFormatObject<'gc>>) -> bool {
    match (a, b) {
        (Some(a), Some(b)) => same(a, b),
        (None, None) => true,
        _ => false,
    }
}

/// (letter, isolated, final, initial, medial); 0 for a form the letter doesn't have (it joins only to the right).
const ARABIC_FORMS: &[(u16, [u16; 4])] = &[
    (0x0621, [0xFE80, 0, 0, 0]),
    (0x0622, [0xFE81, 0xFE82, 0, 0]),
    (0x0623, [0xFE83, 0xFE84, 0, 0]),
    (0x0624, [0xFE85, 0xFE86, 0, 0]),
    (0x0625, [0xFE87, 0xFE88, 0, 0]),
    (0x0626, [0xFE89, 0xFE8A, 0xFE8B, 0xFE8C]),
    (0x0627, [0xFE8D, 0xFE8E, 0, 0]),
    (0x0628, [0xFE8F, 0xFE90, 0xFE91, 0xFE92]),
    (0x0629, [0xFE93, 0xFE94, 0, 0]),
    (0x062A, [0xFE95, 0xFE96, 0xFE97, 0xFE98]),
    (0x062B, [0xFE99, 0xFE9A, 0xFE9B, 0xFE9C]),
    (0x062C, [0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0]),
    (0x062D, [0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4]),
    (0x062E, [0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8]),
    (0x062F, [0xFEA9, 0xFEAA, 0, 0]),
    (0x0630, [0xFEAB, 0xFEAC, 0, 0]),
    (0x0631, [0xFEAD, 0xFEAE, 0, 0]),
    (0x0632, [0xFEAF, 0xFEB0, 0, 0]),
    (0x0633, [0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4]),
    (0x0634, [0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8]),
    (0x0635, [0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC]),
    (0x0636, [0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0]),
    (0x0637, [0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4]),
    (0x0638, [0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8]),
    (0x0639, [0xFEC9, 0xFECA, 0xFECB, 0xFECC]),
    (0x063A, [0xFECD, 0xFECE, 0xFECF, 0xFED0]),
    (0x0641, [0xFED1, 0xFED2, 0xFED3, 0xFED4]),
    (0x0642, [0xFED5, 0xFED6, 0xFED7, 0xFED8]),
    (0x0643, [0xFED9, 0xFEDA, 0xFEDB, 0xFEDC]),
    (0x0644, [0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0]),
    (0x0645, [0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4]),
    (0x0646, [0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8]),
    (0x0647, [0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC]),
    (0x0648, [0xFEED, 0xFEEE, 0, 0]),
    (0x0649, [0xFEEF, 0xFEF0, 0, 0]),
    (0x064A, [0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4]),
    (0x0671, [0xFB50, 0xFB51, 0, 0]),
    (0x0679, [0xFB66, 0xFB67, 0xFB68, 0xFB69]),
    (0x067E, [0xFB56, 0xFB57, 0xFB58, 0xFB59]),
    (0x0686, [0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D]),
    (0x0688, [0xFB88, 0xFB89, 0, 0]),
    (0x0691, [0xFB8C, 0xFB8D, 0, 0]),
    (0x0698, [0xFB8A, 0xFB8B, 0, 0]),
    (0x06A4, [0xFB6A, 0xFB6B, 0xFB6C, 0xFB6D]),
    (0x06A9, [0xFB8E, 0xFB8F, 0xFB90, 0xFB91]),
    (0x06AF, [0xFB92, 0xFB93, 0xFB94, 0xFB95]),
    (0x06BE, [0xFBAA, 0xFBAB, 0xFBAC, 0xFBAD]),
    (0x06C1, [0xFBA6, 0xFBA7, 0xFBA8, 0xFBA9]),
    (0x06CC, [0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF]),
];

/// Lam followed by an alef: (alef, isolated, final).
const LAM_ALEF: &[(u16, u16, u16)] =
    &[(0x0622, 0xFEF5, 0xFEF6), (0x0623, 0xFEF7, 0xFEF8), (0x0625, 0xFEF9, 0xFEFA), (0x0627, 0xFEFB, 0xFEFC)];

const TATWEEL: u16 = 0x0640;

fn forms(unit: u16) -> Option<&'static [u16; 4]> {
    ARABIC_FORMS.iter().find(|(c, _)| *c == unit).map(|(_, f)| f)
}

/// Marks and joiners that sit between letters without breaking the joining.
fn transparent(unit: u16) -> bool {
    matches!(unit, 0x0610..=0x061A | 0x064B..=0x065F | 0x0670 | 0x06D6..=0x06ED | 0x200D)
}

/// Does the letter join to the one after it (on its left)?
fn joins_next(unit: u16) -> bool {
    unit == TATWEEL || forms(unit).is_some_and(|f| f[2] != 0)
}

/// Does the letter join to the one before it (on its right)?
fn joins_prev(unit: u16) -> bool {
    unit == TATWEEL || forms(unit).is_some()
}

fn neighbour(text: &WStr, mut i: usize, back: bool) -> Option<u16> {
    loop {
        if back {
            if i == 0 {
                return None;
            }
            i -= 1;
        } else {
            i += 1;
            if i >= text.len() {
                return None;
            }
        }
        let u = text.get(i)?;
        if !transparent(u) {
            return Some(u);
        }
    }
}

/// Fonts looked up once per format, for the glyph checks.
struct Fonts<'gc> {
    seen: Vec<(ElementFormatObject<'gc>, Option<Font<'gc>>)>,
}

impl<'gc> Fonts<'gc> {
    fn get(
        &mut self,
        activation: &mut Activation<'_, 'gc>,
        format: Option<ElementFormatObject<'gc>>,
    ) -> Option<Font<'gc>> {
        let format = format?;
        if let Some((_, font)) = self.seen.iter().find(|(f, _)| same(*f, format)) {
            return *font;
        }
        let font = resolve_font(activation, format);
        self.seen.push((format, font));
        font
    }
}

fn has(font: Option<Font<'_>>, unit: u16) -> bool {
    font.is_some_and(|f| char::from_u32(unit as u32).is_some_and(|c| f.get_glyph_for_char(c).is_some()))
}

/// The units `from..to` of `text` as the line should draw them (same length).
pub fn shape<'gc>(
    activation: &mut Activation<'_, 'gc>,
    text: &WStr,
    format_at: impl Fn(usize) -> Option<ElementFormatObject<'gc>>,
    from: usize,
    to: usize,
) -> WString {
    let mut out: Vec<u16> = text[from..to].iter().collect();
    let mut fonts = Fonts { seen: Vec::new() };
    let mut i = from;
    while i < to {
        let Some(unit) = text.get(i) else { break };
        let format = format_at(i);
        // Arabic: the joined form, or lam-alef as one glyph.
        if let Some(f) = forms(unit) {
            let prev = neighbour(text, i, true).is_some_and(joins_next);
            let next = neighbour(text, i, false).is_some_and(joins_prev);
            let font = fonts.get(activation, format);
            if unit == 0x0644
                && let Some(j) = (i + 1..to).find(|&j| !text.get(j).is_some_and(transparent))
                && let Some(&(_, isolated, final_)) = LAM_ALEF.iter().find(|(a, _, _)| Some(*a) == text.get(j))
                && same_opt(format_at(j), format)
            {
                let lig = if prev { final_ } else { isolated };
                if has(font, lig) && has(font, ZWSP) {
                    out[i - from] = lig;
                    out[j - from] = ZWSP;
                    i = j + 1;
                    continue;
                }
            }
            let form = match (prev && f[1] != 0, next && f[2] != 0) {
                (true, true) => f[3],
                (true, false) => f[1],
                (false, true) => f[2],
                (false, false) => f[0],
            };
            if form != 0 && has(font, form) {
                out[i - from] = form;
            }
            i += 1;
            continue;
        }
        // Latin: fi, fl, ffi, ffl when the format asks for common ligatures (the default) and the font has them.
        if unit == u16::from(b'f')
            && let Some(format) = format
            && !matches!(format.ligature_level(), LigatureLevelValue::None | LigatureLevelValue::Minimum)
        {
            let next = text.get(i + 1).filter(|_| i + 1 < to && format_at(i + 1).is_some_and(|f| same(f, format)));
            let after = text.get(i + 2).filter(|_| i + 2 < to && format_at(i + 2).is_some_and(|f| same(f, format)));
            let font = fonts.get(activation, Some(format));
            let three = match (next, after) {
                (Some(0x66), Some(0x69)) => Some(0xFB03),
                (Some(0x66), Some(0x6C)) => Some(0xFB04),
                _ => None,
            };
            if let Some(lig) = three
                && has(font, lig)
                && has(font, ZWSP)
            {
                out[i - from] = lig;
                out[i + 1 - from] = ZWSP;
                out[i + 2 - from] = ZWSP;
                i += 3;
                continue;
            }
            let two = match next {
                Some(0x69) => Some(0xFB01),
                Some(0x6C) => Some(0xFB02),
                _ => None,
            };
            if let Some(lig) = two
                && has(font, lig)
                && has(font, ZWSP)
            {
                out[i - from] = lig;
                out[i + 1 - from] = ZWSP;
                i += 2;
                continue;
            }
        }
        i += 1;
    }
    WString::from_buf(Units::Wide(out))
}
