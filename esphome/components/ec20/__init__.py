import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@local"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["text_sensor"]

ec20_ns = cg.esphome_ns.namespace("ec20")
EC20Component = ec20_ns.class_("EC20Component", cg.Component)

CONF_AT_RESPONSE = "at_response"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EC20Component),
        cv.Optional(CONF_AT_RESPONSE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_PORT, default=9000): cv.port,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    add_idf_component(name="espressif/usb_host_cdc_acm", ref="2.4.1")
    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_port(config[CONF_PORT]))

    if at_response := config.get(CONF_AT_RESPONSE):
        sens = await text_sensor.new_text_sensor(at_response)
        cg.add(var.set_at_response_sensor(sens))
