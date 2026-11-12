from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import display, output
import esphome.config_validation as cv
from esphome.const import (
    CONF_BRIGHTNESS,
    CONF_CLOCK_PIN,
    CONF_DATA_PIN,
    CONF_ID,
    CONF_LAMBDA,
)

from . import frekvens_ns

CONF_LATCH_PIN = "latch_pin"
CONF_ENABLE_PIN = "enable_pin"
CONF_ENABLE_PIN_INVERTED = "enable_pin_inverted"
CONF_ENABLE_OUTPUT = "enable_output"
CONF_CLOCK_SPEED = "clock_speed"

DEFAULT_CLOCK_SPEED = 10_000_000

FrekvensDisplay = frekvens_ns.class_("FrekvensDisplay", display.DisplayBuffer)
FrekvensSetBrightnessAction = frekvens_ns.class_(
    "FrekvensSetBrightnessAction", automation.Action, cg.Parented.template(FrekvensDisplay)
)

def validate_enable(config):
    if CONF_ENABLE_PIN in config and CONF_ENABLE_OUTPUT in config:
        raise cv.Invalid(f"Use either '{CONF_ENABLE_PIN}' or '{CONF_ENABLE_OUTPUT}', not both")
    return config


CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(FrekvensDisplay),
        cv.Required(CONF_DATA_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_CLOCK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_LATCH_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_ENABLE_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_ENABLE_PIN_INVERTED, default=True): cv.boolean,
        # Hardware dimming via the OE pin: preserves the full BAM grayscale
        # resolution at any brightness.
        cv.Optional(CONF_ENABLE_OUTPUT): cv.use_id(output.FloatOutput),
        cv.Optional(CONF_CLOCK_SPEED, default=DEFAULT_CLOCK_SPEED): cv.int_range(
            min=1_000_000, max=25_000_000
        ),
        # Global dimming is applied to the pixel values themselves (before BAM), so it
        # does not interfere with the panel refresh rate.
        cv.Optional(CONF_BRIGHTNESS, default=1.0): cv.percentage,
    }
    ),
    validate_enable,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)

    cg.add(var.set_data_pin(config[CONF_DATA_PIN]))
    cg.add(var.set_clock_pin(config[CONF_CLOCK_PIN]))
    cg.add(var.set_latch_pin(config[CONF_LATCH_PIN]))
    cg.add(var.set_clock_speed(config[CONF_CLOCK_SPEED]))

    if CONF_ENABLE_PIN in config:
        cg.add(var.set_enable_pin(config[CONF_ENABLE_PIN]))
        cg.add(var.set_enable_pin_inverted(config[CONF_ENABLE_PIN_INVERTED]))

    if CONF_ENABLE_OUTPUT in config:
        out = await cg.get_variable(config[CONF_ENABLE_OUTPUT])
        cg.add(var.set_enable_output(out))

    cg.add(var.set_brightness(config[CONF_BRIGHTNESS]))

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))


@automation.register_action(
    "frekvens.set_brightness",
    FrekvensSetBrightnessAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(FrekvensDisplay),
            cv.Required(CONF_BRIGHTNESS): cv.templatable(cv.percentage),
        },
        key=CONF_BRIGHTNESS,
    ),
    synchronous=True,
)
async def frekvens_set_brightness_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    brightness = await cg.templatable(config[CONF_BRIGHTNESS], args, cg.float_)
    cg.add(var.set_brightness(brightness))
    return var
