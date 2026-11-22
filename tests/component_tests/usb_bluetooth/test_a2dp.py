"""Tests for streaming to a Bluetooth speaker (usb_bluetooth a2dp_speaker and the speaker platform)."""

from collections.abc import Callable
from pathlib import Path

from esphome.components.esp32.const import KEY_ESP32, KEY_SDKCONFIG_OPTIONS
from esphome.core import CORE

BASE = (
    "esphome:\n  name: test\n\n"
    "esp32:\n  variant: esp32p4\n  framework:\n    type: esp-idf\n\n"
    "usb_host:\n  max_packet_size: 512\n\n"
    "esp32_ble_tracker:\n\n"
)


def _write(tmp_path: Path, text: str) -> Path:
    path = tmp_path / "test.yaml"
    path.write_text(BASE + text)
    return path


def _defines() -> set[str]:
    return {define.name for define in CORE.defines}


def test_a2dp_speaker_enables_classic_and_a2dp(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(
        _write(
            tmp_path,
            "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n"
            "    a2dp_speaker: 4C:72:74:24:C8:05\n\n"
            "speaker:\n  - platform: usb_bluetooth\n    id: bt_speaker\n",
        )
    )
    assert "bt->set_a2dp_speaker(0x4C727424C805ULL)" in main
    assert "bt->set_a2dp_source(bt_speaker)" in main
    assert {"USE_USB_BLUETOOTH_A2DP", "USE_USB_BLUETOOTH_SPEAKER"} <= _defines()
    options = CORE.data[KEY_ESP32][KEY_SDKCONFIG_OPTIONS]
    assert options["CONFIG_BT_A2DP_ENABLE"] is True
    assert options["CONFIG_BT_CLASSIC_ENABLED"] is True


def test_bluedroid_alone_leaves_classic_off(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(
        _write(tmp_path, "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n")
    )
    assert "set_a2dp_speaker" not in main
    assert "USE_USB_BLUETOOTH_A2DP" not in _defines()
    assert (
        CORE.data[KEY_ESP32][KEY_SDKCONFIG_OPTIONS]["CONFIG_BT_CLASSIC_ENABLED"]
        is False
    )


def test_connected_speaker_text_sensor(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(
        _write(
            tmp_path,
            "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n"
            "    a2dp_speaker: 4C:72:74:24:C8:05\n\n"
            "text_sensor:\n  - platform: usb_bluetooth\n"
            "    connected_speaker:\n      name: Connected speaker\n",
        )
    )
    assert "bt->set_connected_speaker_text_sensor(" in main
