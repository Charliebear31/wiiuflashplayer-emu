use crate::avm2::Error;
use crate::avm2::activation::Activation;
use crate::avm2::object::element_format_object::ElementFormatObject;
use crate::avm2::object::script_object::ScriptObjectData;
use crate::avm2::object::{ClassObject, Object, TObject, TextBlockObject};
use crate::display_object::DisplayObject;
use crate::fte::TextRotationValue;
use crate::string::AvmString;
use core::fmt;
use gc_arena::barrier::unlock;
use gc_arena::lock::{Lock, RefLock};
use gc_arena::{Collect, Gc, GcWeak, Mutation};
use ruffle_common::utils::HasPrefixField;
use ruffle_macros::istr;
use std::cell::{Cell, Ref, RefMut};

pub fn content_element_allocator<'gc>(
    class: ClassObject<'gc>,
    activation: &mut Activation<'_, 'gc>,
) -> Result<Object<'gc>, Error<'gc>> {
    Ok(ContentElementObject(Gc::new(
        activation.gc(),
        ContentElementObjectData {
            base: ScriptObjectData::new(class),
            element_format: Lock::new(None),
            text_rotation: Cell::new(TextRotationValue::Rotate0),
            event_mirror: Lock::new(None),
            element_data: RefLock::new(ElementData::Invalid),
            owner_block: Lock::new(None),
            group: Lock::new(None),
        },
    ))
    .into())
}

#[derive(Clone, Collect, Copy)]
#[collect(no_drop)]
pub struct ContentElementObject<'gc>(pub Gc<'gc, ContentElementObjectData<'gc>>);

#[derive(Clone, Collect, Copy, Debug)]
#[collect(no_drop)]
pub struct ContentElementObjectWeak<'gc>(pub GcWeak<'gc, ContentElementObjectData<'gc>>);

impl fmt::Debug for ContentElementObject<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ContentElementObject")
            .field("ptr", &Gc::as_ptr(self.0))
            .finish()
    }
}

#[derive(Collect, HasPrefixField)]
#[collect(no_drop)]
#[repr(C, align(8))]
pub struct ContentElementObjectData<'gc> {
    base: ScriptObjectData<'gc>,

    /// The format applied to this element.
    element_format: Lock<Option<ElementFormatObject<'gc>>>,

    /// Rotation of the text in this element.
    text_rotation: Cell<TextRotationValue>,

    /// The object which should receive copies of events dispatched to any text
    /// line created from this `ContentElement`. TODO: implement this
    event_mirror: Lock<Option<Object<'gc>>>,

    /// Data held by the class extending `ContentElement` (`TextElement`,
    /// `GraphicElement`, and `GroupElement`). User-defined classes that extend
    /// `ContentElement` do not hold any custom data; attempting to set the
    /// `content` of a `TextBlock` to an instance of such a class throws an
    /// error.
    element_data: RefLock<ElementData<'gc>>,

    /// The block whose `content` this is.
    owner_block: Lock<Option<TextBlockObject<'gc>>>,

    /// The `GroupElement` holding this one.
    group: Lock<Option<ContentElementObject<'gc>>>,
}

#[derive(Collect)]
#[collect(no_drop)]
pub enum ElementData<'gc> {
    /// Such as for the `TextElement` class.
    Text {
        /// Despite the `text` property existing for all classes that extend
        /// `ContentElement`, it is always `null` with no way to change it.
        /// Except in `TextElement`. So we only store the actual text for
        /// `TextElement`.
        text: Option<AvmString<'gc>>,
    },

    /// Such as for the `GroupElement` class.
    Group {
        elements: Vec<ContentElementObject<'gc>>,
    },

    /// Such as for the `GraphicElement` class.
    Graphic {
        graphic: Option<DisplayObject<'gc>>,
        width: f64,
        height: f64,
    },

    /// Such as for a user-defined class extending `ContentElement`.
    Invalid,
}

impl<'gc> ContentElementObject<'gc> {
    pub fn element_format(self) -> Option<ElementFormatObject<'gc>> {
        self.0.element_format.get()
    }

    pub fn set_element_format(self, value: Option<ElementFormatObject<'gc>>, mc: &Mutation<'gc>) {
        unlock!(
            Gc::write(mc, self.0),
            ContentElementObjectData,
            element_format
        )
        .set(value);
    }

    pub fn text_rotation(self) -> TextRotationValue {
        self.0.text_rotation.get()
    }

    pub fn set_text_rotation(self, value: TextRotationValue) {
        self.0.text_rotation.set(value);
    }

    pub fn event_mirror(self) -> Option<Object<'gc>> {
        self.0.event_mirror.get()
    }

    pub fn set_event_mirror(self, value: Option<Object<'gc>>, mc: &Mutation<'gc>) {
        unlock!(
            Gc::write(mc, self.0),
            ContentElementObjectData,
            event_mirror
        )
        .set(value);
    }

    pub fn set_owner_block(self, value: Option<TextBlockObject<'gc>>, mc: &Mutation<'gc>) {
        unlock!(Gc::write(mc, self.0), ContentElementObjectData, owner_block).set(value);
    }

    pub fn group(self) -> Option<ContentElementObject<'gc>> {
        self.0.group.get()
    }

    pub fn set_group(self, value: Option<ContentElementObject<'gc>>, mc: &Mutation<'gc>) {
        unlock!(Gc::write(mc, self.0), ContentElementObjectData, group).set(value);
    }

    /// The block this element is in, directly or through its groups.
    pub fn text_block(self) -> Option<TextBlockObject<'gc>> {
        match self.group() {
            Some(group) => group.text_block(),
            None => self.0.owner_block.get(),
        }
    }

    /// This element's text as a block sees it: a graphic is U+FDEF.
    pub fn raw_len(self) -> usize {
        match &*self.element_data() {
            ElementData::Text { text } => text.map_or(0, |t| t.len()),
            ElementData::Group { elements } => elements.iter().map(|e| e.raw_len()).sum(),
            ElementData::Graphic { .. } => 1,
            ElementData::Invalid => 0,
        }
    }

    /// Where this element starts in its block's text, if it's in one.
    pub fn block_begin_index(self) -> Option<usize> {
        let Some(group) = self.group() else {
            return self.0.owner_block.get().map(|_| 0);
        };
        let begin = group.block_begin_index()?;
        let data = group.element_data();
        let ElementData::Group { elements } = &*data else {
            return None;
        };
        let mut before = 0;
        for element in elements {
            if Object::ptr_eq(*element, self) {
                return Some(begin + before);
            }
            before += element.raw_len();
        }
        None
    }

    pub fn element_data(&self) -> Ref<'_, ElementData<'gc>> {
        self.0.element_data.borrow()
    }

    pub fn element_data_mut(&self, mc: &Mutation<'gc>) -> RefMut<'_, ElementData<'gc>> {
        unlock!(
            Gc::write(mc, self.0),
            ContentElementObjectData,
            element_data
        )
        .borrow_mut()
    }

    pub fn text(self, activation: &mut Activation<'_, 'gc>) -> Option<AvmString<'gc>> {
        match &*self.element_data() {
            ElementData::Text { text } => *text,
            ElementData::Group { elements } => {
                let mut result = None;

                for element in elements {
                    // Recursively call `text` to concatenate the descendants
                    // of the `GroupElement`
                    let new_text = element.text(activation);

                    if let Some(new_text) = new_text {
                        if let Some(existing_text) = result {
                            // If `result` is non-`null`, concatenate to it
                            result =
                                Some(AvmString::concat(activation.gc(), existing_text, new_text));
                        } else {
                            // If `result` is `null`, override it
                            result = Some(new_text);
                        }
                    } else if result.is_some_and(|r| r.is_empty()) {
                        // If `result` is an empty string but `new_text` is
                        // null, set `result` to null
                        result = None;
                    }
                }

                result
            }
            ElementData::Graphic { .. } => Some(istr!("")),
            ElementData::Invalid => None,
        }
    }
}

impl<'gc> TObject<'gc> for ContentElementObject<'gc> {
    fn gc_base(&self) -> Gc<'gc, ScriptObjectData<'gc>> {
        HasPrefixField::as_prefix_gc(self.0)
    }
}
