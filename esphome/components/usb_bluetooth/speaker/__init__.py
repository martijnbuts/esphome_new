"""The A2DP speaker that a usb_bluetooth dongle streams to, as an ESPHome speaker."""

import esphome.codegen as cg
from esphome.components import audio, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_BUFFER_DURATION,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
)
import esphome.final_validate as fv
from esphome.types import ConfigType

from .. import (
    CONF_A2DP_SPEAKER,
    CONF_USB_BLUETOOTH_ID,
    DOMAIN,
    USBBluetooth,
    usb_bluetooth_ns,
)

AUTO_LOAD = ["audio", "ring_buffer"]
DEPENDENCIES = ["usb_bluetooth"]


# A2DP with the SBC codec in Bluedroid takes 16-bit stereo at 44.1 kHz
A2DP_SAMPLE_RATE = 44100
A2DP_CHANNELS = 2
A2DP_BITS = 16

USBBluetoothSpeaker = usb_bluetooth_ns.class_(
    "USBBluetoothSpeaker",
    speaker.Speaker,
    cg.Component,
    cg.Parented.template(USBBluetooth),
)


def _set_stream_limits(config: ConfigType) -> ConfigType:
    # The limits validator edits the config in place and returns None
    audio.set_stream_limits(
        min_bits_per_sample=A2DP_BITS,
        max_bits_per_sample=A2DP_BITS,
        min_channels=A2DP_CHANNELS,
        max_channels=A2DP_CHANNELS,
        min_sample_rate=A2DP_SAMPLE_RATE,
        max_sample_rate=A2DP_SAMPLE_RATE,
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBBluetoothSpeaker),
            cv.GenerateID(CONF_USB_BLUETOOTH_ID): cv.use_id(USBBluetooth),
            cv.Optional(
                CONF_BUFFER_DURATION, default="500ms"
            ): cv.positive_time_period_milliseconds,
            # Fixed by A2DP; here so resampler and mixer speakers can inherit them
            cv.Optional(CONF_SAMPLE_RATE, default=A2DP_SAMPLE_RATE): cv.one_of(
                A2DP_SAMPLE_RATE, int=True
            ),
            cv.Optional(CONF_NUM_CHANNELS, default=A2DP_CHANNELS): cv.one_of(
                A2DP_CHANNELS, int=True
            ),
            cv.Optional(CONF_BITS_PER_SAMPLE, default=f"{A2DP_BITS}bit"): cv.All(
                cv.float_with_unit("bits", "bit"), cv.one_of(A2DP_BITS, int=True)
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


def _final_validate(config: ConfigType) -> ConfigType:
    parent_id = config[CONF_USB_BLUETOOTH_ID]
    for entry in fv.full_config.get().get(DOMAIN) or []:
        if entry[CONF_ID] == parent_id and CONF_A2DP_SPEAKER not in entry:
            raise cv.Invalid(
                f"The usb_bluetooth entry '{parent_id}' needs {CONF_A2DP_SPEAKER}"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    frames = A2DP_SAMPLE_RATE * config[CONF_BUFFER_DURATION].total_milliseconds // 1000
    var = cg.new_Pvariable(config[CONF_ID], frames * A2DP_CHANNELS * A2DP_BITS // 8)
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    await cg.register_parented(var, config[CONF_USB_BLUETOOTH_ID])
    parent = await cg.get_variable(config[CONF_USB_BLUETOOTH_ID])
    cg.add(parent.set_a2dp_source(var))
    cg.add_define("USE_USB_BLUETOOTH_SPEAKER")
