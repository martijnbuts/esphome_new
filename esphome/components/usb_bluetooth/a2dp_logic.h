#pragma once

// The A2DP logic of usb_bluetooth that needs no Bluetooth stack: the link state machine, the volume scale, sample
// scaling and the text of the connected speaker. Kept apart so it can be tested on the host.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

namespace esphome::usb_bluetooth {

/// States of the A2DP link to the speaker; from A2DP_CONNECTED on the speaker is connected.
enum A2dpState : uint8_t {
  A2DP_IDLE,
  A2DP_CONNECTING,
  A2DP_CONNECTED,
  A2DP_READY,
  A2DP_STARTING,
  A2DP_STREAMING,
  A2DP_SUSPENDING,
  A2DP_SUSPENDED,
};

/// What the main loop asks Bluedroid for in one step.
enum class A2dpAction : uint8_t {
  A2DP_ACTION_NONE,
  A2DP_ACTION_CONNECT,
  A2DP_ACTION_CHECK_READY,
  A2DP_ACTION_START,
  A2DP_ACTION_SUSPEND,
};

struct A2dpStep {
  A2dpState state;
  A2dpAction action;
};

/// Time between connection attempts while the speaker is off or out of range. It doubles with each failed attempt
/// after the first, up to A2DP_RETRY_MAX_MS, so a speaker that stays off costs little radio time.
static constexpr uint32_t A2DP_RETRY_MS = 10000;
static constexpr uint32_t A2DP_RETRY_MAX_MS = 60000;
/// Time between connection attempts while there is audio to play.
static constexpr uint32_t A2DP_WANTED_RETRY_MS = 3000;
/// Failed connection attempts are counted up to here.
static constexpr uint8_t A2DP_MAX_FAILURES = 8;
/// Time without audio before the stream is suspended (the connection stays).
static constexpr uint32_t A2DP_SUSPEND_AFTER_MS = 3000;
/// Highest AVRCP absolute volume.
static constexpr uint8_t AVRC_VOLUME_MAX = 0x7F;

/// Time to wait before the next connection attempt after this many failed ones in a row.
inline uint32_t a2dp_retry_ms(uint8_t failures) {
  if (failures <= 1)
    return A2DP_RETRY_MS;
  return std::min<uint32_t>(A2DP_RETRY_MS << std::min<uint8_t>(failures - 1, 3), A2DP_RETRY_MAX_MS);
}

/// One step of the A2DP state machine. wanted: there is audio to stream. may_connect: a speaker is chosen and no
/// search is running. last_try: millis() of the last connection attempt, 0 for none yet. last_wanted: millis() when
/// audio was last wanted. failures: failed connection attempts in a row. The caller does the action; a failed
/// connection request keeps A2DP_IDLE.
inline A2dpStep a2dp_step(A2dpState state, bool wanted, bool may_connect, uint32_t now, uint32_t last_try,
                          uint32_t last_wanted, uint8_t failures = 0) {
  switch (state) {
    case A2DP_IDLE: {
      const uint32_t wait = wanted ? A2DP_WANTED_RETRY_MS : a2dp_retry_ms(failures);
      if (!may_connect || (last_try != 0 && now - last_try < wait))
        return {A2DP_IDLE, A2dpAction::A2DP_ACTION_NONE};
      return {A2DP_CONNECTING, A2dpAction::A2DP_ACTION_CONNECT};
    }
    case A2DP_CONNECTING:
      // No answer: start over
      if (now - last_try > 3 * A2DP_RETRY_MS)
        return {A2DP_IDLE, A2dpAction::A2DP_ACTION_NONE};
      break;
    case A2DP_CONNECTED:
      // Only stream when there is something to play; the speaker is set up for media once this is acknowledged
      if (wanted)
        return {A2DP_CONNECTED, A2dpAction::A2DP_ACTION_CHECK_READY};
      break;
    case A2DP_SUSPENDED:
      if (wanted)
        return {A2DP_CONNECTED, A2dpAction::A2DP_ACTION_NONE};
      break;
    case A2DP_READY:
      return {A2DP_STARTING, A2dpAction::A2DP_ACTION_START};
    case A2DP_STREAMING:
      // Nothing to play for a while: suspend; the connection stays and the speaker can go idle
      if (!wanted && now - last_wanted > A2DP_SUSPEND_AFTER_MS)
        return {A2DP_SUSPENDING, A2dpAction::A2DP_ACTION_SUSPEND};
      break;
    default:
      break;
  }
  return {state, A2dpAction::A2DP_ACTION_NONE};
}

/// What the speaker link is doing, for the speaker status text sensor.
enum class SpeakerStatus : uint8_t {
  SPEAKER_STATUS_NONE,
  SPEAKER_STATUS_SEARCHING,
  SPEAKER_STATUS_CONNECTING,
  SPEAKER_STATUS_NOT_REACHABLE,
  SPEAKER_STATUS_CONNECTED,
  SPEAKER_STATUS_PLAYING,
};

/// The text of each SpeakerStatus.
static constexpr const char *SPEAKER_STATUS_NAMES[] = {"No speaker",    "Searching", "Connecting",
                                                       "Not reachable", "Connected", "Playing"};

/// The status of the speaker link. chosen: a speaker is set. failures: failed connection attempts in a row; after
/// one, the speaker counts as not reachable until it connects.
inline SpeakerStatus speaker_status(A2dpState state, bool chosen, bool searching, uint8_t failures) {
  if (searching)
    return SpeakerStatus::SPEAKER_STATUS_SEARCHING;
  if (state == A2DP_STARTING || state == A2DP_STREAMING)
    return SpeakerStatus::SPEAKER_STATUS_PLAYING;
  if (state >= A2DP_CONNECTED)
    return SpeakerStatus::SPEAKER_STATUS_CONNECTED;
  if (!chosen)
    return SpeakerStatus::SPEAKER_STATUS_NONE;
  return failures > 0 ? SpeakerStatus::SPEAKER_STATUS_NOT_REACHABLE : SpeakerStatus::SPEAKER_STATUS_CONNECTING;
}

/// When the BLE scan (esp32_ble_tracker, bluetooth_proxy) gives the radio to Classic Bluetooth.
enum class BleScanPause : uint8_t {
  BLE_SCAN_PAUSE_NEVER,
  BLE_SCAN_PAUSE_CONNECTING,
  BLE_SCAN_PAUSE_STREAMING,
};

/// Whether the BLE scan is paused: while connecting to the speaker or searching for speakers, and with
/// BLE_SCAN_PAUSE_STREAMING also for as long as audio is streamed.
inline bool ble_scan_paused(BleScanPause mode, A2dpState state, bool searching) {
  if (mode == BleScanPause::BLE_SCAN_PAUSE_NEVER)
    return false;
  if (searching || state == A2DP_CONNECTING)
    return true;
  return mode == BleScanPause::BLE_SCAN_PAUSE_STREAMING &&
         (state == A2DP_READY || state == A2DP_STARTING || state == A2DP_STREAMING || state == A2DP_SUSPENDING);
}

/// The AVRCP absolute volume (0..127) of a level from 0 to 1.
inline uint8_t avrc_volume(float level) {
  return static_cast<uint8_t>(lroundf(std::clamp(level, 0.0f, 1.0f) * AVRC_VOLUME_MAX));
}

/// Scale 16-bit samples by gain (0..1), for a speaker without absolute volume.
inline void a2dp_scale(int16_t *samples, size_t count, float gain) {
  for (size_t i = 0; i < count; i++)
    samples[i] = static_cast<int16_t>(samples[i] * gain);
}

/// "Name (AA:BB:CC:DD:EE:FF)", or only the address while the name is not known.
inline void format_speaker(char *out, size_t size, const char *name, const uint8_t *address) {
  const bool named = name[0] != 0;
  snprintf(out, size, "%s%s%02X:%02X:%02X:%02X:%02X:%02X%s", name, named ? " (" : "", address[0], address[1],
           address[2], address[3], address[4], address[5], named ? ")" : "");
}

/// A button on the speaker, sent to the ESP32 as an AVRCP pass-through command.
enum class SpeakerButton : uint8_t {
  SPEAKER_BUTTON_PLAY,
  SPEAKER_BUTTON_PAUSE,
  SPEAKER_BUTTON_STOP,
  SPEAKER_BUTTON_NEXT,
  SPEAKER_BUTTON_PREVIOUS,
  SPEAKER_BUTTON_VOLUME_UP,
  SPEAKER_BUTTON_VOLUME_DOWN,
  SPEAKER_BUTTON_NONE,
};

/// Event type names of the buttons, in the order of SpeakerButton.
static constexpr const char *SPEAKER_BUTTON_NAMES[] = {"play",     "pause",     "stop",       "next",
                                                       "previous", "volume_up", "volume_down"};

/// The button of an AVRCP pass-through operation id (AV/C Panel Subunit), or SPEAKER_BUTTON_NONE.
inline SpeakerButton speaker_button(uint8_t key_code) {
  struct KeyButton {
    uint8_t key_code;
    SpeakerButton button;
  };
  static constexpr KeyButton KEY_BUTTONS[] = {
      {0x44, SpeakerButton::SPEAKER_BUTTON_PLAY},        {0x46, SpeakerButton::SPEAKER_BUTTON_PAUSE},
      {0x45, SpeakerButton::SPEAKER_BUTTON_STOP},        {0x4B, SpeakerButton::SPEAKER_BUTTON_NEXT},
      {0x4C, SpeakerButton::SPEAKER_BUTTON_PREVIOUS},    {0x41, SpeakerButton::SPEAKER_BUTTON_VOLUME_UP},
      {0x42, SpeakerButton::SPEAKER_BUTTON_VOLUME_DOWN},
  };
  for (const auto &entry : KEY_BUTTONS) {
    if (entry.key_code == key_code)
      return entry.button;
  }
  return SpeakerButton::SPEAKER_BUTTON_NONE;
}

/// The level from 0 to 1 of an AVRCP absolute volume (0..127).
inline float avrc_level(uint8_t volume) { return std::min<uint8_t>(volume, AVRC_VOLUME_MAX) / float(AVRC_VOLUME_MAX); }

/// Signal strength of a search result that carried none; sorts below every real value.
static constexpr int8_t RSSI_UNKNOWN = -128;

/// A device found by a search for speakers.
struct FoundSpeaker {
  uint8_t address[6];
  char name[32];
  /// Signal strength of the first result that carried one, in dBm
  int8_t rssi;
  /// Its name has been asked for after the search (it did not send one itself)
  bool name_requested;
};

/// Devices kept from one search.
static constexpr uint8_t MAX_FOUND_SPEAKERS = 8;

/// Whether a Bluetooth Class of Device is an audio or video device (speaker, headphones, radio, car audio).
inline bool is_audio_device(uint32_t cod) {
  static constexpr uint32_t MAJOR_AUDIO_VIDEO = 0x04;
  return ((cod >> 8) & 0x1F) == MAJOR_AUDIO_VIDEO;
}

/// Add a device to the found list, or fill in its name or signal strength; true when the list changed.
/// The list stays sorted by signal strength, strongest (nearest) first. A full list gives its weakest place to a
/// stronger device. The first signal strength of a device is kept, so the order does not change while the
/// device keeps answering during the search.
inline bool add_found_speaker(FoundSpeaker *list, uint8_t &count, uint8_t max, const uint8_t *address, const char *name,
                              int8_t rssi = RSSI_UNKNOWN) {
  uint8_t index = count;
  for (uint8_t i = 0; i < count; i++) {
    if (memcmp(list[i].address, address, sizeof(list[i].address)) == 0) {
      index = i;
      break;
    }
  }
  if (index < count) {
    bool changed = false;
    if (name[0] != 0 && strncmp(list[index].name, name, sizeof(list[index].name) - 1) != 0) {
      snprintf(list[index].name, sizeof(list[index].name), "%s", name);
      changed = true;
    }
    if (rssi != RSSI_UNKNOWN && list[index].rssi == RSSI_UNKNOWN) {
      list[index].rssi = rssi;
      changed = true;
    }
    if (!changed)
      return false;
  } else {
    if (count < max) {
      index = count++;
    } else if (max > 0 && rssi > list[count - 1].rssi) {
      index = count - 1;
    } else {
      return false;
    }
    memcpy(list[index].address, address, sizeof(list[index].address));
    snprintf(list[index].name, sizeof(list[index].name), "%s", name);
    list[index].rssi = rssi;
    list[index].name_requested = false;
  }
  // Move the changed entry to its place
  while (index > 0 && list[index - 1].rssi < list[index].rssi) {
    std::swap(list[index - 1], list[index]);
    index--;
  }
  while (index + 1 < count && list[index + 1].rssi > list[index].rssi) {
    std::swap(list[index + 1], list[index]);
    index++;
  }
  return true;
}

/// The first found device without a name that has not been asked for its name yet, or nullptr.
inline FoundSpeaker *next_unnamed_speaker(FoundSpeaker *list, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    if (list[i].name[0] == 0 && !list[i].name_requested)
      return &list[i];
  }
  return nullptr;
}

}  // namespace esphome::usb_bluetooth
