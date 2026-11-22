#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)

#include "usb_bluetooth.h"
#include "esphome/core/log.h"
#include "usb/usb_helpers.h"

#include <algorithm>
#include <cstring>

#ifdef USE_USB_BLUETOOTH_BLUEDROID
#include "esphome/components/esp32_ble/ble.h"
#include "esp_bluedroid_hci.h"
#endif
#ifdef USE_USB_BLUETOOTH_A2DP
#include "esphome/core/application.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_gap_bt_api.h"
#endif
#ifdef USE_USB_BLUETOOTH_SPEAKER
#include "speaker/usb_bluetooth_speaker.h"
#endif
#ifdef USE_USB_BLUETOOTH_SPEAKER_SELECT
#include "select/usb_bluetooth_speaker_select.h"
#endif

namespace esphome::usb_bluetooth {

ESPHOME_LOG_TAG(TAG, "usb_bluetooth");

static constexpr uint32_t COMMAND_TIMEOUT_ID = 0;
static constexpr uint32_t COMMAND_TIMEOUT_MS = 3000;

static constexpr uint8_t HCI_CLASS = 0xE0;
// An HCI command is a class request to the device on endpoint 0
static constexpr uint8_t HCI_COMMAND_REQUEST_TYPE =
    usb_host::USB_DIR_OUT | usb_host::USB_TYPE_CLASS | usb_host::USB_RECIP_DEVICE;
static constexpr uint8_t HCI_EVENT_COMMAND_COMPLETE = 0x0E;

static constexpr uint16_t OP_RESET = 0x0C03;
static constexpr uint16_t OP_READ_LOCAL_NAME = 0x0C14;
static constexpr uint16_t OP_READ_LOCAL_VERSION = 0x1001;
static constexpr uint16_t OP_READ_BD_ADDR = 0x1009;
// Realtek vendor commands, as used by Linux drivers/bluetooth/btrtl.c
static constexpr uint16_t OP_RTL_DOWNLOAD = 0xFC20;
static constexpr uint16_t OP_RTL_READ_ROM_VERSION = 0xFC6D;
static constexpr size_t RTL_FRAGMENT = 252;
static constexpr uint16_t MANUFACTURER_REALTEK = 93;

#ifdef USE_USB_BLUETOOTH_BLUEDROID
// H4 packet types, the byte Bluedroid's transport puts in front of every packet
static constexpr uint8_t H4_COMMAND = 0x01;
static constexpr uint8_t H4_ACL = 0x02;
static constexpr uint8_t H4_EVENT = 0x04;
// Transfer slots the two readers hold all the time, plus one kept for the next command
static constexpr size_t RESERVED_SLOTS = 3;

// The transport operations are plain C functions without a user argument
static USBBluetooth *g_bluedroid_dongle = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

static void hci_drv_send(uint8_t *data, uint16_t len) { g_bluedroid_dongle->hci_send(data, len); }
static bool hci_drv_check_send_available() { return g_bluedroid_dongle->hci_send_available(); }
static esp_err_t hci_drv_register_host_callback(const esp_bluedroid_hci_driver_callbacks_t *callbacks) {
  g_bluedroid_dongle->hci_attach(callbacks);
  return ESP_OK;
}
static constexpr esp_bluedroid_hci_driver_operations_t HCI_OPS = {
    .send = hci_drv_send,
    .check_send_available = hci_drv_check_send_available,
    .register_host_callback = hci_drv_register_host_callback,
};

bool attach_bluedroid() {
  if (!bluedroid_controller_ready())
    return false;
  return esp_bluedroid_attach_hci_driver(&HCI_OPS) == ESP_OK;
}

void detach_bluedroid() {
  if (g_bluedroid_dongle != nullptr)
    g_bluedroid_dongle->hci_attach(nullptr);
  esp_bluedroid_detach_hci_driver();
}

bool bluedroid_controller_ready() { return g_bluedroid_dongle != nullptr && g_bluedroid_dongle->is_ready(); }

void USBBluetooth::set_bluedroid() {
  g_bluedroid_dongle = this;
  this->hci_command_.reserve(3 + 255);
}

// Bluedroid's tasks. Commands go to endpoint 0, ACL data to the bulk OUT endpoint; the H4 type byte is not sent.
void USBBluetooth::hci_send(const uint8_t *h4, uint16_t len) {
  if (len < 2)
    return;
  if (!this->is_ready()) {
    // The dongle is gone. Bluedroid still sends commands while esp32_ble takes it down and waits for
    // their answers, so they are answered here and it can finish
    if (h4[0] == H4_COMMAND && len >= 3)
      this->answer_without_controller_(encode_uint16(h4[2], h4[1]));
    return;
  }
  bool sent = false;
  auto done = [this](const usb_host::TransferStatus &status) {
    if (!status.success) {
      ESP_LOGW(TAG, "Send to the dongle failed (status %u)", status.error_code);
    }
    this->send_done_();
  };
  if (h4[0] == H4_COMMAND) {
    // Bluedroid sends one command at a time from its HCI task: one buffer, reserved once, holds it
    this->hci_command_.assign(h4 + 1, h4 + len);
    sent = this->control_transfer(HCI_COMMAND_REQUEST_TYPE, 0, 0, 0, done, this->hci_command_);
  } else if (h4[0] == H4_ACL) {
    sent = this->transfer_out(this->acl_out_ep_, done, h4 + 1, len - 1);
  }
  if (!sent) {
    ESP_LOGW(TAG, "HCI packet type %u dropped", h4[0]);
    this->send_blocked_.store(true, std::memory_order_release);
  }
}

// Commands the controller answers with a Command Status event; it answers all others with Command Complete
static constexpr uint16_t STATUS_OPCODES[] = {
    0x0401,  // Inquiry
    0x0405,  // Create Connection
    0x0406,  // Disconnect
    0x0409,  // Accept Connection Request
    0x040A,  // Reject Connection Request
    0x0411,  // Authentication Requested
    0x0413,  // Set Connection Encryption
    0x0419,  // Remote Name Request
    0x041B,  // Read Remote Supported Features
    0x041C,  // Read Remote Extended Features
    0x041D,  // Read Remote Version Information
    0x200D,  // LE Create Connection
    0x2013,  // LE Connection Update
    0x2016,  // LE Read Remote Features
    0x2019,  // LE Enable Encryption
    0x2025,  // LE Read Local P-256 Public Key
    0x2026,  // LE Generate DHKey
    0x2032,  // LE Set PHY
    0x2043,  // LE Extended Create Connection
    0x0428,  // Setup Synchronous Connection
    0x0429,  // Accept Synchronous Connection Request
    0x0803,  // Sniff Mode
    0x0804,  // Exit Sniff Mode
    0x080B,  // Switch Role
};

// Bluedroid's HCI task. A Command Complete with status success, or a Command Status with Command Disallowed for
// commands that would otherwise start something on the air, so no completion event is waited for.
void USBBluetooth::answer_without_controller_(uint16_t opcode) {
  static constexpr uint8_t STATUS_COMMAND_DISALLOWED = 0x0C;
  bool status_event = false;
  for (uint16_t op : STATUS_OPCODES)
    status_event |= op == opcode;
  // H4 type, event code, parameter length, then the parameters; Command Complete gets zeroed return
  // parameters, so a reader of a few more bytes finds them
  uint8_t event[3 + 4 + 8]{};
  event[0] = H4_EVENT;
  size_t len;
  if (status_event) {
    event[1] = 0x0F;
    event[2] = 4;
    event[3] = STATUS_COMMAND_DISALLOWED;
    event[4] = 1;
    event[5] = static_cast<uint8_t>(opcode & 0xFF);
    event[6] = static_cast<uint8_t>(opcode >> 8);
    len = 7;
  } else {
    event[1] = HCI_EVENT_COMMAND_COMPLETE;
    event[2] = 4 + 8;
    event[3] = 1;
    event[4] = static_cast<uint8_t>(opcode & 0xFF);
    event[5] = static_cast<uint8_t>(opcode >> 8);
    len = sizeof(event);
  }
  this->to_host_(event, len);
}

bool USBBluetooth::hci_send_available() const {
  const size_t used = __builtin_popcount(this->trq_in_use_.load(std::memory_order_acquire));
  if (used + RESERVED_SLOTS <= usb_host::MAX_REQUESTS)
    return true;
  this->send_blocked_.store(true, std::memory_order_release);
  return false;
}

void USBBluetooth::hci_attach(const esp_bluedroid_hci_driver_callbacks_t *callbacks) {
  this->host_ = callbacks;
  this->host_attached_.store(callbacks != nullptr, std::memory_order_release);
  if (callbacks != nullptr) {
    ESP_LOGI(TAG, "Bluedroid attached to the dongle");
  }
}

// USB task
void USBBluetooth::send_done_() {
  if (this->send_blocked_.exchange(false, std::memory_order_acq_rel) && this->host_ != nullptr)
    this->host_->notify_host_send_available();
}

// USB task. Bluedroid copies the packet before returning.
void USBBluetooth::to_host_(uint8_t *h4, size_t len) {
  const auto *host = this->host_;
  if (host != nullptr && this->host_attached_.load(std::memory_order_acquire))
    host->notify_host_recv(h4, static_cast<uint16_t>(len));
}

// An ACL packet goes to the dongle in one bulk transfer, whose buffer is USB_MAX_PACKET_SIZE bytes. The controller
// tells the host how long its ACL packets may be (BR/EDR: up to 1021 bytes for 3-DH5); that length is lowered in
// the reply before Bluedroid reads it, so Bluedroid cuts its L2CAP data into packets that fit.
static constexpr uint16_t OP_READ_BUFFER_SIZE = 0x1005;
static constexpr uint16_t OP_LE_READ_BUFFER_SIZE = 0x2002;
static constexpr uint16_t OP_LE_READ_BUFFER_SIZE_V2 = 0x2060;
static constexpr uint16_t ACL_OUT_MAX = usb_host::USB_MAX_PACKET_SIZE - 4;

static void fit_acl_length(uint8_t *event, size_t len) {
  // Command Complete: code, length, number of packets, opcode, status, then the ACL data length (both commands)
  if (event[0] != HCI_EVENT_COMMAND_COMPLETE || len < 8 || event[5] != 0)
    return;
  const uint16_t opcode = encode_uint16(event[4], event[3]);
  if (opcode != OP_READ_BUFFER_SIZE && opcode != OP_LE_READ_BUFFER_SIZE && opcode != OP_LE_READ_BUFFER_SIZE_V2)
    return;
  const uint16_t acl_len = encode_uint16(event[7], event[6]);
  if (acl_len <= ACL_OUT_MAX)
    return;
  event[6] = static_cast<uint8_t>(ACL_OUT_MAX & 0xFF);
  event[7] = static_cast<uint8_t>(ACL_OUT_MAX >> 8);
  ESP_LOGD(TAG, "ACL packets to the dongle limited to %u of %u bytes (opcode 0x%04X)", ACL_OUT_MAX, acl_len, opcode);
}

// USB task. ACL packets are put back together by their own length field, like events.
bool USBBluetooth::read_acl_(uint32_t generation) {
  const bool submitted = this->transfer_in(
      this->acl_in_ep_,
      [this, generation](const usb_host::TransferStatus &status) {
        if (generation != this->generation_.load(std::memory_order_acquire))
          return;
        if (!status.success) {
          if (status.error_code != USB_TRANSFER_STATUS_NO_DEVICE && status.error_code != USB_TRANSFER_STATUS_CANCELED) {
            ESP_LOGW(TAG, "ACL endpoint stopped (status %u)", status.error_code);
            this->events_stopped_.store(true, std::memory_order_release);
            this->enable_loop_soon_any_context();
          }
          return;
        }
        uint8_t *acl = this->acl_h4_ + 1;
        const size_t take = std::min(status.data_len, ACL_MAX - this->acl_len_);
        memcpy(acl + this->acl_len_, status.data, take);
        this->acl_len_ += take;
        const size_t len = this->acl_len_ >= 4 ? 4u + encode_uint16(acl[3], acl[2]) : SIZE_MAX;
        if (this->acl_len_ >= len || this->acl_len_ == ACL_MAX) {
          this->acl_h4_[0] = H4_ACL;
          this->to_host_(this->acl_h4_, 1 + std::min(len, this->acl_len_));
          this->acl_len_ = 0;
        } else if (this->acl_len_ > 0 && status.data_len < this->acl_in_mps_) {
          // A short packet ends the transfer: drop a broken packet and start over
          ESP_LOGW(TAG, "Dropped an incomplete ACL packet (%u bytes)", static_cast<unsigned>(this->acl_len_));
          this->acl_len_ = 0;
        }
        if (!this->read_acl_(generation)) {
          this->events_stopped_.store(true, std::memory_order_release);
          this->enable_loop_soon_any_context();
        }
      },
      this->acl_in_mps_);
  if (!submitted) {
    ESP_LOGE(TAG, "ACL read not submitted");
  }
  return submitted;
}
#endif

// Bluetooth SIG assigned numbers for the HCI version
static const char *core_version(uint8_t version) {
  static constexpr const char *NAMES[] = {"1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1",
                                          "4.2",  "5.0", "5.1", "5.2", "5.3", "5.4", "6.0", "6.1"};
  return version < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[version] : "?";
}

USBBluetooth::FindResult USBBluetooth::find_interface_() {
  const usb_config_desc_t *cfg;
  if (usb_host_get_active_config_descriptor(this->device_handle_, &cfg) != ESP_OK)
    return FindResult::FIND_RESULT_ERROR;
  this->intf_ = 0xFF;
  this->evt_ep_ = this->acl_in_ep_ = this->acl_out_ep_ = 0;
  uint8_t found = 0xFF;
  bool in_hci = false;
  int offset = 0;
  const auto *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  while ((desc = usb_parse_next_descriptor(desc, cfg->wTotalLength, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_hci = found == 0xFF && intf->bAlternateSetting == 0 && intf->bInterfaceClass == HCI_CLASS &&
               intf->bInterfaceSubClass == 0x01 && intf->bInterfaceProtocol == 0x01;
      if (in_hci)
        found = intf->bInterfaceNumber;
      continue;
    }
    if (desc->bDescriptorType != USB_W_VALUE_DT_ENDPOINT || !in_hci)
      continue;
    const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
    const uint8_t type = USB_EP_DESC_GET_XFERTYPE(ep);
    const bool in = USB_EP_DESC_GET_EP_DIR(ep);
    if (type == USB_BM_ATTRIBUTES_XFER_INT && in) {
      this->evt_ep_ = ep->bEndpointAddress;
      this->evt_mps_ = USB_EP_DESC_GET_MPS(ep);
    } else if (type == USB_BM_ATTRIBUTES_XFER_BULK && in) {
      this->acl_in_ep_ = ep->bEndpointAddress;
      this->acl_in_mps_ = USB_EP_DESC_GET_MPS(ep);
    } else if (type == USB_BM_ATTRIBUTES_XFER_BULK) {
      this->acl_out_ep_ = ep->bEndpointAddress;
      this->acl_out_mps_ = USB_EP_DESC_GET_MPS(ep);
    }
  }
  if (found == 0xFF)
    return FindResult::FIND_RESULT_NONE;
  if (this->evt_ep_ == 0 || this->acl_in_ep_ == 0 || this->acl_out_ep_ == 0 || this->evt_mps_ == 0 ||
      this->evt_mps_ > usb_host::USB_MAX_PACKET_SIZE) {
    ESP_LOGE(TAG, "Bluetooth interface %u lacks usable event or ACL endpoints", found);
    return FindResult::FIND_RESULT_ERROR;
  }
  this->intf_ = found;
  return FindResult::FIND_RESULT_FOUND;
}

void USBBluetooth::decline_() {
  const int addr = this->device_addr_;
  if (addr >= 0 && addr < 128)
    this->declined_[addr / 32] |= 1u << (addr % 32);
}

void USBBluetooth::setup() {
  usb_host::USBClient::setup();
  // The longest command is a Realtek download fragment: opcode, length, index and RTL_FRAGMENT bytes
  this->command_.reserve(3 + 1 + RTL_FRAGMENT);
}

void USBBluetooth::on_connected() {
  const uint32_t generation = this->generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  // A device that is closed here is not opened again until it is plugged in again; on_disconnected()
  // then looks at the next device
  const FindResult found = this->find_interface_();
  if (found != FindResult::FIND_RESULT_FOUND) {
    if (found == FindResult::FIND_RESULT_ERROR) {
      ESP_LOGW(TAG, "Device %d not usable; plug it in again to retry", this->device_addr_);
    } else {
      ESP_LOGD(TAG, "No Bluetooth controller interface, closing");
    }
    this->decline_();
    this->disconnect();
    return;
  }
  const esp_err_t err = usb_host_interface_claim(this->handle_, this->device_handle_, this->intf_, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Claiming interface %u failed: %s", this->intf_, esp_err_to_name(err));
    this->intf_ = 0xFF;
    this->decline_();
    this->disconnect();
    return;
  }
  ESP_LOGI(TAG, "Bluetooth controller on interface %u", this->intf_);
  this->event_len_ = 0;
  this->complete_ready_.store(false, std::memory_order_release);
  this->events_stopped_.store(false, std::memory_order_release);
  if (!this->read_event_(generation)) {
    this->finish_probe_(false);
    return;
  }
#ifdef USE_USB_BLUETOOTH_BLUEDROID
  if (g_bluedroid_dongle == this) {
    this->acl_len_ = 0;
    if (!this->read_acl_(generation)) {
      this->finish_probe_(false);
      return;
    }
  }
#endif
  this->step_ = ProbeStep::PROBE_STEP_RESET;
  this->send_command_(OP_RESET, nullptr, 0);
}

void USBBluetooth::on_disconnected() {
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  this->cancel_timeout(COMMAND_TIMEOUT_ID);
  this->pending_opcode_.store(0, std::memory_order_release);
  this->complete_ready_.store(false, std::memory_order_release);
  this->step_ = ProbeStep::PROBE_STEP_IDLE;
  this->download_ = nullptr;
  memset(this->address_, 0, sizeof(this->address_));
  if (this->intf_ != 0xFF) {
    // Stop and cancel the transfers still queued on the interface's endpoints; it cannot be released before
    for (const uint8_t ep : {this->evt_ep_, this->acl_in_ep_, this->acl_out_ep_}) {
      if (ep == 0)
        continue;
      usb_host_endpoint_halt(this->device_handle_, ep);
      usb_host_endpoint_flush(this->device_handle_, ep);
    }
    const esp_err_t err = usb_host_interface_release(this->handle_, this->device_handle_, this->intf_);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Releasing interface %u failed: %s", this->intf_, esp_err_to_name(err));
    }
    this->intf_ = 0xFF;
    ESP_LOGI(TAG, "Bluetooth controller removed");
  } else {
    // Closed without being claimed (not a Bluetooth dongle, or refused by the vid/pid filter). The
    // host library announces a device once, so one plugged in together with this one (behind a hub)
    // was not offered to this client while it was busy with this one. Deferred: the device is still
    // open here.
    this->defer([this]() { this->try_next_device_(); });
  }
#ifdef USE_USB_BLUETOOTH_BLUEDROID
  // Bluedroid reads its controller once, when it starts: take it down now and bring it back on the next dongle
  // Bluedroid stays attached until esp32_ble has taken it down; answer_without_controller_() answers it meanwhile
  if (this->host_attached_.load(std::memory_order_acquire) && !this->restart_host_) {
    this->restart_host_ = true;
    esp32_ble::global_ble->disable();
  }
#endif
  usb_host::USBClient::on_disconnected();
}

// USB task. One read is one wMaxPacketSize packet; events are put back together by their own length
// field, because an event that is a whole number of packets long ends without a short packet.
bool USBBluetooth::read_event_(uint32_t generation) {
  const bool submitted = this->transfer_in(
      this->evt_ep_,
      [this, generation](const usb_host::TransferStatus &status) {
        if (generation != this->generation_.load(std::memory_order_acquire))
          return;
        if (!status.success) {
          // A removed or closed device is handled by the disconnect
          if (status.error_code != USB_TRANSFER_STATUS_NO_DEVICE && status.error_code != USB_TRANSFER_STATUS_CANCELED) {
            ESP_LOGW(TAG, "Event endpoint stopped (status %u)", status.error_code);
            this->events_stopped_.store(true, std::memory_order_release);
            this->enable_loop_soon_any_context();
          }
          return;
        }
        const size_t take = std::min(status.data_len, EVENT_MAX - this->event_len_);
        memcpy(this->event_ + this->event_len_, status.data, take);
        this->event_len_ += take;
        if (this->event_len_ >= 2 && this->event_len_ >= 2u + this->event_[1]) {
          const size_t len = 2u + this->event_[1];
          this->event_len_ = 0;
          bool for_host = false;
#ifdef USE_USB_BLUETOOTH_BLUEDROID
          for_host = this->host_attached_.load(std::memory_order_acquire);
          if (for_host) {
            fit_acl_length(this->event_, len);
            this->event_h4_[0] = H4_EVENT;
            this->to_host_(this->event_h4_, len + 1);
          }
#endif
          // Command Complete: code, length, number of packets, opcode, status, return parameters
          const uint16_t pending = this->pending_opcode_.load(std::memory_order_acquire);
          if (!for_host && this->event_[0] == HCI_EVENT_COMMAND_COMPLETE && len >= 6 && pending != 0 &&
              encode_uint16(this->event_[4], this->event_[3]) == pending &&
              !this->complete_ready_.load(std::memory_order_acquire)) {
            memcpy(this->complete_, this->event_, len);
            this->complete_len_ = len;
            this->complete_ready_.store(true, std::memory_order_release);
            this->enable_loop_soon_any_context();
          }
        } else if (this->event_len_ > 0 && (status.data_len < this->evt_mps_ || this->event_len_ == EVENT_MAX)) {
          // Each event starts a transfer, so a short packet ends it: drop a broken one and start over
          ESP_LOGW(TAG, "Dropped an incomplete HCI event (%u bytes)", static_cast<unsigned>(this->event_len_));
          this->event_len_ = 0;
        }
        if (!this->read_event_(generation)) {
          this->events_stopped_.store(true, std::memory_order_release);
          this->enable_loop_soon_any_context();
        }
      },
      this->evt_mps_);
  if (!submitted) {
    ESP_LOGE(TAG, "Event read not submitted");
  }
  return submitted;
}

void USBBluetooth::try_next_device_() {
  uint8_t addrs[32];
  int count = 0;
  if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &count) != ESP_OK)
    return;
  // Addresses are reused: forget declined devices that are gone
  uint32_t present[4]{};
  for (int i = 0; i < count; i++) {
    if (addrs[i] < 128)
      present[addrs[i] / 32] |= 1u << (addrs[i] % 32);
  }
  for (size_t i = 0; i < 4; i++)
    this->declined_[i] &= present[i];
  for (int i = 0; i < count; i++) {
    const uint8_t addr = addrs[i];
    if (addr < 128 && (this->declined_[addr / 32] & (1u << (addr % 32))) == 0) {
      // Each device is looked at once, also when the vid/pid filter refuses it
      this->declined_[addr / 32] |= 1u << (addr % 32);
      this->on_opened(addr);
      return;
    }
  }
}

void USBBluetooth::send_command_(uint16_t opcode, const uint8_t *params, size_t len) {
  // Reserved in setup(): assign() reuses the buffer
  this->command_.assign(3, 0);
  this->command_[0] = static_cast<uint8_t>(opcode & 0xFF);
  this->command_[1] = static_cast<uint8_t>(opcode >> 8);
  this->command_[2] = static_cast<uint8_t>(len);
  this->command_.insert(this->command_.end(), params, params + len);
  this->pending_opcode_.store(opcode, std::memory_order_release);
  // A device-recipient class request: wIndex is 0 (Bluetooth Core, USB transport)
  const bool sent = this->control_transfer(
      HCI_COMMAND_REQUEST_TYPE, 0, 0, 0,
      [opcode](const usb_host::TransferStatus &status) {
        if (!status.success) {
          ESP_LOGW(TAG, "HCI command 0x%04X not taken (status %u)", opcode, status.error_code);
        }
      },
      this->command_);
  if (!sent) {
    ESP_LOGE(TAG, "HCI command 0x%04X not submitted", opcode);
    this->finish_probe_(false);
    return;
  }
  this->set_timeout(COMMAND_TIMEOUT_ID, COMMAND_TIMEOUT_MS, [this]() {
    ESP_LOGE(TAG, "No answer to HCI command 0x%04X", this->pending_opcode_.load(std::memory_order_acquire));
    this->finish_probe_(false);
  });
}

void USBBluetooth::loop() {
  bool work = this->process_usb_events_();
  // After process_usb_events_(): a stop caused by removing the dongle is then already a disconnect
  if (this->events_stopped_.exchange(false, std::memory_order_acq_rel) && this->intf_ != 0xFF &&
      this->step_ != ProbeStep::PROBE_STEP_FAILED) {
    ESP_LOGE(TAG, "The controller stopped sending events or data; plug it in again");
    this->finish_probe_(false);
  }
  if (this->complete_ready_.load(std::memory_order_acquire)) {
    work = true;
    this->cancel_timeout(COMMAND_TIMEOUT_ID);
    this->pending_opcode_.store(0, std::memory_order_release);
    const uint8_t status = this->complete_[5];
    const uint16_t opcode = encode_uint16(this->complete_[4], this->complete_[3]);
    // Copy out before the flag lets the USB task write the buffer again
    uint8_t ret[EVENT_MAX];
    const size_t len = this->complete_len_ - 6;
    memcpy(ret, this->complete_ + 6, len);
    this->complete_ready_.store(false, std::memory_order_release);
    if (status != 0) {
      ESP_LOGE(TAG, "HCI command 0x%04X failed, status 0x%02X", opcode, status);
      this->finish_probe_(false);
    } else {
      this->handle_command_complete_(ret, len);
    }
  }
  if (!work)
    this->disable_loop();
}

void USBBluetooth::handle_command_complete_(const uint8_t *ret, size_t len) {
  switch (this->step_) {
    case ProbeStep::PROBE_STEP_RESET:
      this->step_ = ProbeStep::PROBE_STEP_READ_VERSION;
      this->send_command_(OP_READ_LOCAL_VERSION, nullptr, 0);
      return;

    case ProbeStep::PROBE_STEP_READ_VERSION:
    case ProbeStep::PROBE_STEP_READ_VERSION_PATCHED: {
      // HCI version, HCI revision, LMP version, manufacturer, LMP subversion
      if (len < 8) {
        this->finish_probe_(false);
        return;
      }
      this->hci_version_ = ret[0];
      this->hci_revision_ = encode_uint16(ret[2], ret[1]);
      this->manufacturer_ = encode_uint16(ret[5], ret[4]);
      this->lmp_subver_ = encode_uint16(ret[7], ret[6]);
      if (this->step_ == ProbeStep::PROBE_STEP_READ_VERSION_PATCHED) {
        if (this->lmp_subver_ == this->rom_lmp_subver_) {
          // Linux btrtl does not check this either; the controller may still work
          ESP_LOGW(TAG, "Controller still reports its ROM version after the download (LMP subversion 0x%04X)",
                   this->lmp_subver_);
        } else {
          ESP_LOGI(TAG, "Firmware loaded: revision 0x%04X, LMP subversion 0x%04X", this->hci_revision_,
                   this->lmp_subver_);
        }
      } else if (this->manufacturer_ == MANUFACTURER_REALTEK) {
        // A controller on its ROM reports the lmp_subver of its chip, a patched one that of its firmware
        bool for_chip = false;
        for (const auto &image : this->realtek_images_)
          for_chip |= image.lmp_subver == this->lmp_subver_;
        if (for_chip) {
          this->rom_lmp_subver_ = this->lmp_subver_;
          this->step_ = ProbeStep::PROBE_STEP_RTL_READ_ROM_VERSION;
          this->send_command_(OP_RTL_READ_ROM_VERSION, nullptr, 0);
          return;
        }
        if (this->realtek_images_.empty()) {
          ESP_LOGW(TAG, "Realtek controller without firmware configured: on its ROM it does not work on the air");
        } else {
          // Already patched (after a reboot of the ESP), or firmware for another chip
          ESP_LOGI(TAG, "No firmware for LMP subversion 0x%04X (configured: 0x%04X); treated as patched",
                   this->lmp_subver_, this->realtek_images_[0].lmp_subver);
        }
      }
      this->step_ = ProbeStep::PROBE_STEP_READ_ADDRESS;
      this->send_command_(OP_READ_BD_ADDR, nullptr, 0);
      return;
    }

    case ProbeStep::PROBE_STEP_RTL_READ_ROM_VERSION: {
      if (len < 1) {
        this->finish_probe_(false);
        return;
      }
      this->download_ = nullptr;
      for (const auto &image : this->realtek_images_) {
        if (image.lmp_subver == this->lmp_subver_ && image.rom_version == ret[0])
          this->download_ = &image;
      }
      if (this->download_ == nullptr) {
        // On its ROM the controller does not work on the air
        ESP_LOGE(TAG, "The firmware has no patch for ROM version %u", ret[0]);
        this->finish_probe_(false);
        return;
      }
      ESP_LOGD(TAG, "Realtek ROM version %u: downloading %u bytes", ret[0],
               static_cast<unsigned>(this->download_->length));
      this->step_ = ProbeStep::PROBE_STEP_RTL_DOWNLOAD;
      this->fragment_ = 0;
      this->fragment_count_ = this->download_->length / RTL_FRAGMENT + 1;
      this->next_fragment_();
      return;
    }

    case ProbeStep::PROBE_STEP_RTL_DOWNLOAD:
      // The return parameters carry the index of the fragment taken
      if (len < 1) {
        ESP_LOGE(TAG, "Realtek download fragment %u not acknowledged", static_cast<unsigned>(this->fragment_));
        this->finish_probe_(false);
        return;
      }
      if (++this->fragment_ < this->fragment_count_) {
        this->next_fragment_();
        return;
      }
      this->download_ = nullptr;
      this->step_ = ProbeStep::PROBE_STEP_READ_VERSION_PATCHED;
      this->send_command_(OP_READ_LOCAL_VERSION, nullptr, 0);
      return;

    case ProbeStep::PROBE_STEP_READ_ADDRESS:
      if (len < 6) {
        this->finish_probe_(false);
        return;
      }
      for (size_t i = 0; i < 6; i++)
        this->address_[i] = ret[5 - i];
      this->step_ = ProbeStep::PROBE_STEP_READ_NAME;
      this->send_command_(OP_READ_LOCAL_NAME, nullptr, 0);
      return;

    case ProbeStep::PROBE_STEP_READ_NAME: {
      // The name is NUL terminated, but some controllers (Realtek) pad it with 0xFF instead
      size_t name_len = 0;
      while (name_len < len && ret[name_len] != 0x00 && ret[name_len] != 0xFF)
        name_len++;
      ESP_LOGI(TAG,
               "Controller ready: Bluetooth %s, manufacturer %u, address %02X:%02X:%02X:%02X:%02X:%02X, name '%.*s'",
               core_version(this->hci_version_), this->manufacturer_, this->address_[0], this->address_[1],
               this->address_[2], this->address_[3], this->address_[4], this->address_[5], static_cast<int>(name_len),
               reinterpret_cast<const char *>(ret));
      this->finish_probe_(true);
      return;
    }

    case ProbeStep::PROBE_STEP_IDLE:
    case ProbeStep::PROBE_STEP_READY:
    case ProbeStep::PROBE_STEP_FAILED:
      return;
  }
}

// Fragments of RTL_FRAGMENT bytes, the last one flagged 0x80 and carrying the remainder; the
// index runs 0..0x7F and then wraps to 1, as rtl_download_firmware() in btrtl.c does.
void USBBluetooth::next_fragment_() {
  const uint32_t i = this->fragment_;
  uint8_t index = i <= 0x7F ? static_cast<uint8_t>(i) : static_cast<uint8_t>((i - 0x80) % 0x7F + 1);
  size_t take = RTL_FRAGMENT;
  if (i + 1 == this->fragment_count_) {
    index |= 0x80;
    take = this->download_->length % RTL_FRAGMENT;
  }
  uint8_t params[1 + RTL_FRAGMENT];
  params[0] = index;
  memcpy(params + 1, this->download_->data + i * RTL_FRAGMENT, take);
  this->send_command_(OP_RTL_DOWNLOAD, params, 1 + take);
}

void USBBluetooth::finish_probe_(bool ok) {
  this->cancel_timeout(COMMAND_TIMEOUT_ID);
  this->pending_opcode_.store(0, std::memory_order_release);
  this->download_ = nullptr;
  this->step_ = ok ? ProbeStep::PROBE_STEP_READY : ProbeStep::PROBE_STEP_FAILED;
  if (ok) {
    this->status_clear_warning();
#ifdef USE_USB_BLUETOOTH_BLUEDROID
    if (this->restart_host_) {
      this->restart_host_ = false;
      esp32_ble::global_ble->enable();
    }
#endif
    this->report_connected_();
  } else {
    // Not reported as connected: is_ready() stays false until the dongle is plugged in again
    this->status_set_warning(LOG_STR("Bluetooth controller did not start"));
  }
}

#ifdef USE_USB_BLUETOOTH_A2DP
static constexpr uint32_t A2DP_POLL_ID = 1;
static constexpr uint32_t A2DP_POLL_MS = 1000;
// AVRCP transaction labels, one per kind of command
static constexpr uint8_t TL_CAPS = 0;
static constexpr uint8_t TL_VOLUME_NOTIFY = 1;
static constexpr uint8_t TL_SET_VOLUME = 2;

// Bluedroid's callbacks have no user pointer; one dongle runs Bluedroid
static USBBluetooth *g_a2dp = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Bluedroid's BTC task
static void a2dp_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_CFM_REQ_EVT:
      // Secure Simple Pairing, Just Works: a speaker has no display to compare a number on
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
      // Legacy pairing: the usual fixed PIN of speakers and headsets
      esp_bt_pin_code_t pin{'0', '0', '0', '0'};
      esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
      break;
    }
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
      if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS && g_a2dp != nullptr)
        g_a2dp->speaker_name_(param->read_rmt_name.bda, reinterpret_cast<const char *>(param->read_rmt_name.rmt_name));
      break;
    case ESP_BT_GAP_DISC_RES_EVT: {
      if (g_a2dp == nullptr)
        break;
      uint32_t cod = 0;
      char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = "";
      for (int i = 0; i < param->disc_res.num_prop; i++) {
        const esp_bt_gap_dev_prop_t &prop = param->disc_res.prop[i];
        if (prop.type == ESP_BT_GAP_DEV_PROP_COD) {
          cod = *static_cast<uint32_t *>(prop.val);
        } else if (prop.type == ESP_BT_GAP_DEV_PROP_BDNAME && name[0] == 0) {
          snprintf(name, sizeof(name), "%.*s", prop.len, static_cast<const char *>(prop.val));
        } else if (prop.type == ESP_BT_GAP_DEV_PROP_EIR && name[0] == 0) {
          uint8_t len = 0;
          auto *eir = static_cast<uint8_t *>(prop.val);
          const uint8_t *found = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
          if (found == nullptr)
            found = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
          if (found != nullptr)
            snprintf(name, sizeof(name), "%.*s", len, reinterpret_cast<const char *>(found));
        }
      }
      g_a2dp->found_speaker_(param->disc_res.bda, cod, name);
      break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      if (g_a2dp != nullptr)
        g_a2dp->scan_state_(param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
      break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Paired with '%s'", reinterpret_cast<const char *>(param->auth_cmpl.device_name));
      } else {
        ESP_LOGW(TAG, "Pairing failed, status %d", param->auth_cmpl.stat);
      }
      break;
    default:
      break;
  }
}

// A2DP source task: fill buf with 16-bit stereo PCM at 44.1 kHz
static int32_t a2dp_data(uint8_t *buf, int32_t len) {
  if (buf == nullptr || len <= 0 || g_a2dp == nullptr)
    return 0;
  g_a2dp->a2dp_fill_(reinterpret_cast<int16_t *>(buf), static_cast<size_t>(len) / 4);
  return len;
}

void USBBluetooth::a2dp_fill_(int16_t *frames, size_t count) {
#ifdef USE_USB_BLUETOOTH_SPEAKER
  // Taken out of the buffer also while muted, so playing goes on
  if (this->a2dp_source_ != nullptr) {
    this->a2dp_source_->fill(frames, count);
  } else
#endif
  {
    memset(frames, 0, count * 4);
  }
  if (this->a2dp_mute_.load(std::memory_order_acquire)) {
    memset(frames, 0, count * 4);
  } else if (this->a2dp_gain_ < 0.999f) {
    // A speaker without absolute volume: scale the samples
    a2dp_scale(frames, 2 * count, this->a2dp_gain_);
  }
}

// Bluedroid's BTC task: only records what happened, the main loop acts on it
static void a2dp_event(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  if (g_a2dp == nullptr)
    return;
  if (event == ESP_A2D_CONNECTION_STATE_EVT) {
    g_a2dp->a2dp_connection_(param->conn_stat.state, param->conn_stat.remote_bda);
  } else if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
    g_a2dp->a2dp_media_ack_(param->media_ctrl_stat.cmd, param->media_ctrl_stat.status);
  }
}

void USBBluetooth::a2dp_connection_(int state, const uint8_t *address) {
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    ESP_LOGI(TAG, "Speaker %02X:%02X:%02X:%02X:%02X:%02X connected", address[0], address[1], address[2], address[3],
             address[4], address[5]);
    // A speaker chosen earlier can connect by itself; only the chosen one stays
    if (memcmp(address, this->a2dp_peer_, sizeof(this->a2dp_peer_)) != 0) {
      ESP_LOGI(TAG, "Not the chosen speaker; disconnecting");
      esp_a2d_source_disconnect(const_cast<uint8_t *>(address));
      return;
    }
    this->a2dp_state_.store(A2DP_CONNECTED, std::memory_order_release);
    this->speaker_name_(address, "");
    // The name arrives as ESP_BT_GAP_READ_REMOTE_NAME_EVT
    esp_bt_gap_read_remote_name(const_cast<uint8_t *>(address));
  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    ESP_LOGI(TAG, "Speaker disconnected");
    this->a2dp_state_.store(A2DP_IDLE, std::memory_order_release);
#ifdef USE_TEXT_SENSOR
    this->speaker_changed_.store(true, std::memory_order_release);
#endif
  }
}

// Bluedroid's BTC task
void USBBluetooth::speaker_name_(const uint8_t *address, const char *name) {
  if (name[0] != 0) {
    ESP_LOGI(TAG, "Speaker name: '%s'", name);
  }
#ifdef USE_TEXT_SENSOR
  {
    LockGuard guard(this->speaker_lock_);
    memcpy(this->speaker_address_, address, sizeof(this->speaker_address_));
    snprintf(this->speaker_name_buf_, sizeof(this->speaker_name_buf_), "%s", name);
  }
  this->speaker_changed_.store(true, std::memory_order_release);
#endif
}

#ifdef USE_TEXT_SENSOR
// Main loop: the name and address of the connected speaker, or empty while none is connected
void USBBluetooth::publish_connected_speaker_() {
  if (this->connected_speaker_text_sensor_ == nullptr || !this->speaker_changed_.exchange(false))
    return;
  char text[sizeof(this->speaker_name_buf_) + 24] = "";
  if (this->a2dp_connected()) {
    LockGuard guard(this->speaker_lock_);
    format_speaker(text, sizeof(text), this->speaker_name_buf_, this->speaker_address_);
  }
  this->connected_speaker_text_sensor_->publish_state(text);
}
#endif

void USBBluetooth::a2dp_media_ack_(int cmd, int status) {
  const bool ok = status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS;
  if (cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
    if (ok)
      this->a2dp_state_.store(A2DP_READY, std::memory_order_release);
  } else if (cmd == ESP_A2D_MEDIA_CTRL_START) {
    if (!ok) {
      ESP_LOGW(TAG, "The speaker did not start the stream");
    }
    this->a2dp_state_.store(ok ? A2DP_STREAMING : A2DP_CONNECTED, std::memory_order_release);
  } else if (cmd == ESP_A2D_MEDIA_CTRL_SUSPEND && ok) {
    this->a2dp_state_.store(A2DP_SUSPENDED, std::memory_order_release);
  }
}

// Bluedroid's BTC task
static void avrc_ct_event(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
  if (g_a2dp == nullptr)
    return;
  switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
      g_a2dp->avrc_connected_(param->conn_stat.connected);
      if (param->conn_stat.connected)
        esp_avrc_ct_send_get_rn_capabilities_cmd(TL_CAPS);
      break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
      g_a2dp->avrc_absolute_volume_(esp_avrc_rn_evt_bit_mask_operation(
          ESP_AVRC_BIT_MASK_OP_TEST, &param->get_rn_caps_rsp.evt_set, ESP_AVRC_RN_VOLUME_CHANGE));
      break;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
      if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        ESP_LOGD(TAG, "Speaker volume %u of 127", param->change_ntf.event_parameter.volume);
        // A notification is sent once per registration
        esp_avrc_ct_send_register_notification_cmd(TL_VOLUME_NOTIFY, ESP_AVRC_RN_VOLUME_CHANGE, 0);
      }
      break;
    default:
      break;
  }
}

void USBBluetooth::avrc_connected_(bool connected) {
  if (!connected) {
    this->avrc_abs_volume_.store(false, std::memory_order_release);
    this->a2dp_gain_ = this->volume_;
  }
}

void USBBluetooth::avrc_absolute_volume_(bool supported) {
  ESP_LOGD(TAG, "Speaker %s absolute volume", supported ? LOG_STR_LITERAL("supports") : LOG_STR_LITERAL("lacks"));
  this->avrc_abs_volume_.store(supported, std::memory_order_release);
  // With absolute volume the speaker sets the level; otherwise the samples are scaled
  this->a2dp_gain_ = supported ? 1.0f : this->volume_;
  if (supported) {
    esp_avrc_ct_send_register_notification_cmd(TL_VOLUME_NOTIFY, ESP_AVRC_RN_VOLUME_CHANGE, 0);
    // Before anything plays, set the speaker to the current level
    esp_avrc_ct_send_set_absolute_volume_cmd(TL_SET_VOLUME, avrc_volume(this->volume_));
  }
}

void USBBluetooth::set_volume(float level) {
  this->volume_ = std::clamp(level, 0.0f, 1.0f);
  if (this->avrc_abs_volume_.load(std::memory_order_acquire)) {
    esp_avrc_ct_send_set_absolute_volume_cmd(TL_SET_VOLUME, avrc_volume(this->volume_));
  } else {
    this->a2dp_gain_ = this->volume_;
  }
}

void USBBluetooth::enable_a2dp() {
  g_a2dp = this;
  this->set_interval(A2DP_POLL_ID, A2DP_POLL_MS, [this]() { this->a2dp_poll_(); });
}

void USBBluetooth::set_a2dp_speaker(uint64_t address) {
  for (int i = 0; i < 6; i++)
    this->a2dp_peer_[i] = static_cast<uint8_t>(address >> (8 * (5 - i)));
  this->enable_a2dp();
}

void USBBluetooth::set_a2dp_peer(const uint8_t *address) {
  if (memcmp(address, this->a2dp_peer_, sizeof(this->a2dp_peer_)) == 0)
    return;
  uint8_t previous[6];
  memcpy(previous, this->a2dp_peer_, sizeof(previous));
  memcpy(this->a2dp_peer_, address, sizeof(this->a2dp_peer_));
  this->a2dp_last_try_ = 0;
  ESP_LOGI(TAG, "Speaker set to %02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1], address[2], address[3],
           address[4], address[5]);
  // The disconnect event sets A2DP_IDLE, and the next poll connects to the new speaker
  if (this->a2dp_up_ && this->a2dp_state_.load(std::memory_order_acquire) != A2DP_IDLE)
    esp_a2d_source_disconnect(previous);
}

void USBBluetooth::scan_speakers() {
  if (!this->a2dp_up_) {
    ESP_LOGW(TAG, "Bluetooth is not running; cannot search for speakers");
    return;
  }
  {
    LockGuard guard(this->found_lock_);
    this->found_count_ = 0;
  }
  this->found_changed_.store(true, std::memory_order_release);
  // 10 x 1.28 s, the inquiry length of the Bluetooth specification for a general search
  esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_bt_gap_start_discovery failed: %s", esp_err_to_name(err));
    return;
  }
  // Set here too, so the select shows the search before Bluedroid reports it
  this->scanning_.store(true, std::memory_order_release);
}

uint8_t USBBluetooth::found_speakers(FoundSpeaker *out) {
  LockGuard guard(this->found_lock_);
  memcpy(out, this->found_, this->found_count_ * sizeof(FoundSpeaker));
  return this->found_count_;
}

// Bluedroid's BTC task
void USBBluetooth::found_speaker_(const uint8_t *address, uint32_t cod, const char *name) {
  if (!is_audio_device(cod))
    return;
  bool changed;
  {
    LockGuard guard(this->found_lock_);
    changed = add_found_speaker(this->found_, this->found_count_, MAX_FOUND_SPEAKERS, address, name);
  }
  if (changed) {
    ESP_LOGD(TAG, "Found %02X:%02X:%02X:%02X:%02X:%02X '%s'", address[0], address[1], address[2], address[3],
             address[4], address[5], name);
    this->found_changed_.store(true, std::memory_order_release);
  }
}

// Bluedroid's BTC task
void USBBluetooth::scan_state_(bool running) {
  this->scanning_.store(running, std::memory_order_release);
  this->found_changed_.store(true, std::memory_order_release);
}

// Main loop. Bluedroid forgets the Classic callbacks and profiles when esp32_ble takes it down (dongle unplugged,
// ble.disable), so they are set up again each time esp32_ble becomes active.
bool USBBluetooth::a2dp_setup_() {
  esp_err_t err = esp_bt_gap_register_callback(a2dp_gap_event);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_bt_gap_register_callback failed: %s", esp_err_to_name(err));
    return false;
  }
  esp_bt_gap_set_device_name(App.get_name().c_str());
  // No display or keyboard: Just Works pairing
  esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
  // AVRCP for the speaker's volume (absolute volume, AVRCP 1.4); initialised before A2DP, as in the ESP-IDF example
  esp_avrc_ct_register_callback(avrc_ct_event);
  esp_avrc_ct_init();
  esp_a2d_register_callback(a2dp_event);
  esp_a2d_source_register_data_callback(a2dp_data);
  err = esp_a2d_source_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_a2d_source_init failed: %s", esp_err_to_name(err));
    return false;
  }
  // Connectable, so the speaker can connect back when it is switched on; not discoverable
  err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_bt_gap_set_scan_mode failed: %s", esp_err_to_name(err));
  }
  return true;
}

// Main loop
void USBBluetooth::a2dp_poll_() {
#ifdef USE_TEXT_SENSOR
  this->publish_connected_speaker_();
#endif
#ifdef USE_USB_BLUETOOTH_SPEAKER_SELECT
  if (this->found_changed_.exchange(false, std::memory_order_acq_rel) && this->speaker_select_ != nullptr)
    this->speaker_select_->update_options();
#endif
  if (esp32_ble::global_ble == nullptr || !esp32_ble::global_ble->is_active()) {
#ifdef USE_TEXT_SENSOR
    // Bluetooth stopped (dongle unplugged): no disconnect event follows
    if (this->a2dp_state_.load(std::memory_order_acquire) != A2DP_IDLE)
      this->speaker_changed_.store(true, std::memory_order_release);
#endif
    this->a2dp_up_ = false;
    this->scanning_.store(false, std::memory_order_release);
    this->a2dp_state_.store(A2DP_IDLE, std::memory_order_release);
    return;
  }
  if (!this->a2dp_up_) {
    this->a2dp_up_ = this->a2dp_setup_();
    this->a2dp_last_try_ = 0;
    return;
  }
  const uint32_t now = millis();
  bool wanted = false;
#ifdef USE_USB_BLUETOOTH_SPEAKER
  wanted = this->a2dp_source_ != nullptr && this->a2dp_source_->wants_stream();
#endif
  if (wanted)
    this->a2dp_last_wanted_ = now;
  static constexpr uint8_t NO_PEER[6]{};
  // A connection attempt during a search fails, as the radio is busy with the search
  const bool may_connect = !this->scanning() && memcmp(this->a2dp_peer_, NO_PEER, sizeof(this->a2dp_peer_)) != 0;
  const A2dpState state = this->a2dp_state_.load(std::memory_order_acquire);
  const A2dpStep step = a2dp_step(state, wanted, may_connect, now, this->a2dp_last_try_, this->a2dp_last_wanted_);
  switch (step.action) {
    case A2dpAction::A2DP_ACTION_CONNECT:
      this->a2dp_last_try_ = now == 0 ? 1 : now;
      if (esp_a2d_source_connect(this->a2dp_peer_) != ESP_OK)
        return;
      break;
    case A2dpAction::A2DP_ACTION_CHECK_READY:
      esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
      break;
    case A2dpAction::A2DP_ACTION_START:
      esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
      break;
    case A2dpAction::A2DP_ACTION_SUSPEND:
      esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
      break;
    case A2dpAction::A2DP_ACTION_NONE:
      break;
  }
  // Bluedroid's task may have moved the state on meanwhile; its state wins
  A2dpState expected = state;
  if (step.state != state)
    this->a2dp_state_.compare_exchange_strong(expected, step.state, std::memory_order_acq_rel);
}
#endif

void USBBluetooth::dump_config() {
  usb_host::USBClient::dump_config();
  ESP_LOGCONFIG(TAG, "  Realtek firmware images: %u", static_cast<unsigned>(this->realtek_images_.size()));
#ifdef USE_USB_BLUETOOTH_A2DP
  ESP_LOGCONFIG(TAG, "  A2DP speaker: %02X:%02X:%02X:%02X:%02X:%02X", this->a2dp_peer_[0], this->a2dp_peer_[1],
                this->a2dp_peer_[2], this->a2dp_peer_[3], this->a2dp_peer_[4], this->a2dp_peer_[5]);
#endif
}

}  // namespace esphome::usb_bluetooth

#endif
