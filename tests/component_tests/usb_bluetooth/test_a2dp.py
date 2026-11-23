"""Tests for streaming to a Bluetooth speaker (usb_bluetooth a2dp_speaker and the speaker platform)."""

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


def test_speaker_status_text_sensor(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(
        _write(
            tmp_path,
            "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n"
            "    a2dp_speaker: 4C:72:74:24:C8:05\n\n"
            "text_sensor:\n  - platform: usb_bluetooth\n"
            "    speaker_status:\n      name: Speaker status\n",
        )
    )
    assert "bt->set_speaker_status_text_sensor(" in main


def test_speaker_signal_sensor(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(
        _write(
            tmp_path,
            "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n"
            "    a2dp_speaker: 4C:72:74:24:C8:05\n\n"
            "sensor:\n  - platform: usb_bluetooth\n"
            "    speaker_signal:\n      name: Speaker signal\n",
        )
    )
    assert "bt->set_speaker_signal_sensor(" in main
    assert "USE_USB_BLUETOOTH_SPEAKER_SIGNAL" in _defines()


SPEAKER = "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n    a2dp_speaker: 4C:72:74:24:C8:05\n"


@pytest.mark.parametrize(
    ("option", "expected"),
    [
        pytest.param("", "BLE_SCAN_PAUSE_CONNECTING", id="default_while_connecting"),
        pytest.param(
            "    pause_ble_scan: streaming\n",
            "BLE_SCAN_PAUSE_STREAMING",
            id="while_streaming",
        ),
    ],
)
def test_pause_ble_scan(
    tmp_path: Path,
    generate_main: Callable[[str | Path], str],
    option: str,
    expected: str,
) -> None:
    main = generate_main(_write(tmp_path, SPEAKER + option))
    assert f"bt->set_ble_scan_pause(usb_bluetooth::BleScanPause::{expected})" in main
    assert "USE_USB_BLUETOOTH_PAUSE_BLE_SCAN" in _defines()


def test_pause_ble_scan_never(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(_write(tmp_path, SPEAKER + "    pause_ble_scan: never\n"))
    assert "set_ble_scan_pause" not in main
    assert "USE_USB_BLUETOOTH_PAUSE_BLE_SCAN" not in _defines()


def test_pause_ble_scan_needs_a_tracker(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    path = tmp_path / "test.yaml"
    path.write_text(BASE.replace("esp32_ble_tracker:\n\n", "esp32_ble:\n\n") + SPEAKER)
    main = generate_main(path)
    assert "set_ble_scan_pause" not in main
    assert "USE_USB_BLUETOOTH_PAUSE_BLE_SCAN" not in _defines()


@pytest.mark.parametrize(
    ("text", "message"),
    [
        pytest.param(
            "usb_bluetooth:\n  - id: bt\n    a2dp_speaker: 4C:72:74:24:C8:05\n",
            "a2dp_speaker needs bluedroid: true",
            id="speaker_needs_bluedroid",
        ),
        pytest.param(
            "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n\n"
            "speaker:\n  - platform: usb_bluetooth\n    id: bt_speaker\n",
            "The speaker platform needs a2dp_speaker or a speaker select on the usb_bluetooth entry 'bt'",
            id="speaker_platform_needs_a2dp_speaker",
        ),
        pytest.param(
            "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n\n"
            "text_sensor:\n  - platform: usb_bluetooth\n"
            "    connected_speaker:\n      name: Connected speaker\n",
            "connected_speaker needs a2dp_speaker or a speaker select on the usb_bluetooth entry 'bt'",
            id="text_sensor_needs_a2dp_speaker",
        ),
        pytest.param(
            "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n\n"
            "text_sensor:\n  - platform: usb_bluetooth\n"
            "    speaker_status:\n      name: Speaker status\n",
            "speaker_status needs a2dp_speaker or a speaker select on the usb_bluetooth entry 'bt'",
            id="speaker_status_needs_a2dp_speaker",
        ),
        pytest.param(
            "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n\n"
            "sensor:\n  - platform: usb_bluetooth\n"
            "    speaker_signal:\n      name: Speaker signal\n",
            "speaker_signal needs a2dp_speaker or a speaker select on the usb_bluetooth entry 'bt'",
            id="speaker_signal_needs_a2dp_speaker",
        ),
        pytest.param(
            "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n    pause_ble_scan: streaming\n",
            "pause_ble_scan needs a2dp_speaker or a speaker select",
            id="pause_ble_scan_needs_a2dp",
        ),
    ],
)
def test_a2dp_rejects(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], text: str, message: str
) -> None:
    CORE.config_path = _write(tmp_path, text)
    assert read_config({}) is None
    assert message in capsys.readouterr().out
