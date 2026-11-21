#pragma once

#include "esphome/core/defines.h"

#ifdef USE_USB_BLUETOOTH_SPEAKER

#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/usb_bluetooth/usb_bluetooth.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <atomic>
#include <memory>

namespace esphome::usb_bluetooth {

/// The A2DP speaker of a usb_bluetooth dongle as an ESPHome speaker: audio is buffered here and Bluedroid's A2DP
/// source task takes it out as 16-bit stereo PCM at 44.1 kHz. The stream to the speaker starts when audio comes and
/// is suspended a few seconds after it stops; while the speaker is not connected, audio is dropped.
class USBBluetoothSpeaker final : public speaker::Speaker, public Component, public Parented<USBBluetooth> {
 public:
  explicit USBBluetoothSpeaker(size_t buffer_size) : buffer_size_(buffer_size) {
    this->audio_stream_info_ = audio::AudioStreamInfo(16, 2, 44100);
  }

  void setup() override;
  void loop() override;
  void dump_config() override;

  void start() override;
  void stop() override;
  void finish() override;
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }
  bool has_buffered_data() const override;
  void set_volume(float volume) override;
  void set_mute_state(bool mute_state) override;

  /// Whether there is audio to stream (read by usb_bluetooth's main loop work)
  bool wants_stream() const { return this->active_.load(std::memory_order_acquire); }
  /// A2DP source task: fill frames (16-bit stereo) from the buffer, silence where it runs short
  void fill(int16_t *frames, size_t count);

 protected:
  std::unique_ptr<ring_buffer::RingBuffer> buffer_;
  size_t buffer_size_;
  std::atomic<bool> active_{false};
  bool finishing_{false};
};

}  // namespace esphome::usb_bluetooth

#endif
