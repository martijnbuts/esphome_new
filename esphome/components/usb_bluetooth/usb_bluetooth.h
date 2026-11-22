#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)

#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#ifdef USE_USB_BLUETOOTH_A2DP
#include "a2dp_logic.h"
#endif
#if defined(USE_USB_BLUETOOTH_A2DP) && defined(USE_TEXT_SENSOR)
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_USB_BLUETOOTH_BLUEDROID
struct esp_bluedroid_hci_driver_callbacks;
#endif

namespace esphome::usb_bluetooth {

#ifdef USE_USB_BLUETOOTH_SPEAKER
class USBBluetoothSpeaker;
#endif
#ifdef USE_USB_BLUETOOTH_SPEAKER_SELECT
class USBBluetoothSpeakerSelect;
#endif

/// A USB Bluetooth dongle (Bluetooth Core Specification, USB transport) as a usb_host client.
///
/// HCI commands are class requests on endpoint 0, events arrive on the interrupt IN endpoint of
/// the controller interface (class E0, subclass 01, protocol 01) and ACL data on its bulk pair.
/// On connect the controller is reset, a Realtek controller still on its ROM gets the configured
/// firmware, and its version, address and name are read and logged.
class USBBluetooth final : public usb_host::USBClient {
 public:
  using usb_host::USBClient::USBClient;

  /// One Realtek patch: the bytes downloaded into a controller with this lmp_subver and ROM version.
  struct RealtekImage {
    const uint8_t *data;
    size_t length;
    uint16_t lmp_subver;
    uint8_t rom_version;
  };

  void setup() override;
  void loop() override;
  void dump_config() override;

  /// Storage for the Realtek patches, sized by code generation.
  void set_realtek_firmware_storage(std::span<RealtekImage> storage) { this->realtek_images_.init(storage); }
  /// A Realtek patch for one ROM version of the chip with this lmp_subver, prepared by realtek.py.
  void add_realtek_firmware(uint16_t lmp_subver, uint8_t rom_version, const uint8_t *data, size_t length) {
    this->realtek_images_.push_back({data, length, lmp_subver, rom_version});
  }

#ifdef USE_USB_BLUETOOTH_BLUEDROID
  /// This dongle is the controller of Bluedroid (esp32_ble)
  void set_bluedroid();
  // Bluedroid's host-only transport (esp_bluedroid_hci.h); its tasks call these
  void hci_send(const uint8_t *h4, uint16_t len);
  bool hci_send_available() const;
  void hci_attach(const ::esp_bluedroid_hci_driver_callbacks *callbacks);
#endif
#ifdef USE_USB_BLUETOOTH_A2DP
  /// Stream to an A2DP sink (speaker); the speaker is set with set_a2dp_speaker() or chosen at run time.
  void enable_a2dp();
  /// The A2DP sink (speaker) to connect to and stream to, as a 48-bit address.
  void set_a2dp_speaker(uint64_t address);
  /// Main loop: connect to this speaker from now on; a connection to another one is closed.
  void set_a2dp_peer(const uint8_t *address);
  /// The speaker to connect to; all zero while none is chosen.
  const uint8_t *a2dp_peer() const { return this->a2dp_peer_; }
  /// Main loop: search 13 s for audio devices. The list of the previous search is cleared.
  void scan_speakers();
  /// Whether a search for speakers is running.
  bool scanning() const { return this->scanning_.load(std::memory_order_acquire); }
  /// Copy the devices found by the last search into out, MAX_FOUND_SPEAKERS entries; returns how many.
  uint8_t found_speakers(FoundSpeaker *out);
#ifdef USE_USB_BLUETOOTH_SPEAKER_SELECT
  /// The select that lists the found speakers; told from the main loop when the list changes.
  void set_speaker_select(USBBluetoothSpeakerSelect *select) { this->speaker_select_ = select; }
#endif
  /// The speaker volume, 0..1: sent to the speaker over AVRCP (absolute volume), or applied to the samples.
  void set_volume(float level);
  /// Silence the audio to the speaker.
  void set_mute(bool mute) { this->a2dp_mute_.store(mute, std::memory_order_release); }
  /// Whether the A2DP connection to the speaker is up.
  bool a2dp_connected() const { return this->a2dp_state_.load(std::memory_order_acquire) >= A2DP_CONNECTED; }
#ifdef USE_USB_BLUETOOTH_SPEAKER
  /// The speaker component whose audio is streamed.
  void set_a2dp_source(USBBluetoothSpeaker *source) { this->a2dp_source_ = source; }
#endif
  // Bluedroid's BTC task (AVRCP)
  void avrc_connected_(bool connected);
  void avrc_absolute_volume_(bool supported);
  // A2DP source task: PCM frames for the speaker
  void a2dp_fill_(int16_t *frames, size_t count);
  // Bluedroid's BTC task
  void a2dp_connection_(int state, const uint8_t *address);
  void a2dp_media_ack_(int cmd, int status);
  void speaker_name_(const uint8_t *address, const char *name);
  void found_speaker_(const uint8_t *address, uint32_t cod, const char *name);
  void scan_state_(bool running);
#ifdef USE_TEXT_SENSOR
  SUB_TEXT_SENSOR(connected_speaker)
#endif
#endif

  /// Whether a controller is connected and answered the start-up commands.
  bool is_ready() const { return this->step_ == ProbeStep::PROBE_STEP_READY; }
  /// The controller's public address, most significant byte first; all zero until it is ready.
  const uint8_t *address() const { return this->address_; }

 protected:
  enum class ProbeStep : uint8_t {
    PROBE_STEP_IDLE,
    PROBE_STEP_RESET,
    PROBE_STEP_READ_VERSION,
    PROBE_STEP_RTL_READ_ROM_VERSION,
    PROBE_STEP_RTL_DOWNLOAD,
    PROBE_STEP_READ_VERSION_PATCHED,
    PROBE_STEP_READ_ADDRESS,
    PROBE_STEP_READ_NAME,
    PROBE_STEP_READY,
    PROBE_STEP_FAILED,
  };

  // An event is code, parameter length and up to 255 parameter bytes.
  static constexpr size_t EVENT_MAX = 2 + 255;
  // An ACL packet is handle, length and up to 1021 bytes (the largest BR/EDR packet, 3-DH5)
  static constexpr size_t ACL_MAX = 4 + 1021;

  void on_connected() override;
  void on_disconnected() override;
  bool reports_connection_itself() const override { return true; }

  enum class FindResult : uint8_t { FIND_RESULT_FOUND, FIND_RESULT_NONE, FIND_RESULT_ERROR };
  FindResult find_interface_();
  void decline_();
  // Main loop: open the next device that is plugged in and not yet looked at, after one was closed
  // without being claimed.
  void try_next_device_();
  // USB task (main loop for the first read). False when no transfer slot was free.
  bool read_event_(uint32_t generation);
#ifdef USE_USB_BLUETOOTH_BLUEDROID
  bool read_acl_(uint32_t generation);
  // USB task: a packet for Bluedroid, its H4 type in byte 0
  void to_host_(uint8_t *h4, size_t len);
  // USB task: a command or ACL transfer to the dongle finished
  void send_done_();
  // Bluedroid's HCI task: answer a command while there is no dongle
  void answer_without_controller_(uint16_t opcode);
#endif
  // Main loop.
  void send_command_(uint16_t opcode, const uint8_t *params, size_t len);
  void handle_command_complete_(const uint8_t *ret, size_t len);
  void next_fragment_();
  void finish_probe_(bool ok);

  StaticFixedVector<RealtekImage> realtek_images_;
  // The HCI command being sent, reserved in setup() so sending allocates nothing
  std::vector<uint8_t> command_{};
  // Device addresses (0..127) already looked at and not to be opened again while they stay plugged in
  uint32_t declined_[4]{};
  const RealtekImage *download_{nullptr};

  // Bumped on connect and disconnect, so completions of an earlier device are dropped.
  std::atomic<uint32_t> generation_{0};
  // The USB task stores the Command Complete of the pending command here and sets the flag;
  // the main loop handles it and clears the flag.
  std::atomic<bool> complete_ready_{false};
  // Set by the USB task when the event or ACL read stopped and could not be queued again.
  std::atomic<bool> events_stopped_{false};
  uint8_t complete_[EVENT_MAX]{};
  size_t complete_len_{0};
  // Event reassembly, USB task only. Byte 0 is kept free for the H4 packet type.
  uint8_t event_h4_[1 + EVENT_MAX]{};
  uint8_t *const event_{event_h4_ + 1};
  size_t event_len_{0};
#ifdef USE_USB_BLUETOOTH_BLUEDROID
  // ACL reassembly, USB task only; byte 0 holds the H4 packet type
  uint8_t acl_h4_[1 + ACL_MAX]{};
  size_t acl_len_{0};
  const ::esp_bluedroid_hci_driver_callbacks *host_{nullptr};
  // The command Bluedroid is sending, reserved once so a command allocates nothing
  std::vector<uint8_t> hci_command_{};
  // Bluedroid owns the HCI traffic: events go to it rather than to the probe
  std::atomic<bool> host_attached_{false};
  // A send was refused for want of transfer slots; Bluedroid is told once one is free
  mutable std::atomic<bool> send_blocked_{false};
  // Bluedroid was running on this dongle when it went away: bring esp32_ble back once it is ready again
  bool restart_host_{false};
#endif
#ifdef USE_USB_BLUETOOTH_A2DP
  // Main loop, every second: set up Classic Bluetooth when esp32_ble becomes active, then keep the speaker connected
  void a2dp_poll_();
  bool a2dp_setup_();
  // Classic Bluetooth set up for the current run of Bluedroid; cleared when Bluetooth stops
  bool a2dp_up_{false};
  std::atomic<A2dpState> a2dp_state_{A2DP_IDLE};
  uint8_t a2dp_peer_[6]{};
  uint32_t a2dp_last_try_{0};
  // When audio was last wanted, so the stream is suspended a while after it stops
  uint32_t a2dp_last_wanted_{0};
  // The volume asked for, and the gain on the samples for a speaker without absolute volume
  float volume_{1.0f};
  float a2dp_gain_{1.0f};
  std::atomic<bool> avrc_abs_volume_{false};
  std::atomic<bool> a2dp_mute_{false};
#ifdef USE_TEXT_SENSOR
  void publish_connected_speaker_();
  // The connected speaker, written by Bluedroid's BTC task and published from the main loop
  Mutex speaker_lock_;
  char speaker_name_buf_[64]{};
  uint8_t speaker_address_[6]{};
  // Starts set, so the sensor gets a state at boot
  std::atomic<bool> speaker_changed_{true};
#endif
#ifdef USE_USB_BLUETOOTH_SPEAKER
  USBBluetoothSpeaker *a2dp_source_{nullptr};
#endif
#ifdef USE_USB_BLUETOOTH_SPEAKER_SELECT
  USBBluetoothSpeakerSelect *speaker_select_{nullptr};
#endif
  // Devices found by the last search, written by Bluedroid's BTC task and read from the main loop
  Mutex found_lock_;
  FoundSpeaker found_[MAX_FOUND_SPEAKERS]{};
  uint8_t found_count_{0};
  std::atomic<bool> found_changed_{false};
  std::atomic<bool> scanning_{false};
#endif

  uint32_t fragment_{0};
  uint32_t fragment_count_{0};
  std::atomic<uint16_t> pending_opcode_{0};
  uint16_t evt_mps_{0};
  uint16_t acl_in_mps_{0};
  uint16_t acl_out_mps_{0};
  uint16_t manufacturer_{0};
  uint16_t lmp_subver_{0};
  // The lmp_subver reported before the firmware download, to check that the download took
  uint16_t rom_lmp_subver_{0};
  uint16_t hci_revision_{0};
  uint8_t address_[6]{};
  uint8_t hci_version_{0};
  uint8_t intf_{0xFF};
  uint8_t evt_ep_{0};
  uint8_t acl_in_ep_{0};
  uint8_t acl_out_ep_{0};
  ProbeStep step_{ProbeStep::PROBE_STEP_IDLE};
};

#ifdef USE_USB_BLUETOOTH_BLUEDROID
/// For esp32_ble: hand Bluedroid the dongle's transport; false when there is no dongle to hand over.
bool attach_bluedroid();
/// For esp32_ble: take the transport back once Bluedroid is down.
void detach_bluedroid();
/// For esp32_ble: whether the dongle is plugged in and set up, so Bluedroid can start on it.
bool bluedroid_controller_ready();
#endif

}  // namespace esphome::usb_bluetooth

#endif
