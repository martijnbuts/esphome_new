"""Sensors of a usb_bluetooth dongle."""

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_SIGNAL_STRENGTH,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL,
)
from esphome.types import ConfigType

from . import CONF_USB_BLUETOOTH_ID, DOMAIN, USBBluetooth, require_a2dp

DEPENDENCIES = [DOMAIN]

CONF_SPEAKER_SIGNAL = "speaker_signal"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_USB_BLUETOOTH_ID): cv.use_id(USBBluetooth),
        # The signal of the connected speaker as the controller reports it (HCI Read RSSI): 0 while it is
        # inside the controller's ideal receive range, below 0 when it is weaker
        cv.Optional(CONF_SPEAKER_SIGNAL): sensor.sensor_schema(
            unit_of_measurement=UNIT_DECIBEL,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


def _final_validate(config: ConfigType) -> ConfigType:
    if CONF_SPEAKER_SIGNAL in config:
        require_a2dp(config[CONF_USB_BLUETOOTH_ID], CONF_SPEAKER_SIGNAL)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    if (conf := config.get(CONF_SPEAKER_SIGNAL)) is None:
        return
    hub = await cg.get_variable(config[CONF_USB_BLUETOOTH_ID])
    cg.add_define("USE_USB_BLUETOOTH_SPEAKER_SIGNAL")
    sens = await sensor.new_sensor(conf)
    cg.add(hub.set_speaker_signal_sensor(sens))
