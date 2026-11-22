"""Events of a usb_bluetooth dongle: the buttons on the Bluetooth speaker."""

import esphome.codegen as cg
from esphome.components import event
import esphome.config_validation as cv
from esphome.types import ConfigType

from . import CONF_USB_BLUETOOTH_ID, DOMAIN, USBBluetooth, require_a2dp

DEPENDENCIES = [DOMAIN]

CONF_SPEAKER_BUTTONS = "speaker_buttons"

ICON_SPEAKER_BUTTONS = "mdi:play-pause"

# The order of SpeakerButton in a2dp_logic.h
EVENT_TYPES = ["play", "pause", "stop", "next", "previous", "volume_up", "volume_down"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_USB_BLUETOOTH_ID): cv.use_id(USBBluetooth),
        # Play, pause, stop, next and previous pressed on the speaker, and volume up and down on a speaker without
        # absolute volume
        cv.Required(CONF_SPEAKER_BUTTONS): event.event_schema(
            event.Event, icon=ICON_SPEAKER_BUTTONS
        ),
    }
)


def _final_validate(config: ConfigType) -> ConfigType:
    require_a2dp(config[CONF_USB_BLUETOOTH_ID], CONF_SPEAKER_BUTTONS)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_USB_BLUETOOTH_ID])
    var = await event.new_event(config[CONF_SPEAKER_BUTTONS], event_types=EVENT_TYPES)
    cg.add(hub.set_speaker_buttons_event(var))
    cg.add_define("USE_USB_BLUETOOTH_SPEAKER_BUTTONS")
