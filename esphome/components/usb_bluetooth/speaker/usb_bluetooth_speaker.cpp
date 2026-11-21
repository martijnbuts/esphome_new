#include "usb_bluetooth_speaker.h"

#ifdef USE_USB_BLUETOOTH_SPEAKER

#include "esphome/core/log.h"

#include <esp_timer.h>

#include <cstring>

namespace esphome::usb_bluetooth {

ESPHOME_LOG_TAG(TAG, "usb_bluetooth.speaker");

static constexpr size_t BYTES_PER_FRAME = 4;  // 16-bit stereo

void USBBluetoothSpeaker::setup() {
  this->buffer_ = ring_buffer::RingBuffer::create(this->buffer_size_);
  if (this->buffer_ == nullptr) {
    this->mark_failed(LOG_STR("Audio buffer not allocated"));
    return;
  }
  this->disable_loop();
}

void USBBluetoothSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG,
                "USB Bluetooth speaker (A2DP):\n"
                "  Buffer: %u bytes",
                static_cast<unsigned>(this->buffer_size_));
}

void USBBluetoothSpeaker::start() {
  if (this->buffer_ == nullptr || this->state_ == speaker::STATE_RUNNING)
    return;
  this->finishing_ = false;
  this->state_ = speaker::STATE_RUNNING;
  this->active_.store(true, std::memory_order_release);
  this->enable_loop();
}

void USBBluetoothSpeaker::stop() {
  this->active_.store(false, std::memory_order_release);
  this->finishing_ = false;
  if (this->buffer_ != nullptr)
    this->buffer_->reset();
  this->state_ = speaker::STATE_STOPPED;
}

void USBBluetoothSpeaker::finish() {
  if (this->state_ != speaker::STATE_RUNNING) {
    this->stop();
    return;
  }
  // loop() stops once the buffered audio has gone to the speaker
  this->finishing_ = true;
  this->enable_loop();
}

void USBBluetoothSpeaker::loop() {
  if (this->finishing_ && !this->has_buffered_data())
    this->stop();
  if (this->state_ == speaker::STATE_STOPPED)
    this->disable_loop();
}

size_t USBBluetoothSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->buffer_ == nullptr)
    return 0;
  if (this->state_ != speaker::STATE_RUNNING)
    this->start();
  // Not connected: drop the audio, so whoever plays does not wait for a speaker that is off
  if (!this->parent_->a2dp_connected())
    return length;
  return this->buffer_->write_without_replacement(data, length, ticks_to_wait);
}

bool USBBluetoothSpeaker::has_buffered_data() const {
  return this->buffer_ != nullptr && this->buffer_->available() > 0;
}

void USBBluetoothSpeaker::set_volume(float volume) {
  speaker::Speaker::set_volume(volume);
  this->parent_->set_volume(volume);
}

void USBBluetoothSpeaker::set_mute_state(bool mute_state) {
  speaker::Speaker::set_mute_state(mute_state);
  this->parent_->set_mute(mute_state);
}

void USBBluetoothSpeaker::fill(int16_t *frames, size_t count) {
  size_t got = 0;
  if (this->active_.load(std::memory_order_acquire) && this->buffer_ != nullptr)
    got = this->buffer_->read(frames, count * BYTES_PER_FRAME, 0) / BYTES_PER_FRAME;
  if (got < count)
    memset(frames + 2 * got, 0, (count - got) * BYTES_PER_FRAME);
  if (got != 0)
    this->audio_output_callback_.call(static_cast<uint32_t>(got), esp_timer_get_time());
}

}  // namespace esphome::usb_bluetooth

#endif
