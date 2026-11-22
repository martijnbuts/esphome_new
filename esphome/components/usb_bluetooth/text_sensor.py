"""Text sensors of a usb_bluetooth dongle."""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.types import ConfigType

from . import CONF_USB_BLUETOOTH_ID, DOMAIN, USBBluetooth, require_a2dp

DEPENDENCIES = [DOMAIN]

CONF_CONNECTED_SPEAKER = "connected_speaker"

ICON_SPEAKER_BLUETOOTH = "mdi:speaker-bluetooth"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_USB_BLUETOOTH_ID): cv.use_id(USBBluetooth),
        # The name and address of the Bluetooth speaker while it is connected; empty while it is not
        cv.Optional(CONF_CONNECTED_SPEAKER): text_sensor.text_sensor_schema(
            icon=ICON_SPEAKER_BLUETOOTH
        ),
    }
)


def _final_validate(config: ConfigType) -> ConfigType:
    if CONF_CONNECTED_SPEAKER not in config:
        return config
    require_a2dp(config[CONF_USB_BLUETOOTH_ID], CONF_CONNECTED_SPEAKER)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    if (conf := config.get(CONF_CONNECTED_SPEAKER)) is None:
        return
    hub = await cg.get_variable(config[CONF_USB_BLUETOOTH_ID])
    sens = await text_sensor.new_text_sensor(conf)
    cg.add(hub.set_connected_speaker_text_sensor(sens))
