"""Tests for the buttons and the volume of the Bluetooth speaker (usb_bluetooth event platform, on_speaker_volume)."""

from collections.abc import Callable
from pathlib import Path

import pytest

from esphome.components.usb_bluetooth.event import EVENT_TYPES
from esphome.config import read_config
from esphome.core import CORE

BASE = (
    "esphome:\n  name: test\n\n"
    "esp32:\n  variant: esp32p4\n  framework:\n    type: esp-idf\n\n"
    "usb_host:\n  max_packet_size: 512\n\n"
    "esp32_ble_tracker:\n\n"
    "logger:\n\n"
)
DONGLE = "usb_bluetooth:\n  - id: bt\n    bluedroid: true\n"
SPEAKER = "    a2dp_speaker: 4C:72:74:24:C8:05\n"
EVENT = (
    "\nevent:\n  - platform: usb_bluetooth\n"
    "    speaker_buttons:\n      name: Speaker buttons\n"
)


def _write(tmp_path: Path, text: str) -> Path:
    path = tmp_path / "test.yaml"
    path.write_text(BASE + text)
    return path


def _defines() -> set[str]:
    return {define.name for define in CORE.defines}


def test_speaker_buttons_event(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(_write(tmp_path, DONGLE + SPEAKER + EVENT))
    assert "bt->set_speaker_buttons_event(" in main
    assert "USE_USB_BLUETOOTH_SPEAKER_BUTTONS" in _defines()
    for event_type in EVENT_TYPES:
        assert f'"{event_type}"' in main


def test_on_speaker_volume(
    tmp_path: Path, generate_main: Callable[[str | Path], str]
) -> None:
    main = generate_main(
        _write(
            tmp_path,
            DONGLE
            + SPEAKER
            + "    on_speaker_volume:\n      - logger.log: volume changed\n",
        )
    )
    assert "bt->add_on_speaker_volume_callback(" in main
    assert "USE_USB_BLUETOOTH_SPEAKER_BUTTONS" not in _defines()


@pytest.mark.parametrize(
    ("text", "message"),
    [
        pytest.param(
            DONGLE + EVENT,
            "speaker_buttons needs a2dp_speaker or a speaker select on the usb_bluetooth entry 'bt'",
            id="event_needs_a2dp",
        ),
        pytest.param(
            DONGLE + "    on_speaker_volume:\n      - logger.log: volume changed\n",
            "on_speaker_volume needs a2dp_speaker or a speaker select",
            id="volume_needs_a2dp",
        ),
    ],
)
def test_speaker_buttons_rejects(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], text: str, message: str
) -> None:
    CORE.config_path = _write(tmp_path, text)
    assert read_config({}) is None
    assert message in capsys.readouterr().out
