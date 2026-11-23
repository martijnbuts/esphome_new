"""Text sensors of a usb_bluetooth dongle."""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.types import ConfigType

from . import CONF_USB_BLUETOOTH_ID, DOMAIN, USBBluetooth, require_a2dp

DEPENDENCIES = [DOMAIN]

CONF_CONNECTED_SPEAKER = "connected_speaker"
CONF_SPEAKER_STATUS = "speaker_status"

ICON_SPEAKER_BLUETOOTH = "mdi:speaker-bluetooth"
ICON_BLUETOOTH_AUDIO = "mdi:bluetooth-audio"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_USB_BLUETOOTH_ID): cv.use_id(USBBluetooth),
        # The name and address of the Bluetooth speaker while it is connected; empty while it is not
        cv.Optional(CONF_CONNECTED_SPEAKER): text_sensor.text_sensor_schema(
            icon=ICON_SPEAKER_BLUETOOTH
        ),
        # What the speaker link does: No speaker, Searching, Connecting, Not reachable, Connected or Playing
        cv.Optional(CONF_SPEAKER_STATUS): text_sensor.text_sensor_schema(
            icon=ICON_BLUETOOTH_AUDIO
        ),
    }
)

SENSORS = (CONF_CONNECTED_SPEAKER, CONF_SPEAKER_STATUS)


def _final_validate(config: ConfigType) -> ConfigType:
    for key in SENSORS:
        if key in config:
            require_a2dp(config[CONF_USB_BLUETOOTH_ID], key)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_USB_BLUETOOTH_ID])
    for key in SENSORS:
        if (conf := config.get(key)) is not None:
            sens = await text_sensor.new_text_sensor(conf)
            cg.add(getattr(hub, f"set_{key}_text_sensor")(sens))
