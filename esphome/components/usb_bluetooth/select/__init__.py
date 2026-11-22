"""Choose the Bluetooth speaker of a usb_bluetooth dongle: a select with the speakers a search found."""

import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_SPEAKER
import esphome.final_validate as fv
from esphome.types import ConfigType

from .. import (
    CONF_BLUEDROID,
    CONF_USB_BLUETOOTH_ID,
    DOMAIN,
    USBBluetooth,
    usb_bluetooth_ns,
)

DEPENDENCIES = [DOMAIN]

USBBluetoothSpeakerSelect = usb_bluetooth_ns.class_(
    "USBBluetoothSpeakerSelect", select.Select, cg.Component
)

ICON_SPEAKER_BLUETOOTH = "mdi:speaker-bluetooth"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_USB_BLUETOOTH_ID): cv.use_id(USBBluetooth),
        # The current speaker, the speakers the last search found, and an option that starts a search
        cv.Required(CONF_SPEAKER): select.select_schema(
            USBBluetoothSpeakerSelect, icon=ICON_SPEAKER_BLUETOOTH
        ).extend(cv.COMPONENT_SCHEMA),
    }
)


def _final_validate(config: ConfigType) -> ConfigType:
    parent_id = config[CONF_USB_BLUETOOTH_ID]
    for entry in fv.full_config.get().get(DOMAIN) or []:
        if entry[CONF_ID] == parent_id and not entry[CONF_BLUEDROID]:
            raise cv.Invalid(
                f"The speaker select needs {CONF_BLUEDROID}: true on the usb_bluetooth entry '{parent_id}'"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    conf = config[CONF_SPEAKER]
    hub = await cg.get_variable(config[CONF_USB_BLUETOOTH_ID])
    # The options are found at run time
    var = await select.new_select(conf, options=[])
    await cg.register_component(var, conf)
    await cg.register_parented(var, hub)
    cg.add(hub.set_speaker_select(var))
    cg.add_define("USE_USB_BLUETOOTH_SPEAKER_SELECT")
