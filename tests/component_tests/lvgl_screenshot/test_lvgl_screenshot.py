"""Tests for the lvgl_screenshot component."""

from collections.abc import Callable
from pathlib import Path

from esphome.components.lvgl import defines as lvgl_defines
from esphome.core import CORE


def test_screenshot_codegen(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    """The camera gets its encoder and interval, and LVGL's snapshot API is turned on."""
    main_cpp = generate_main(component_config_path("screenshot.yaml"))

    assert "lvgl_screenshot::LvglScreenshot(screenshot_encoder, 2000);" in main_cpp
    defines = {d.name for d in CORE.defines}
    assert "USE_CAMERA" in defines
    assert "USE_LVGL_SCREENSHOT" in defines
    assert "USE_LVGL_SNAPSHOT" in defines
    assert lvgl_defines.get_defines().get("LV_USE_SNAPSHOT") == "1"


def test_screenshot_default_encoder(
    generate_main: Callable[[str | Path], str],
    component_config_path: Callable[[str], Path],
) -> None:
    """Without a camera_encoder block one is loaded and used, with a 1 s interval."""
    main_cpp = generate_main(component_config_path("screenshot_default.yaml"))

    assert "camera_encoder::ESP32CameraJPEGEncoder(80," in main_cpp
    assert ", 1000);" in main_cpp
    assert (
        "lvgl_screenshot::LvglScreenshot(camera_encoder_esp32camerajpegencoder_id"
        in (main_cpp)
    )
