"""Tests for the camera_encoder component."""

from collections.abc import Callable
from pathlib import Path

import pytest

from esphome import config_validation as cv
from esphome.components.esp32 import KEY_BOARD, VARIANT_ESP32P4, VARIANT_ESP32S3
from esphome.const import KEY_VARIANT, PlatformFramework
from esphome.core import CORE
from tests.component_tests.types import SetCoreConfigCallable


@pytest.mark.parametrize(
    ("board", "variant"),
    [("esp32-s3-devkitc-1", VARIANT_ESP32S3)],
)
def test_hardware_encoder_rejected_without_codec(
    set_core_config: SetCoreConfigCallable, board: str, variant: str
) -> None:
    """The hardware type is only accepted on chips with a JPEG codec."""
    set_core_config(
        PlatformFramework.ESP32_IDF,
        platform_data={KEY_BOARD: board, KEY_VARIANT: variant},
    )
    from esphome.components.camera_encoder import CONFIG_SCHEMA

    with pytest.raises(
        cv.Invalid, match="The hardware JPEG encoder is only available on ESP32P4"
    ):
        CONFIG_SCHEMA({"type": "hardware"})


def test_hardware_encoder_defaults(set_core_config: SetCoreConfigCallable) -> None:
    """The hardware type fills in its own defaults on the ESP32-P4."""
    set_core_config(
        PlatformFramework.ESP32_IDF,
        platform_data={KEY_BOARD: "esp32-p4-evboard", KEY_VARIANT: VARIANT_ESP32P4},
    )
    from esphome.components.camera_encoder import CONFIG_SCHEMA

    config = CONFIG_SCHEMA({"type": "hardware"})
    assert config["quality"] == 80
    assert config["buffer_size"] == 65536
    assert config["buffer_expand_size"] == 32768
    assert str(config["subsampling"]) == "4:2:0"


def test_hardware_encoder_codegen(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    """The hardware type creates its encoder and buffer and enables the codec driver."""
    main_cpp = generate_main(component_config_path("hardware.yaml"))

    assert "camera_encoder::ESP32HardwareJPEGEncoderBuffer();" in main_cpp
    assert "->set_buffer_size(65536);" in main_cpp
    assert (
        "camera_encoder::ESP32HardwareJPEGEncoder(90, ::JPEG_DOWN_SAMPLING_YUV444,"
        in main_cpp
    )
    assert "set_buffer_expand_size(32768)" in main_cpp
    assert "USE_ESP32_HARDWARE_JPEG_ENCODER" in {d.name for d in CORE.defines}
    assert "USE_ESP32_CAMERA_JPEG_ENCODER" not in {d.name for d in CORE.defines}
