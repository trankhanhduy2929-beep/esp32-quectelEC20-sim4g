import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, text_sensor
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@local"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["binary_sensor", "sensor", "text_sensor"]

ec20_ns = cg.esphome_ns.namespace("ec20")
EC20Component = ec20_ns.class_("EC20Component", cg.Component)

CONF_AT_RESPONSE = "at_response"
CONF_USB_CONNECTED = "usb_connected"
CONF_TCP_CONNECTED = "tcp_connected"
CONF_RECONNECTS = "reconnects"
CONF_GNSS = "gnss"
CONF_LATITUDE = "latitude"
CONF_LONGITUDE = "longitude"
CONF_ALTITUDE = "altitude"
CONF_SPEED = "speed"
CONF_SATELLITES = "satellites"
CONF_FIX = "fix"

GNSS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_LATITUDE): sensor.sensor_schema(
            unit_of_measurement="°",
            accuracy_decimals=6,
            icon="mdi:latitude",
        ),
        cv.Optional(CONF_LONGITUDE): sensor.sensor_schema(
            unit_of_measurement="°",
            accuracy_decimals=6,
            icon="mdi:longitude",
        ),
        cv.Optional(CONF_ALTITUDE): sensor.sensor_schema(
            unit_of_measurement="m",
            accuracy_decimals=0,
            icon="mdi:elevation-rise",
        ),
        cv.Optional(CONF_SPEED): sensor.sensor_schema(
            unit_of_measurement="km/h",
            accuracy_decimals=1,
            icon="mdi:speedometer",
        ),
        cv.Optional(CONF_SATELLITES): sensor.sensor_schema(
            accuracy_decimals=0,
            icon="mdi:satellite-variant",
        ),
        cv.Optional(CONF_FIX): text_sensor.text_sensor_schema(
            icon="mdi:crosshairs-gps",
        ),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EC20Component),
        cv.Optional(CONF_AT_RESPONSE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_USB_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class="connectivity",
        ),
        cv.Optional(CONF_TCP_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class="connectivity",
        ),
        cv.Optional(CONF_RECONNECTS): sensor.sensor_schema(
            accuracy_decimals=0,
            icon="mdi:restart",
        ),
        cv.Optional(CONF_GNSS): GNSS_SCHEMA,
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

    if usb_connected := config.get(CONF_USB_CONNECTED):
        sens = await binary_sensor.new_binary_sensor(usb_connected)
        cg.add(var.set_usb_connected_sensor(sens))

    if tcp_connected := config.get(CONF_TCP_CONNECTED):
        sens = await binary_sensor.new_binary_sensor(tcp_connected)
        cg.add(var.set_tcp_connected_sensor(sens))

    if reconnects := config.get(CONF_RECONNECTS):
        sens = await sensor.new_sensor(reconnects)
        cg.add(var.set_reconnect_sensor(sens))

    if gnss := config.get(CONF_GNSS):
        if latitude := gnss.get(CONF_LATITUDE):
            sens = await sensor.new_sensor(latitude)
            cg.add(var.set_gnss_latitude_sensor(sens))
        if longitude := gnss.get(CONF_LONGITUDE):
            sens = await sensor.new_sensor(longitude)
            cg.add(var.set_gnss_longitude_sensor(sens))
        if altitude := gnss.get(CONF_ALTITUDE):
            sens = await sensor.new_sensor(altitude)
            cg.add(var.set_gnss_altitude_sensor(sens))
        if speed := gnss.get(CONF_SPEED):
            sens = await sensor.new_sensor(speed)
            cg.add(var.set_gnss_speed_sensor(sens))
        if satellites := gnss.get(CONF_SATELLITES):
            sens = await sensor.new_sensor(satellites)
            cg.add(var.set_gnss_satellites_sensor(sens))
        if fix := gnss.get(CONF_FIX):
            sens = await text_sensor.new_text_sensor(fix)
            cg.add(var.set_gnss_fix_sensor(sens))
