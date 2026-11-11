import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light, spi, output
from esphome import pins
from esphome.const import CONF_OUTPUT_ID, CONF_DATA_PIN, CONF_CLOCK_PIN, CONF_MAX_REFRESH_RATE, CONF_OUTPUT

DEPENDENCIES = ["esp32"]

CONF_LATCH_PIN = "latch_pin"

ikea_frekvens_ns = cg.esphome_ns.namespace('ikea_frekvens_simple')
IkeaFrekvensLightOutput = ikea_frekvens_ns.class_('IkeaFrekvensSimpleLightOutput', light.AddressableLight, spi.SPIDevice)

CONFIG_SCHEMA = light.ADDRESSABLE_LIGHT_SCHEMA.extend({
    cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(IkeaFrekvensLightOutput),
    cv.Required(CONF_LATCH_PIN): pins.internal_gpio_output_pin_number,
    cv.Optional(CONF_OUTPUT): cv.use_id(output.FloatOutput),
}).extend(spi.spi_device_schema(cs_pin_required=False))

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await light.register_light(var, config)
    await spi.register_spi_device(var, config)

    cg.add(var.set_latch_pin(config[CONF_LATCH_PIN]))
    if CONF_OUTPUT in config:
        out = await cg.get_variable(config[CONF_OUTPUT])
        cg.add(var.set_output(out))
