use crate::avm2::Avm2;
use crate::avm2::Avm2StrRepresentable;
use crate::avm2::activation::Activation;
use crate::avm2::error::{Error, Error2004Type, make_error_2004, make_error_2008, make_error_2175};
use crate::avm2::function::FunctionArgs;
use crate::avm2::globals::flash::display::display_object::initialize_for_allocator;
use crate::avm2::object::{
    ContentElementObject, ElementData, ElementFormatObject, Object, TextBlockObject, VectorObject,
};
use crate::avm2::parameters::ParametersExt;
use crate::avm2::value::Value;
use crate::display_object::{DisplayObject, EDIT_TEXT_GUTTER, EditText, LineGraphic, TDisplayObject, TextLine};
use crate::fte::{
    BreakOpportunityValue, TextBaselineValue, TextLineCreationResultValue, TextLineValidity,
    TextRotationValue, TypographicCaseValue,
};
use crate::html::{FormatSpans, TextFormat, TextSpan};
use crate::string::{AvmString, WStr, WString};
use crate::{avm2_stub_getter, avm2_stub_setter};
use ruffle_macros::istr;

pub use crate::avm2::object::text_block_allocator;

pub fn get_apply_non_linear_font_scaling<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    Ok(this.apply_non_linear_font_scaling().into())
}

pub fn set_apply_non_linear_font_scaling<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();

    avm2_stub_setter!(
        activation,
        "flash.text.engine.TextBlock",
        "applyNonLinearFontScaling"
    );

    this.set_apply_non_linear_font_scaling(args.get_bool(0));

    Ok(Value::Undefined)
}

pub fn get_baseline_font_description<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    Ok(this
        .baseline_font_description()
        .map(Value::from)
        .unwrap_or(Value::Null))
}

pub fn set_baseline_font_description<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();

    avm2_stub_setter!(
        activation,
        "flash.text.engine.TextBlock",
        "baselineFontDescription"
    );

    let value = args
        .try_get_object(0)
        .map(|v| v.as_font_description_object().unwrap());

    this.set_baseline_font_description(value, activation.gc());

    Ok(Value::Undefined)
}

pub fn get_baseline_font_size<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    Ok(this.baseline_font_size().into())
}

pub fn set_baseline_font_size<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    let value = args.get_f64(0);
    if value < 0.0 {
        return Err(make_error_2004(activation, Error2004Type::ArgumentError));
    }

    avm2_stub_setter!(
        activation,
        "flash.text.engine.TextBlock",
        "baselineFontSize"
    );

    this.set_baseline_font_size(value);

    Ok(Value::Undefined)
}

pub fn get_baseline_zero<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    Ok(this.baseline_zero().as_avm2_str(activation).into())
}

pub fn set_baseline_zero<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();

    let value = args.get_string_non_null(activation, 0, "baselineZero")?;
    let value = TextBaselineValue::from_avm2_str(&value)
        .filter(|v| !matches!(v, TextBaselineValue::UseDominantBaseline))
        .ok_or_else(|| make_error_2008(activation, "baselineZero"))?;

    avm2_stub_setter!(activation, "flash.text.engine.TextBlock", "baselineZero");

    this.set_baseline_zero(value);

    Ok(Value::Undefined)
}

pub fn get_bidi_level<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    Ok(this.bidi_level().into())
}

pub fn set_bidi_level<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    let value = args.get_i32(0);
    if value < 0 {
        return Err(make_error_2004(activation, Error2004Type::ArgumentError));
    }

    avm2_stub_setter!(activation, "flash.text.engine.TextBlock", "bidiLevel");

    this.set_bidi_level(value);

    Ok(Value::Undefined)
}

pub fn get_line_rotation<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    Ok(this.line_rotation().as_avm2_str(activation).into())
}

pub fn set_line_rotation<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();

    let value = args.get_string_non_null(activation, 0, "lineRotation")?;
    let value = TextRotationValue::from_avm2_str(&value)
        .filter(|v| !matches!(v, TextRotationValue::Auto))
        .ok_or_else(|| make_error_2008(activation, "lineRotation"))?;

    avm2_stub_setter!(activation, "flash.text.engine.TextBlock", "lineRotation");

    this.set_line_rotation(value);

    Ok(Value::Undefined)
}

pub fn get_tab_stops<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    Ok(this
        .tab_stops()
        .map(|v| Value::from(VectorObject::from_vector(v.storage().clone(), activation)))
        .unwrap_or(Value::Null))
}

pub fn set_tab_stops<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    let tab_stops = args
        .try_get_object(0)
        .and_then(|v| v.as_vector_object())
        .map(|v| VectorObject::from_vector(v.storage().clone(), activation));

    avm2_stub_setter!(activation, "flash.text.engine.TextBlock", "tabStops");

    this.set_tab_stops(tab_stops, activation.gc());

    Ok(Value::Undefined)
}

pub fn get_text_justifier<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    Ok(this
        .text_justifier()
        .map(Value::from)
        .unwrap_or(Value::Null))
}

pub fn set_text_justifier<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();

    let justifier = args.get_object(activation, 0, "textJustifier")?;

    avm2_stub_setter!(activation, "flash.text.engine.TextBlock", "textJustifier");

    this.set_text_justifier(justifier, activation.gc());

    Ok(Value::Undefined)
}

pub fn get_content<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    Ok(this.content().map(Value::from).unwrap_or(Value::Null))
}

pub fn set_content<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();

    let content = args
        .try_get_object(0)
        .map(|v| v.as_content_element_object().unwrap());

    // Flash forbids setting a `TextBlock`'s content to a user-defined subclass
    // of `ContentElement`
    let is_invalid = content.is_some_and(|c| {
        let data = c.element_data();
        matches!(&*data, ElementData::Invalid)
    });
    if is_invalid {
        return Err(make_error_2004(activation, Error2004Type::ArgumentError));
    }

    if let Some(old) = this.content() {
        old.set_owner_block(None, activation.gc());
    }
    if let Some(content) = content {
        content.set_owner_block(Some(this), activation.gc());
    }
    this.set_content(content, activation.gc());

    Ok(Value::Undefined)
}

pub fn get_text_line_creation_result<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();
    Ok(this
        .text_line_creation_result()
        .map(|v| v.as_avm2_str(activation).into())
        .unwrap_or(Value::Null))
}

pub fn get_first_invalid_line<'gc>(
    activation: &mut Activation<'_, 'gc>,
    _this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    avm2_stub_getter!(
        activation,
        "flash.text.engine.TextBlock",
        "firstInvalidLine"
    );

    Ok(Value::Null)
}

pub fn get_first_line<'gc>(
    _activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();

    let line = this
        .first_line()
        .map(|l| l.object2().expect("Already created"))
        .map(Value::from);

    Ok(line.unwrap_or(Value::Null))
}

pub fn get_last_line<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    _args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();

    avm2_stub_getter!(activation, "flash.text.engine.TextBlock", "lastLine");

    let line = this
        .lines()
        .last()
        .map(|l| l.object2().expect("Already created"))
        .map(Value::from);

    Ok(line.unwrap_or(Value::Null))
}

pub fn release_lines<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this.as_object().unwrap().as_text_block_object().unwrap();

    let line_1 = args
        .get_object(activation, 0, "firstLine")?
        .as_display_object()
        .and_then(|o| o.as_text_line())
        .expect("Guaranteed by AS signature");

    let line_2 = args
        .get_object(activation, 1, "lastLine")?
        .as_display_object()
        .and_then(|o| o.as_text_line())
        .expect("Guaranteed by AS signature");

    // Flash has some unexpected behavior for certain edge cases of this method:

    // 1. Callers can swap the order of parameters to this method without ever
    //    affecting the functionality of the method.
    // 2. If two text lines are in the same text block and are siblings to each
    //    other, this method can still fail with an error if they can't be
    //    reached by iterating forward on the linked list of lines of this text
    //    block. It's possible to enter this scenario by rebreaking a line in
    //    the middle of a block.
    // 3. This method will always throw an error if either line is from a
    //    different text block.

    // The simplest way I could reproduce this behavior was with the following
    // logic:

    // 1. Start iterating over the linked list of lines, starting at
    //    `this.first_line()`.
    // 2. When reaching a line that is either `line_1` or `line_2` for the first
    //    time, record its position in the iterator.
    // 3. When reaching a line that is either `line_1` or `line_2` for the
    //    second time, record its position in the iterator and stop iterating.
    // 4. If either iteration reached the end of the list, return an error, as
    //    that means that one of the lines was not present in the block.
    // 5. Use the two positions to release the lines between `line_1` and
    //    `line_2` in the linked list of lines.

    let matches_either_line =
        |l| DisplayObject::ptr_eq(l, line_1) || DisplayObject::ptr_eq(l, line_2);

    let Some(first_position) = this.lines().position(matches_either_line) else {
        return Err(make_error_2004(activation, Error2004Type::ArgumentError));
    };

    if DisplayObject::ptr_eq(line_1, line_2) {
        // Special case: the two lines are the same. NOTE: We only do this after
        // we ensure that the line is actually reachable from `this.first_line`.
        line_1.release(activation.gc());

        return Ok(Value::Undefined);
    }

    let lines_count = this
        .lines()
        .skip(first_position + 1)
        .position(matches_either_line)
        .map(|p| p + 2);

    let Some(lines_count) = lines_count else {
        return Err(make_error_2004(activation, Error2004Type::ArgumentError));
    };

    let lines_to_remove = this
        .lines()
        .skip(first_position)
        .take(lines_count)
        .collect::<Vec<_>>();

    // `collect` the iterator, as we're modifying the doubly-linked list at the
    // same time as we iterate over it

    for line in lines_to_remove {
        line.release(activation.gc());
    }

    Ok(Value::Undefined)
}

pub fn do_create_text_line<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this_obj = this.as_object().unwrap();
    let block = this_obj.as_text_block_object().unwrap();

    let line_to_use = args
        .try_get_object(0)
        .and_then(|o| o.as_display_object())
        .and_then(|o| o.as_text_line());

    let previous_text_line = args
        .try_get_object(1)
        .and_then(|o| o.as_display_object())
        .and_then(|o| o.as_text_line());

    let width = args.get_f64(2);
    let fit_something = args.get_bool(4);

    let content = block.content().expect("Guaranteed by AS checks");

    let previous_position = if let Some(previous_text_line) = previous_text_line {
        previous_text_line.end_index() as usize
    } else {
        0
    };

    let block_content = match get_spans_from_content(content) {
        Ok(block_content) => block_content,
        Err(HandleContentError::NullElementFormat) => {
            // For some reason, FP handles this error as it handles an uncaught
            // exception, and returns `null` from this method.
            let error = make_error_2175(activation);

            Avm2::uncaught_error(activation, None, error, "Error creating TextLine");

            return Ok(Value::Null);
        }
    };
    let spans = &block_content.spans;
    let text = spans.text();

    if previous_position > text.len() {
        // This can happen when the content is changed after creating a TextLine.
        block.set_text_line_creation_result(Some(TextLineCreationResultValue::Complete));
        return Ok(Value::Null);
    }

    let paragraph_end = next_line_break(text, previous_position);

    if text.is_empty() || paragraph_end == text.len() && previous_position == paragraph_end {
        // No more text.
        block.set_text_line_creation_result(Some(TextLineCreationResultValue::Complete));
        return Ok(Value::Null);
    }

    let line_index = if let Some(previous_text_line) = previous_text_line {
        previous_text_line.line_index() + 1
    } else {
        0
    };

    // `recreateTextLine` measures with its line's own `EditText`; the line itself isn't touched until the text fits.
    let measure = match line_to_use {
        Some(line) => line.fallback(),
        None => new_fallback(activation, width),
    };
    let tab_stops = block_tab_stops(activation, block)?;
    let Some((next_position, result)) = break_line(
        activation,
        measure,
        &block_content,
        &tab_stops,
        previous_position,
        paragraph_end,
        width,
        fit_something,
    ) else {
        block.set_text_line_creation_result(Some(TextLineCreationResultValue::InsufficientWidth));
        return Ok(Value::Null);
    };

    // `do_create_text_line` is called from both `TextBlock.recreateTextLine`
    // and `TextBlock.createTextLine`. The former passes this method an existing
    // `TextLine` to reuse, while the latter expects this method to create a new
    // `TextLine`.
    // Whatever followed the new line's place no longer fits the text before it. Those lines stay
    // linked to each other, and a line from this block being recreated keeps the line that followed it.
    let stale_start = match previous_text_line {
        Some(previous_line) => previous_line.next_line(),
        None => block.first_line(),
    };
    let mut next_line = None;
    let text_line = if let Some(line) = line_to_use {
        // `TextLine.recreateTextLine` is the caller: completely reset the
        // properties of the passed line and use it.
        let line_block = line.text_block();
        if line_block.is_some_and(|b| Object::ptr_eq(b, block)) {
            next_line = line.next_line();
        } else if line_block.is_some_and(|b| b.first_line().is_some()) {
            line.release(activation.gc());
        }
        line.reset_properties(activation.context);
        line
    } else {
        // `TextLine.createTextLine` is the caller: create a new `TextLine`.
        create_text_line(activation, measure)
    };

    // (The line that used to follow is cut off from the lines before it.)
    if let Some(start) = stale_start
        && !DisplayObject::ptr_eq(start, text_line)
    {
        start.set_previous_line(None, activation.gc());
    }
    for start in [stale_start, next_line].into_iter().flatten() {
        if !DisplayObject::ptr_eq(start, text_line) {
            start.set_validity(TextLineValidity::Invalid, activation.gc());
        }
        for line in start.next_lines() {
            if !DisplayObject::ptr_eq(line, text_line) {
                line.set_validity(TextLineValidity::Invalid, activation.gc());
            }
        }
    }
    if let Some(next_line) = next_line {
        text_line.set_next_line(Some(next_line), activation.gc());
        next_line.set_previous_line(Some(text_line), activation.gc());
    }

    let text_line_instance = text_line.object2().expect("Already created the object2");
    let fallback = text_line.fallback();

    let order = visual_order(&text[previous_position..next_position], block.bidi_level() % 2 == 1);
    fill_fallback(activation, fallback, &block_content, &tab_stops, previous_position, next_position, false, order.as_deref());
    let mut logical_to_visual = Vec::new();
    if let Some(order) = &order {
        logical_to_visual = vec![0; order.len()];
        for (visual, &logical) in order.iter().enumerate() {
            logical_to_visual[logical] = visual;
        }
    }
    text_line.set_visual(logical_to_visual.clone(), activation.gc());
    justify_line(activation, block, fallback, width, next_position == text.len(), next_position == paragraph_end)?;

    let measured_text = fallback.measure_text(activation.context);
    fallback.set_height(activation.context, measured_text.1.to_pixels());

    text_line.set_text_block(Some(block), activation.gc());
    text_line.set_specified_width(width);
    text_line.set_raw_text_length((next_position - previous_position) as u32);
    text_line.set_begin_index(previous_position as u32);
    text_line.set_end_index(next_position as u32);
    text_line.set_line_index(line_index);
    text_line.set_line_rotation(block.line_rotation());

    let first_format = block_content.format_at(previous_position);
    text_line.set_baseline_shift(first_format.map_or(0.0, |f| f.baseline_shift()));
    text_line.set_text_alpha(first_format.map_or(1.0, |f| f.alpha()));
    text_line.set_graphics(
        block_content
            .graphics
            .iter()
            .filter(|g| (previous_position..next_position).contains(&g.position))
            .map(|g| LineGraphic {
                position: {
                    let p = g.position - previous_position;
                    logical_to_visual.get(p).copied().unwrap_or(p)
                },
                ..*g
            })
            .collect(),
        activation.gc(),
    );

    if let Some(previous_line) = previous_text_line {
        text_line.set_previous_line(Some(previous_line), activation.gc());
        previous_line.set_next_line(Some(text_line), activation.gc());
    } else {
        // If there's no previous line, then this is the first line.
        block.set_first_line(Some(text_line), activation.gc());
    }

    block.set_text_line_creation_result(Some(result));

    // Elements with an eventMirror: the line passes its mouse events on (TextLine.as).
    let _ = Value::from(text_line_instance).call_public_property(
        AvmString::new_utf8(activation.gc(), "initMirrors"),
        FunctionArgs::from_slice(&[]),
        activation,
    );

    Ok(text_line_instance.into())
}

/// Spreads a line to its width by widening its spaces, as the block's justifier asks.
fn justify_line<'gc>(
    activation: &mut Activation<'_, 'gc>,
    block: TextBlockObject<'gc>,
    fallback: EditText<'gc>,
    width: f64,
    ends_block: bool,
    ends_paragraph: bool,
) -> Result<(), Error<'gc>> {
    let Some(justifier) = block.text_justifier() else {
        return Ok(());
    };
    let mode = Value::from(justifier).get_public_property(AvmString::new_utf8(activation.gc(), "lineJustification"), activation)?;
    let mode = mode.coerce_to_string(activation)?;
    let justify = if &mode == b"allIncludingLastLine" {
        true
    } else if &mode == b"allButLast" {
        !ends_block
    } else if &mode == b"allButMandatoryBreak" {
        !ends_paragraph
    } else {
        false
    };
    if !justify || width >= 1000000.0 {
        return Ok(());
    }

    let text = fallback.text();
    let mut end = text.len();
    while end > 0 && matches!(text.get(end - 1), Some(0x20 | 0x0A | 0x0D | 0x2028 | 0x2029)) {
        end -= 1;
    }
    let spaces: Vec<usize> = (0..end).filter(|i| text.get(*i) == Some(0x20)).collect();
    if end == 0 || spaces.is_empty() {
        return Ok(());
    }
    let used = fallback
        .char_bounds(end - 1)
        .map_or(width, |b| (b.x_max - EDIT_TEXT_GUTTER).to_pixels());
    let extra = (width - used) / spaces.len() as f64;
    if extra <= 0.0 {
        return Ok(());
    }
    for space in spaces {
        let mut format = fallback.text_format(space, space + 1);
        format.letter_spacing = Some(format.letter_spacing.unwrap_or(0.0) + extra);
        fallback.set_text_format(space, space + 1, format, activation.context);
    }
    Ok(())
}

fn new_fallback<'gc>(activation: &mut Activation<'_, 'gc>, width: f64) -> EditText<'gc> {
    // TODO should we use the caller's movie instead? Does it matter?
    let movie = *activation.context.root_swf;
    EditText::new_fte(activation.context, movie, 0.0, 0.0, width, 15.0)
}

fn create_text_line<'gc>(activation: &mut Activation<'_, 'gc>, fallback: EditText<'gc>) -> TextLine<'gc> {
    let class = activation.avm2().classes().textline;
    let movie = *activation.context.root_swf;

    let text_line = TextLine::new(activation.context, movie, fallback);
    initialize_for_allocator(activation.context, text_line.into(), class);

    text_line
}

fn block_tab_stops<'gc>(
    activation: &mut Activation<'_, 'gc>,
    block: TextBlockObject<'gc>,
) -> Result<Option<Vec<f64>>, Error<'gc>> {
    let Some(tab_stops) = block.tab_stops() else {
        return Ok(None);
    };
    let values: Vec<Value<'gc>> = tab_stops.storage().iter().collect();
    let mut positions = Vec::with_capacity(values.len());
    for value in values {
        if value.as_object().is_some() {
            let position = value.get_public_property(istr!("position"), activation)?;
            positions.push(position.coerce_to_number(activation)?);
        }
    }
    Ok(Some(positions))
}

/// Puts `from..to` of the block's text into an `EditText`, with its formats.
fn fill_fallback<'gc>(
    activation: &mut Activation<'_, 'gc>,
    fallback: EditText<'gc>,
    content: &BlockContent<'gc>,
    tab_stops: &Option<Vec<f64>>,
    from: usize,
    to: usize,
    word_wrap: bool,
    order: Option<&[usize]>,
) {
    // TODO: Handle device font/non-device font properly
    fallback.set_is_device_font(activation.context, false);
    fallback.set_word_wrap(word_wrap, activation.context);
    // Joined Arabic forms and fi/fl ligatures, one unit per character still (shaping.rs).
    let shaped = super::shaping::shape(activation, content.spans.text(), |p| content.format_at(p), from, to);
    if let Some(order) = order {
        // Shown in visual order (right-to-left runs reversed), each unit keeping its own format.
        let mut visual = WString::with_capacity(order.len(), shaped.is_wide());
        for &logical in order {
            visual.push(shaped.get(logical).unwrap_or(0x20));
        }
        fallback.set_text(&visual, activation.context);
        let span_of = |logical: usize| {
            content
                .spans
                .iter_spans()
                .position(|(s, e, _, _)| from + logical >= s && from + logical < e)
        };
        let mut v = 0;
        while v < order.len() {
            let id = span_of(order[v]);
            let mut end = v + 1;
            while end < order.len() && span_of(order[end]) == id {
                end += 1;
            }
            if let Some((_, _, _, span)) = id.and_then(|i| content.spans.iter_spans().nth(i)) {
                let mut format = span.get_text_format();
                if tab_stops.is_some() {
                    format.tab_stops = tab_stops.clone();
                }
                fallback.set_text_format(v, end, format.clone(), activation.context);
                if v == 0 {
                    fallback.set_new_text_format(format);
                }
            }
            v = end;
        }
        return;
    }
    fallback.set_text(&shaped, activation.context);
    for (start, end, _, span) in content.spans.iter_spans() {
        let (start, end) = (start.max(from), end.min(to));
        if start < end {
            let mut format = span.get_text_format();
            if tab_stops.is_some() {
                format.tab_stops = tab_stops.clone();
            }
            fallback.set_text_format(start - from, end - from, format.clone(), activation.context);
            if start == from {
                fallback.set_new_text_format(format);
            }
        }
    }
}

fn is_right_to_left(unit: u16) -> bool {
    matches!(unit, 0x0590..=0x08FF | 0xFB1D..=0xFDFF | 0xFE70..=0xFEFF)
}

/// The visual order of a line's units (visual -> logical), or `None` when it reads left to right as it is.
/// Runs of right-to-left script are reversed; in a right-to-left block the runs themselves go right to left,
/// with digits and left-to-right words kept in reading order. Spaces and punctuation take the direction of
/// their neighbours when those agree, else the block's. (The simple part of Unicode's algorithm: no embeddings.)
fn visual_order(text: &WStr, rtl_block: bool) -> Option<Vec<usize>> {
    let n = text.len();
    if n == 0 || (!rtl_block && !text.iter().any(is_right_to_left)) {
        return None;
    }
    // 0: left to right, 1: right to left, 2: undecided
    let neutral = |c: u16| matches!(c, 0x20 | 0x09 | 0x21 | 0x22 | 0x27..=0x2F | 0x3A..=0x3F | 0x5B..=0x5D | 0x7B..=0x7D | 0xA0);
    let mut class: Vec<u8> = text
        .iter()
        .map(|c| if is_right_to_left(c) { 1 } else if neutral(c) { 2 } else { 0 })
        .collect();
    let base = rtl_block as u8;
    let mut i = 0;
    while i < n {
        if class[i] != 2 {
            i += 1;
            continue;
        }
        let start = i;
        while i < n && class[i] == 2 {
            i += 1;
        }
        let left = if start == 0 { base } else { class[start - 1] };
        let right = if i == n { base } else { class[i] };
        let decided = if left == right { left } else { base };
        for c in &mut class[start..i] {
            *c = decided;
        }
    }
    let mut runs: Vec<(usize, usize, u8)> = Vec::new();
    let mut start = 0;
    for i in 1..=n {
        if i == n || class[i] != class[start] {
            runs.push((start, i, class[start]));
            start = i;
        }
    }
    if rtl_block {
        runs.reverse();
    }
    let mut order = Vec::with_capacity(n);
    for (s, e, c) in runs {
        if c == 1 {
            order.extend((s..e).rev());
        } else {
            order.extend(s..e);
        }
    }
    if order.iter().enumerate().all(|(v, &l)| v == l) {
        return None;
    }
    Some(order)
}

/// Where a line starting at `start` ends, or `None` if nothing fits and nothing has to.
#[expect(clippy::too_many_arguments)]
fn break_line<'gc>(
    activation: &mut Activation<'_, 'gc>,
    fallback: EditText<'gc>,
    content: &BlockContent<'gc>,
    tab_stops: &Option<Vec<f64>>,
    start: usize,
    paragraph_end: usize,
    width: f64,
    fit_something: bool,
) -> Option<(usize, TextLineCreationResultValue)> {
    use TextLineCreationResultValue::{Emergency, Success};

    let text = content.spans.text();
    let opportunity = content
        .format_at(start)
        .map_or(BreakOpportunityValue::Auto, |f| f.break_opportunity());

    if matches!(opportunity, BreakOpportunityValue::All) {
        // A break at every opportunity: one word to a line.
        let end = text[start..paragraph_end]
            .iter()
            .position(|c| c == 0x20)
            .map_or(paragraph_end, |space| start + space + 1);
        return Some((end, Success));
    }

    // (`EditText` keeps a gutter on each side, which a text line doesn't have.)
    fallback.set_width(activation.context, width + 4.0);
    let word_wrap = matches!(opportunity, BreakOpportunityValue::Auto);
    fill_fallback(activation, fallback, content, tab_stops, start, paragraph_end, word_wrap, None);

    let wrapped_length = fallback
        .layout()
        .lines()
        .first()
        .map_or(paragraph_end - start, |line| line.end().min(paragraph_end - start))
        .max(1);

    // Spaces at the end of a line can hang past its width.
    let mut visible = wrapped_length;
    while visible > 1 && matches!(text.get(start + visible - 1), Some(0x20 | 0x0A | 0x0D | 0x2028 | 0x2029)) {
        visible -= 1;
    }
    let right_of = |index: usize| {
        fallback
            .char_bounds(index)
            .map_or(0.0, |b| (b.x_max - EDIT_TEXT_GUTTER).to_pixels())
    };
    let is_break = |c: Option<u16>| matches!(c, None | Some(0x20 | 0x09 | 0x0A | 0x0D | 0x2D | 0x2028 | 0x2029));
    let split_word = start + wrapped_length < paragraph_end
        && !is_break(text.get(start + wrapped_length - 1))
        && !is_break(text.get(start + wrapped_length));
    let fits = !split_word && right_of(visible - 1) <= width;

    if fits && !matches!(opportunity, BreakOpportunityValue::Any) {
        let mut end = start + wrapped_length;
        // The line break that ends a paragraph belongs to its last line.
        if end < paragraph_end
            && text[end..paragraph_end].iter().all(|c| matches!(c, 0x0A | 0x0D | 0x2028 | 0x2029))
        {
            end = paragraph_end;
        }
        return Some((end, Success));
    }

    // As many characters as fit.
    let mut count = 0;
    while count < visible && right_of(count) <= width {
        count += 1;
    }
    if matches!(opportunity, BreakOpportunityValue::Any) && count > 0 {
        return Some((start + if fits { wrapped_length } else { count }, Success));
    }
    if !fit_something {
        return None;
    }
    Some((start + count.max(1), Emergency))
}

enum HandleContentError {
    NullElementFormat,
}

/// A block's content, flattened.
struct BlockContent<'gc> {
    spans: FormatSpans,
    graphics: Vec<LineGraphic<'gc>>,
    /// Where each element's format starts, in order.
    formats: Vec<(usize, ElementFormatObject<'gc>)>,
}

impl<'gc> BlockContent<'gc> {
    fn format_at(&self, position: usize) -> Option<ElementFormatObject<'gc>> {
        self.formats
            .iter()
            .rev()
            .find(|(start, _)| *start <= position)
            .map(|(_, format)| *format)
    }
}

fn get_spans_from_content<'gc>(
    content: ContentElementObject<'gc>,
) -> Result<BlockContent<'gc>, HandleContentError> {
    let mut all_text = WString::new();
    let mut all_spans = Vec::new();
    let mut graphics = Vec::new();
    let mut formats = Vec::new();

    handle_content_element(content, &mut all_text, &mut all_spans, &mut graphics, &mut formats)?;

    let mut spans = FormatSpans::from_str_and_spans(&all_text, &all_spans);
    spans.normalize();
    Ok(BlockContent { spans, graphics, formats })
}

/// Changes the case of what's shown, only where the length stays the same: indices into the text must stay right.
fn push_with_case(out: &mut WString, text: &WStr, case: TypographicCaseValue) {
    for unit in text.iter() {
        let changed = char::from_u32(unit as u32).and_then(|c| {
            let mapped = match case {
                TypographicCaseValue::Uppercase
                | TypographicCaseValue::Caps
                | TypographicCaseValue::SmallCaps
                | TypographicCaseValue::CapsAndSmallCaps => c.to_uppercase().next(),
                TypographicCaseValue::Lowercase => c.to_lowercase().next(),
                _ => Some(c),
            };
            mapped.filter(|m| m.len_utf16() == 1).map(|m| m as u16)
        });
        out.push(changed.unwrap_or(unit));
    }
}

fn handle_content_element<'gc>(
    content: ContentElementObject<'gc>,
    all_text: &mut WString,
    all_spans: &mut Vec<TextSpan>,
    graphics: &mut Vec<LineGraphic<'gc>>,
    formats: &mut Vec<(usize, ElementFormatObject<'gc>)>,
) -> Result<(), HandleContentError> {
    let data = content.element_data();
    let format = content.element_format();

    match &*data {
        ElementData::Text { text } => {
            // If `text` is `None`, FP just completely ignores the element. It
            // doesn't even check its `elementFormat`.
            if let Some(text) = text {
                let format = format.ok_or(HandleContentError::NullElementFormat)?;
                formats.push((all_text.len(), format));
                push_with_case(all_text, text, format.typographic_case());

                let text_format = format.as_text_format();

                let span = TextSpan::with_length_and_format(text.len(), &text_format);
                all_spans.push(span);
            }
        }
        ElementData::Group { elements } => {
            // TODO: The docs say GroupElement's format has some effects?
            for element in elements {
                handle_content_element(*element, all_text, all_spans, graphics, formats)?;
            }
        }
        ElementData::Graphic { graphic, width, height } => {
            // Takes up its width in the line: a space in tiny type, with the rest as letter spacing.
            graphics.push(LineGraphic {
                position: all_text.len(),
                object: *graphic,
                height: *height,
            });
            if let Some(format) = format {
                formats.push((all_text.len(), format));
            }
            all_text.push(0xA0);
            let text_format = TextFormat {
                size: Some(1.0),
                letter_spacing: Some(*width),
                ..format.map(|f| f.as_text_format()).unwrap_or_default()
            };
            all_spans.push(TextSpan::with_length_and_format(1, &text_format));
        }
        ElementData::Invalid => {
            unreachable!(
                "TextBlock and GroupElement prevent holding user subclasses of ContentElement"
            )
        }
    }

    Ok(())
}

fn next_line_break(text: &WStr, start: usize) -> usize {
    let remaining_text = &text[start..];
    let len = remaining_text
        .iter()
        .position(|ch| matches!(ch, 0x0A | 0x0D | 0x2028 | 0x2029))
        // Include the separator.
        .map(|pos| {
            if remaining_text.get(pos) == Some(0x0D) && remaining_text.get(pos + 1) == Some(0x0A) {
                pos + 2
            } else {
                pos + 1
            }
        });

    if let Some(len) = len {
        start + len
    } else {
        text.len()
    }
}
