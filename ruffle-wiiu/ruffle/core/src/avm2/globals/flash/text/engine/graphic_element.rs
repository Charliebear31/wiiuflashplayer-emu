use crate::avm2::activation::Activation;
use crate::avm2::error::Error;
use crate::avm2::function::FunctionArgs;
use crate::avm2::object::ElementData;
use crate::avm2::parameters::ParametersExt;
use crate::avm2::value::Value;

pub fn set_graphic_data<'gc>(
    activation: &mut Activation<'_, 'gc>,
    this: Value<'gc>,
    args: FunctionArgs<'_, 'gc>,
) -> Result<Value<'gc>, Error<'gc>> {
    let this = this
        .as_object()
        .unwrap()
        .as_content_element_object()
        .unwrap();

    let graphic = args.try_get_object(0).and_then(|o| o.as_display_object());
    let width = args.get_f64(1);
    let height = args.get_f64(2);

    let mut data = this.element_data_mut(activation.gc());
    *data = ElementData::Graphic {
        graphic,
        width,
        height,
    };

    Ok(Value::Undefined)
}
