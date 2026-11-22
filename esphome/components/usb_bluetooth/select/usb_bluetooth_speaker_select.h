#pragma once

#include "esphome/core/defines.h"

#ifdef USE_USB_BLUETOOTH_SPEAKER_SELECT

#include "esphome/components/select/select.h"
#include "esphome/components/usb_bluetooth/usb_bluetooth.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

namespace esphome::usb_bluetooth {

/// Choose the Bluetooth speaker of a usb_bluetooth dongle: the current speaker, the devices the last search found,
/// and an option that starts a search. The choice is kept in flash.
class USBBluetoothSpeakerSelect final : public select::Select, public Component, public Parented<USBBluetooth> {
 public:
  void setup() override;
  void dump_config() override;
  /// Main loop: rebuild the options after a search found a device, started or ended.
  void update_options();
  /// Main loop: take the name the current speaker reports when it connects, which a2dp_speaker does not give.
  void connected_speaker_changed();

 protected:
  void control(size_t index) override;

  struct SavedSpeaker {
    uint8_t address[6];
    char name[32];
  };
  static constexpr size_t LABEL_SIZE = sizeof(SavedSpeaker::name) + 20;

  ESPPreferenceObject pref_;
  SavedSpeaker current_{};
  FoundSpeaker found_[MAX_FOUND_SPEAKERS]{};
  // The current speaker, the found ones and the search option; option_list_ points into labels_ and at literals
  char labels_[MAX_FOUND_SPEAKERS + 1][LABEL_SIZE]{};
  const char *option_list_[MAX_FOUND_SPEAKERS + 2]{};
  // Index of found_ behind each option, -1 for the current speaker and the search option
  int8_t option_found_[MAX_FOUND_SPEAKERS + 2]{};
  uint8_t option_count_{0};
};

}  // namespace esphome::usb_bluetooth

#endif
