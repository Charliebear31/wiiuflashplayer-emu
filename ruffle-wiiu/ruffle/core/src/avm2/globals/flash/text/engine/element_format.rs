use crate::avm2::activation::Activation;
use crate::avm2::error::{Error2004Type, make_error_2004, make_error_2008};
use crate::avm2::function::FunctionArgs;
use crate::avm2::parameters::ParametersExt;
use crate::avm2::value::Value;
use crate::avm2::{Avm2StrRepresentable, Error};
use crate::fte::{
    BreakOpportunityValue, DigitCaseValue, DigitWidthValue, KerningValue, LigatureLevelValue,
    TextBaselineValue, TextRotationValue, TypographicCaseValue,
};

pub use crate::avm2::object::element_format_allocator;

pub fn get_alignment_baseline<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.alignment_baseline().as_avm2_str(activation).into())
}

pub fn set_alignment_baseline<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    let s = args.get_string_non_null(activation, 0, "alignmentBaseline")?;
    let Some(value) = TextBaselineValue::from_avm2_str(&s) else {
        return Err(make_error_2008(activation, "alignmentBaseline"));
    };
    this.set_alignment_baseline(value);
    Ok(Value::Undefined)
}

pub fn get_alpha<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.alpha().into())
}

pub fn set_alpha<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    this.set_alpha(args.get_f64(0));
    Ok(Value::Undefined)
}

pub fn get_baseline_shift<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.baseline_shift().into())
}

pub fn set_baseline_shift<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    this.set_baseline_shift(args.get_f64(0));
    Ok(Value::Undefined)
}

pub fn get_break_opportunity<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.break_opportunity().as_avm2_str(activation).into())
}

pub fn set_break_opportunity<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    let s = args.get_string_non_null(activation, 0, "breakOpportunity")?;
    let Some(value) = BreakOpportunityValue::from_avm2_str(&s) else {
        return Err(make_error_2008(activation, "breakOpportunity"));
    };
    this.set_break_opportunity(value);
    Ok(Value::Undefined)
}

pub fn get_color<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.color().to_rgba().into())
}

pub fn set_color<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    this.set_color(swf::Color::from_rgba(args.get_u32(0)));
    Ok(Value::Undefined)
}

pub fn get_digit_case<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.digit_case().as_avm2_str(activation).into())
}

pub fn set_digit_case<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    let s = args.get_string_non_null(activation, 0, "digitCase")?;
    let Some(value) = DigitCaseValue::from_avm2_str(&s) else {
        return Err(make_error_2008(activation, "digitCase"));
    };
    this.set_digit_case(value);
    Ok(Value::Undefined)
}

pub fn get_digit_width<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.digit_width().as_avm2_str(activation).into())
}

pub fn set_digit_width<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    let s = args.get_string_non_null(activation, 0, "digitWidth")?;
    let Some(value) = DigitWidthValue::from_avm2_str(&s) else {
        return Err(make_error_2008(activation, "digitWidth"));
    };
    this.set_digit_width(value);
    Ok(Value::Undefined)
}

pub fn get_dominant_baseline<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.dominant_baseline().as_avm2_str(activation).into())
}

pub fn set_dominant_baseline<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    let s = args.get_string_non_null(activation, 0, "dominantBaseline")?;
    let Some(value) = TextBaselineValue::from_avm2_str(&s) else {
        return Err(make_error_2008(activation, "dominantBaseline"));
    };
    if value == TextBaselineValue::UseDominantBaseline {
        return Err(make_error_2008(activation, "dominantBaseline"));
    }
    this.set_dominant_baseline(value);
    Ok(Value::Undefined)
}

pub fn get_font_description<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.font_description().into())
}

pub fn set_font_description<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();

    let fd = args
        .get_object(activation, 0, "fontDescription")?
        .as_font_description_object()
        .unwrap();

    this.set_font_description(fd, activation.gc());

    Ok(Value::Undefined)
}

pub fn get_font_size<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.font_size().into())
}

pub fn set_font_size<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    let size = args.get_f64(0);
    if size < 0.0 {
        return Err(make_error_2004(activation, Error2004Type::ArgumentError));
    }
    this.set_font_size(size);
    Ok(Value::Undefined)
}

pub fn get_kerning<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.kerning().as_avm2_str(activation).into())
}

pub fn set_kerning<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    let s = args.get_string_non_null(activation, 0, "kerning")?;
    let Some(value) = KerningValue::from_avm2_str(&s) else {
        return Err(make_error_2008(activation, "kerning"));
    };
    this.set_kerning(value);
    Ok(Value::Undefined)
}

pub fn get_ligature_level<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.ligature_level().as_avm2_str(activation).into())
}

pub fn set_ligature_level<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    let s = args.get_string_non_null(activation, 0, "ligatureLevel")?;
    let Some(value) = LigatureLevelValue::from_avm2_str(&s) else {
        return Err(make_error_2008(activation, "ligatureLevel"));
    };
    this.set_ligature_level(value);
    Ok(Value::Undefined)
}

pub fn get_locale<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.locale().into())
}

pub fn set_locale<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    let locale = args.get_string_non_null(activation, 0, "locale")?;
    this.set_locale(locale, activation.gc());
    Ok(Value::Undefined)
}

pub fn get_text_rotation<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.text_rotation().as_avm2_str(activation).into())
}

pub fn set_text_rotation<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    let s = args.get_string_non_null(activation, 0, "textRotation")?;
    let Some(value) = TextRotationValue::from_avm2_str(&s) else {
        return Err(make_error_2008(activation, "textRotation"));
    };
    this.set_text_rotation(value);
    Ok(Value::Undefined)
}

pub fn get_tracking_left<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.tracking_left().into())
}

pub fn set_tracking_left<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    this.set_tracking_left(args.get_f64(0));
    Ok(Value::Undefined)
}

pub fn get_tracking_right<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.tracking_right().into())
}

pub fn set_tracking_right<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    this.set_tracking_right(args.get_f64(0));
    Ok(Value::Undefined)
}

pub fn get_typographic_case<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.typographic_case().as_avm2_str(activation).into())
}

pub fn set_typographic_case<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    let s = args.get_string_non_null(activation, 0, "typographicCase")?;
    let Some(value) = TypographicCaseValue::from_avm2_str(&s) else {
        return Err(make_error_2008(activation, "typographicCase"));
    };
    this.set_typographic_case(value);
    Ok(Value::Undefined)
}

pub fn get_locked<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    Ok(this.locked().into())
}

pub fn set_locked<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_element_format_object()
        .unwrap();
    this.set_locked(args.get_bool(0));
    Ok(Value::Undefined)
}

/// The font's lines and scripts at this format's size, in pixels, for `getFontMetrics`:
/// [em top, em height, strikethrough offset, its thickness, underline offset, its thickness, subscript offset,
/// subscript scale, superscript offset, superscript scale, line gap]. Null without a font.
/// The font a format's description names, as the text engine's lines will draw it: an embedded font of the
/// calling movie, else a default or device font.
pub fn resolve_font<'gc>(
    activation: &mut Activation<'_, 'gc>,
    format: crate::avm2::object::ElementFormatObject<'gc>,
) -> Option<crate::font::Font<'gc>> {
    use crate::font::{DefaultFont, FontType};
    use crate::fte::{FontPostureValue, FontWeightValue};

    let fd = format.font_description();
    let name = fd.font_name().to_string();
    let bold = fd.font_weight() == FontWeightValue::Bold;
    let italic = fd.font_posture() == FontPostureValue::Italic;
    let movie = activation.caller_movie_or_root();
    let embedded = activation
        .context
        .library
        .get_embedded_font_by_name(&name, FontType::EmbeddedCFF, bold, italic, Some(movie))
        .or_else(|| {
            activation.context.library.get_embedded_font_by_name(
                &name,
                FontType::Embedded,
                bold,
                italic,
                Some(movie),
            )
        });
    embedded.or_else(|| {
        let ctx = &mut *activation.context;
        match DefaultFont::from_name(&name) {
            Some(default) => ctx
                .library
                .default_font(default, bold, italic, ctx.ui, ctx.renderer, ctx.gc_context)
                .first()
                .copied(),
            None => ctx
                .library
                .get_or_sort_device_fonts(&name, bold, italic, ctx.ui, ctx.renderer, ctx.gc_context)
                .first()
                .copied(),
        }
    })
}

pub fn font_metrics_values<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    use crate::avm2::array::ArrayStorage;
    use crate::avm2::object::ArrayObject;
    use crate::font::FontLike;

    let this = this.as_object().unwrap().as_element_format_object().unwrap();
    let size = this.font_size();
    let Some(font) = resolve_font(activation, this) else {
        return Ok(Value::Null);
    };

    let m = font.metrics();
    let s = size / m.scale as f64;
    let (ascent, descent, leading) = (m.ascent as f64 * s, m.descent as f64 * s, m.leading as f64 * s);
    let (strike, strike_thick, under, under_thick, sub_off, sub_scale, sup_off, sup_scale) = match m.extra {
        Some(e) => (
            e.strikeout_position as f64 * s,
            e.strikeout_thickness as f64 * s,
            e.underline_position as f64 * s,
            e.underline_thickness as f64 * s,
            e.subscript_offset as f64 * s,
            e.subscript_scale as f64,
            e.superscript_offset as f64 * s,
            e.superscript_scale as f64,
        ),
        None => (ascent * 0.3, size * 0.05, -size * 0.1, size * 0.05, size * 0.14, 0.6, size * 0.48, 0.6),
    };
    // Flash's offsets go down the page: a line above the baseline is negative.
    let values = [
        -ascent,
        ascent + descent,
        -strike,
        strike_thick,
        -under,
        under_thick,
        sub_off,
        sub_scale,
        -sup_off,
        sup_scale,
        leading,
    ];
    let storage = ArrayStorage::from_storage(values.iter().map(|v| Some(Value::from(*v))).collect());
    Ok(ArrayObject::from_storage(activation.context, storage).into())
}