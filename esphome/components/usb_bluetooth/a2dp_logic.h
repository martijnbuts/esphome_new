#pragma once

// The A2DP logic of usb_bluetooth that needs no Bluetooth stack: the link state machine, the volume scale, sample
// scaling and the text of the connected speaker. Kept apart so it can be tested on the host.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

/// Time between connection attempts while the speaker is off or out of range.
static constexpr uint32_t A2DP_RETRY_MS = 10000;
/// Time without audio before the stream is suspended (the connection stays).
static constexpr uint32_t A2DP_SUSPEND_AFTER_MS = 3000;
/// Highest AVRCP absolute volume.
static constexpr uint8_t AVRC_VOLUME_MAX = 0x7F;

/// One step of the A2DP state machine. wanted: there is audio to stream. may_connect: a speaker is chosen and no
/// search is running. last_try: millis() of the last connection attempt, 0 for none yet. last_wanted: millis() when
/// audio was last wanted. The caller does the action; a failed connection request keeps A2DP_IDLE.
inline A2dpStep a2dp_step(A2dpState state, bool wanted, bool may_connect, uint32_t now, uint32_t last_try,
                          uint32_t last_wanted) {
  switch (state) {
    case A2DP_IDLE:
      if (!may_connect || (last_try != 0 && now - last_try < A2DP_RETRY_MS))
        return {A2DP_IDLE, A2dpAction::A2DP_ACTION_NONE};
      return {A2DP_CONNECTING, A2dpAction::A2DP_ACTION_CONNECT};
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

/// A device found by a search for speakers.
struct FoundSpeaker {
  uint8_t address[6];
  char name[32];
};

/// Devices kept from one search.
static constexpr uint8_t MAX_FOUND_SPEAKERS = 8;

/// Whether a Bluetooth Class of Device is an audio or video device (speaker, headphones, radio, car audio).
inline bool is_audio_device(uint32_t cod) {
  static constexpr uint32_t MAJOR_AUDIO_VIDEO = 0x04;
  return ((cod >> 8) & 0x1F) == MAJOR_AUDIO_VIDEO;
}

/// Add a device to the found list, or fill in its name; true when the list changed. Devices past max are dropped.
inline bool add_found_speaker(FoundSpeaker *list, uint8_t &count, uint8_t max, const uint8_t *address,
                              const char *name) {
  for (uint8_t i = 0; i < count; i++) {
    if (memcmp(list[i].address, address, sizeof(list[i].address)) != 0)
      continue;
    if (name[0] == 0 || strncmp(list[i].name, name, sizeof(list[i].name) - 1) == 0)
      return false;
    snprintf(list[i].name, sizeof(list[i].name), "%s", name);
    return true;
  }
  if (count >= max)
    return false;
  memcpy(list[count].address, address, sizeof(list[count].address));
  snprintf(list[count].name, sizeof(list[count].name), "%s", name);
  count++;
  return true;
}

}  // namespace esphome::usb_bluetooth
