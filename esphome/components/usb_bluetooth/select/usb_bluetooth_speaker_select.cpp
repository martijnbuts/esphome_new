#include "usb_bluetooth_speaker_select.h"

#ifdef USE_USB_BLUETOOTH_SPEAKER_SELECT

#include "esphome/core/log.h"

#include <cstring>

namespace esphome::usb_bluetooth {

ESPHOME_LOG_TAG(TAG, "usb_bluetooth.select");

static const char *const OPTION_NONE = "(none)";
static const char *const OPTION_SEARCH = "Search for speakers";
static constexpr uint8_t NO_PEER[6]{};

void USBBluetoothSpeakerSelect::setup() {
  this->pref_ = global_preferences->make_preference<SavedSpeaker>(this->get_preference_hash(), true);
  if (this->pref_.load(&this->current_) && memcmp(this->current_.address, NO_PEER, sizeof(NO_PEER)) != 0) {
    // A speaker chosen here wins over a2dp_speaker in the configuration
    this->current_.name[sizeof(this->current_.name) - 1] = 0;
    this->parent_->set_a2dp_peer(this->current_.address);
  } else {
    memset(&this->current_, 0, sizeof(this->current_));
    memcpy(this->current_.address, this->parent_->a2dp_peer(), sizeof(this->current_.address));
  }
  this->update_options();
}

void USBBluetoothSpeakerSelect::dump_config() { LOG_SELECT("", "USB Bluetooth speaker", this); }

void USBBluetoothSpeakerSelect::update_options() {
  const uint8_t found_count = this->parent_->found_speakers(this->found_);
  uint8_t count = 0;
  if (memcmp(this->current_.address, NO_PEER, sizeof(NO_PEER)) != 0) {
    format_speaker(this->labels_[0], LABEL_SIZE, this->current_.name, this->current_.address);
    this->option_list_[count] = this->labels_[0];
  } else {
    this->option_list_[count] = OPTION_NONE;
  }
  this->option_found_[count++] = -1;
  for (uint8_t i = 0; i < found_count; i++) {
    if (memcmp(this->found_[i].address, this->current_.address, sizeof(NO_PEER)) == 0)
      continue;
    format_speaker(this->labels_[count], LABEL_SIZE, this->found_[i].name, this->found_[i].address);
    this->option_list_[count] = this->labels_[count];
    this->option_found_[count++] = static_cast<int8_t>(i);
  }
  // One fixed label: Home Assistant only knows the options it read when it connected
  this->option_list_[count] = OPTION_SEARCH;
  this->option_found_[count++] = -1;
  this->option_count_ = count;
  this->traits.set_options_static(this->option_list_, count);
  // While searching the select shows the search; otherwise the current speaker
  this->publish_state(this->parent_->scanning() ? count - 1 : 0);
}

void USBBluetoothSpeakerSelect::connected_speaker_changed() {
  uint8_t address[6];
  char name[sizeof(this->current_.name)];
  if (!this->parent_->connected_speaker(address, name, sizeof(name)) || name[0] == 0 ||
      memcmp(address, this->current_.address, sizeof(address)) != 0 ||
      strncmp(name, this->current_.name, sizeof(name)) == 0)
    return;
  memcpy(this->current_.name, name, sizeof(name));
  if (!this->pref_.save(&this->current_)) {
    ESP_LOGW(TAG, "Could not store the speaker name");
  }
  this->update_options();
}

void USBBluetoothSpeakerSelect::control(size_t index) {
  if (index >= this->option_count_)
    return;
  if (index == static_cast<size_t>(this->option_count_ - 1)) {
    if (!this->parent_->scanning())
      this->parent_->scan_speakers();
  } else if (this->option_found_[index] >= 0) {
    const FoundSpeaker &found = this->found_[this->option_found_[index]];
    memcpy(this->current_.address, found.address, sizeof(this->current_.address));
    memcpy(this->current_.name, found.name, sizeof(this->current_.name));
    if (!this->pref_.save(&this->current_)) {
      ESP_LOGW(TAG, "Could not store the speaker; it is used until the next restart");
    }
    this->parent_->set_a2dp_peer(this->current_.address);
  }
  this->update_options();
}

}  // namespace esphome::usb_bluetooth

#endif
