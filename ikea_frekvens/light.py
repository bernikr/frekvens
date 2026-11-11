import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome import pins
from esphome.const import CONF_OUTPUT_ID, CONF_DATA_PIN, CONF_CLOCK_PIN, CONF_MAX_REFRESH_RATE

DEPENDENCIES = ["esp32"]

CONF_LATCH_PIN = "latch_pin"

ikea_frekvens_ns = cg.esphome_ns.namespace('ikea_frekvens')
IkeaFrekvensLightOutput = ikea_frekvens_ns.class_('IkeaFrekvensLightOutput', light.AddressableLight)

CONFIG_SCHEMA = light.ADDRESSABLE_LIGHT_SCHEMA.extend({
    cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(IkeaFrekvensLightOutput),
    cv.Required(CONF_DATA_PIN): pins.internal_gpio_output_pin_number,
    cv.Required(CONF_CLOCK_PIN): pins.internal_gpio_output_pin_number,
    cv.Required(CONF_LATCH_PIN): pins.internal_gpio_output_pin_number,
    cv.Optional(CONF_MAX_REFRESH_RATE): cv.positive_time_period_microseconds,
})

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await light.register_light(var, config)

    cg.add(var.set_data_pin(config[CONF_DATA_PIN]))
    cg.add(var.set_clock_pin(config[CONF_CLOCK_PIN]))
    cg.add(var.set_latch_pin(config[CONF_LATCH_PIN]))
    if CONF_MAX_REFRESH_RATE in config:
        cg.add(var.set_max_refresh_rate(config[CONF_MAX_REFRESH_RATE]))
