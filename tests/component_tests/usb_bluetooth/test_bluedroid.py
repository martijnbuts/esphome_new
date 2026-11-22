"""Tests for Bluedroid on a USB Bluetooth dongle (usb_bluetooth bluedroid: true)."""

from collections.abc import Callable
from pathlib import Path

import pytest

from esphome.components.esp32.const import KEY_ESP32, KEY_SDKCONFIG_OPTIONS
from esphome.config import read_config
from esphome.core import CORE

BASE = (
    "esphome:\n  name: test\n\n"
    "esp32:\n  variant: esp32p4\n  framework:\n    type: esp-idf\n\n"
)
USB_HOST = "usb_host:\n  max_packet_size: 512\n\n"
TRACKER = "esp32_ble_tracker:\n\n"
DONGLE = "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n\n"


def _write(tmp_path: Path, text: str) -> Path:
    path = tmp_path / "test.yaml"
    path.write_text(BASE + text)
    return path


def _defines() -> set[str]:
    return {define.name for define in CORE.defines}


def test_bluedroid_runs_esp32_ble_on_the_dongle(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(_write(tmp_path, USB_HOST + DONGLE + TRACKER))
    assert "bt->set_bluedroid()" in main
    assert {
        "USE_USB_BLUETOOTH_BLUEDROID",
        "USE_USB_HOST_LIB_TASK",
        "USE_ESP32_BLE_USB_HCI",
    } <= _defines()
    options = CORE.data[KEY_ESP32][KEY_SDKCONFIG_OPTIONS]
    assert options["CONFIG_BT_CONTROLLER_DISABLED"] is True
    assert options["CONFIG_BT_BLUEDROID_ENABLED"] is True
    assert options["CONFIG_BT_BLE_ENABLED"] is True


def test_without_bluedroid_esp32_ble_is_untouched(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(
        _write(tmp_path, USB_HOST + "usb_bluetooth:\n  - id: bt\n\n" + TRACKER)
    )
    assert "set_bluedroid" not in main
    assert (
        not {
            "USE_USB_BLUETOOTH_BLUEDROID",
            "USE_USB_HOST_LIB_TASK",
            "USE_ESP32_BLE_USB_HCI",
        }
        & _defines()
    )
    assert "CONFIG_BT_CONTROLLER_DISABLED" not in CORE.data[KEY_ESP32].get(
        KEY_SDKCONFIG_OPTIONS, {}
    )


@pytest.mark.parametrize(
    ("text", "message"),
    [
        pytest.param(
            USB_HOST + DONGLE,
            "bluedroid is for esp32_ble, which this configuration does not have",
            id="needs_esp32_ble",
        ),
        pytest.param(
            USB_HOST
            + DONGLE
            + TRACKER
            + "esp32_hosted:\n  variant: ESP32C6\n  reset_pin: GPIO54\n  cmd_pin: GPIO19\n"
            "  clk_pin: GPIO18\n  d0_pin: GPIO14\n  d1_pin: GPIO15\n  d2_pin: GPIO16\n"
            "  d3_pin: GPIO17\n\n",
            "esp32_ble already uses the co-processor of esp32_hosted",
            id="not_with_esp32_hosted",
        ),
        pytest.param(
            USB_HOST
            + "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n    vid: 0x2357\n    pid: 0x0604\n"
            "  - id: bt2\n    bluedroid: true\n    vid: 0x0BDA\n    pid: 0x8771\n\n"
            + TRACKER,
            "Only one usb_bluetooth entry can have bluedroid",
            id="one_dongle",
        ),
        pytest.param(
            "usb_host:\n  max_packet_size: 512\n  max_transfer_requests: 4\n\n"
            + DONGLE
            + TRACKER,
            "usb_bluetooth needs usb_host max_transfer_requests of 8 or more",
            id="transfer_requests",
        ),
    ],
)
def test_bluedroid_rejects(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], text: str, message: str
) -> None:
    CORE.config_path = _write(tmp_path, text)
    assert read_config({}) is None
    assert message in capsys.readouterr().out


def test_without_bluedroid_three_transfer_requests_are_enough(
    tmp_path: Path,
) -> None:
    CORE.config_path = _write(
        tmp_path,
        "usb_host:\n  max_packet_size: 512\n  max_transfer_requests: 3\n\n"
        "usb_bluetooth:\n  - id: bt\n\n",
    )
    assert read_config({}) is not None
