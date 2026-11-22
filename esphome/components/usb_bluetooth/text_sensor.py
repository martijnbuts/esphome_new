"""Text sensors of a usb_bluetooth dongle."""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID
import esphome.final_validate as fv
from esphome.types import ConfigType

from . import CONF_A2DP_SPEAKER, DOMAIN, USBBluetooth

DEPENDENCIES = [DOMAIN]

CONF_USB_BLUETOOTH_ID = "usb_bluetooth_id"
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
    parent_id = config[CONF_USB_BLUETOOTH_ID]
    for entry in fv.full_config.get().get(DOMAIN) or []:
        if entry[CONF_ID] == parent_id and CONF_A2DP_SPEAKER not in entry:
            raise cv.Invalid(
                f"{CONF_CONNECTED_SPEAKER} needs {CONF_A2DP_SPEAKER} on the usb_bluetooth entry '{parent_id}'"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    if (conf := config.get(CONF_CONNECTED_SPEAKER)) is None:
        return
    hub = await cg.get_variable(config[CONF_USB_BLUETOOTH_ID])
    sens = await text_sensor.new_text_sensor(conf)
    cg.add(hub.set_connected_speaker_text_sensor(sens))
