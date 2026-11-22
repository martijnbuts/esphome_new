"""Tests for choosing the Bluetooth speaker with a select (usb_bluetooth select platform)."""

from collections.abc import Callable
from pathlib import Path

import pytest

from esphome.components.esp32.const import KEY_ESP32, KEY_SDKCONFIG_OPTIONS
from esphome.config import read_config
from esphome.core import CORE

BASE = (
    "esphome:\n  name: test\n\n"
    "esp32:\n  variant: esp32p4\n  framework:\n    type: esp-idf\n\n"
    "usb_host:\n  max_packet_size: 512\n\n"
    "esp32_ble_tracker:\n\n"
)
SELECT = "select:\n  - platform: usb_bluetooth\n    speaker:\n      name: Bluetooth speaker\n\n"


def _write(tmp_path: Path, text: str) -> Path:
    path = tmp_path / "test.yaml"
    path.write_text(BASE + text)
    return path


def _defines() -> set[str]:
    return {define.name for define in CORE.defines}


def test_select_turns_on_a2dp_without_a2dp_speaker(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(
        _write(
            tmp_path,
            "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n\n"
            + SELECT
            + "speaker:\n  - platform: usb_bluetooth\n    id: bt_speaker\n",
        )
    )
    assert "bt->enable_a2dp()" in main
    assert "set_a2dp_speaker" not in main
    assert "bt->set_speaker_select(" in main
    assert {"USE_USB_BLUETOOTH_A2DP", "USE_USB_BLUETOOTH_SPEAKER_SELECT"} <= _defines()
    options = CORE.data[KEY_ESP32][KEY_SDKCONFIG_OPTIONS]
    assert options["CONFIG_BT_A2DP_ENABLE"] is True
    assert options["CONFIG_BT_CLASSIC_ENABLED"] is True


def test_select_with_a2dp_speaker_keeps_the_configured_speaker(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(
        _write(
            tmp_path,
            "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n"
            "    a2dp_speaker: 4C:72:74:24:C8:05\n\n" + SELECT,
        )
    )
    assert "bt->set_a2dp_speaker(0x4C727424C805ULL)" in main
    assert "enable_a2dp" not in main


def test_select_needs_bluedroid(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    CORE.config_path = _write(tmp_path, "usb_bluetooth:\n  - id: bt\n\n" + SELECT)
    assert read_config({}) is None
    assert (
        "The speaker select needs bluedroid: true on the usb_bluetooth entry 'bt'"
        in capsys.readouterr().out
    )
