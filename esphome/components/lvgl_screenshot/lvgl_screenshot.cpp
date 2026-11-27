#include "lvgl_screenshot.h"

#if defined(USE_ESP32) && defined(USE_LVGL_SCREENSHOT)

#include <cinttypes>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::lvgl_screenshot {

ESPHOME_LOG_TAG(TAG, "lvgl_screenshot");

/// The esp32-camera software encoder uses about 2.3 kB of it.
static constexpr uint32_t ENCODE_TASK_STACK_SIZE = 6144;
/// The same as the main loop; on a dual-core chip the task mostly runs on the other core.
static constexpr UBaseType_t ENCODE_TASK_PRIORITY = 1;
/// Each retry follows a larger output buffer, so this bounds how often a screenshot is encoded.
static constexpr uint8_t MAX_ENCODE_ATTEMPTS = 8;

void LvglScreenshot::setup() {
  lv_display_t *display = lv_display_get_default();
  if (display == nullptr) {
    this->mark_failed(LOG_STR("No LVGL display"));
    return;
  }
  this->width_ = lv_display_get_horizontal_resolution(display);
  this->height_ = lv_display_get_vertical_resolution(display);
  const uint32_t stride = this->width_ * 2;
  this->raw_size_ = stride * this->height_;
  RAMAllocator<uint8_t> allocator;
  this->raw_ = allocator.allocate(this->raw_size_);
  if (this->raw_ == nullptr) {
    this->mark_failed(LOG_STR("Failed to allocate the screenshot buffer"));
    return;
  }
  lv_draw_buf_init(&this->draw_buf_, this->width_, this->height_, LV_COLOR_FORMAT_RGB565, stride, this->raw_,
                   this->raw_size_);
  this->reusable_image_ = std::make_shared<ScreenshotImage>();
  if (xTaskCreate(LvglScreenshot::encode_task, "lvgl_screenshot", ENCODE_TASK_STACK_SIZE, this, ENCODE_TASK_PRIORITY,
                  &this->task_) != pdPASS) {
    this->mark_failed(LOG_STR("Failed to start the encoding task"));
    return;
  }
  this->disable_loop();
}

void LvglScreenshot::dump_config() {
  ESP_LOGCONFIG(TAG,
                "LVGL Screenshot:\n"
                "  Size: %ux%u\n"
                "  Stream interval: %" PRIu32 " ms",
                this->width_, this->height_, this->stream_interval_);
  // The encoder is not a component, so nothing else logs its settings.
  this->encoder_->dump_config();
}

void LvglScreenshot::loop() {
  const CaptureState state = this->state_.load(std::memory_order_acquire);
  if (state == CaptureState::CAPTURE_STATE_BUSY)
    return;
  if (state == CaptureState::CAPTURE_STATE_DONE) {
    this->publish_();
    this->state_.store(CaptureState::CAPTURE_STATE_IDLE, std::memory_order_release);
  } else if (state == CaptureState::CAPTURE_STATE_FAILED) {
    this->state_.store(CaptureState::CAPTURE_STATE_IDLE, std::memory_order_release);
    ESP_LOGW(TAG, "Encoding failed: %s", camera::to_string(this->encode_result_));
    if (this->encode_result_ == camera::ENCODER_ERROR_CONFIGURATION) {
      // Stops further screenshots; see the check below.
      this->status_set_error(LOG_STR("The encoder cannot encode the screen"));
    } else {
      this->status_set_warning(LOG_STR("Encoding failed"));
    }
  }

  // Release the previous screenshot once every client has finished reading it, which frees the
  // encoder's output buffer for the next one.
  if (this->current_image_ && this->current_image_.use_count() == 2)
    this->current_image_.reset();

  const uint32_t now = App.get_loop_component_start_time();
  const bool stream_due = this->stream_requesters_ != 0 && now - this->last_stream_frame_ >= this->stream_interval_;
  const uint8_t requesters = this->single_requesters_ | (stream_due ? this->stream_requesters_ : 0);
  if (requesters == 0 || this->status_has_error()) {
    if (!this->current_image_ && this->stream_requesters_ == 0)
      this->disable_loop();
    return;
  }
  if (this->current_image_)
    return;

  this->single_requesters_ = 0;
  if (stream_due)
    this->last_stream_frame_ = now;
  if (!this->render_())
    return;
  this->pending_requesters_ = requesters;
  this->state_.store(CaptureState::CAPTURE_STATE_BUSY, std::memory_order_release);
  xTaskNotifyGive(this->task_);
}

bool LvglScreenshot::render_() {
  // LVGL is not thread-safe, so this stays in the main loop.
  const uint32_t start = millis();
#if LV_USE_SNAPSHOT  // Set in lv_conf.h for this component; static analysis builds LVGL without it.
  const lv_result_t result = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &this->draw_buf_);
#else
  const lv_result_t result = LV_RESULT_INVALID;
#endif
  this->render_ms_ = millis() - start;
  // The encoder takes the buffer as one block of rows, so the screen must fill it exactly.
  if (result != LV_RESULT_OK || this->draw_buf_.header.w != this->width_ || this->draw_buf_.header.h != this->height_ ||
      this->draw_buf_.header.stride != this->width_ * 2) {
    ESP_LOGW(TAG, "Failed to render the screen");
    this->status_set_warning(LOG_STR("Failed to render the screen"));
    return false;
  }
  return true;
}

void LvglScreenshot::encode_task(void *param) {
  auto *self = static_cast<LvglScreenshot *>(param);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    self->encode_();
    App.wake_loop_threadsafe();
  }
}

void LvglScreenshot::encode_() {
  const uint32_t start = millis();
  // LVGL renders RGB565 with the low byte first; camera encoders take the high byte first, as a
  // camera sends it.
  auto *pixels = reinterpret_cast<uint16_t *>(this->raw_);
  for (size_t i = 0; i < this->raw_size_ / 2; i++)
    pixels[i] = byteswap(pixels[i]);

  camera::CameraImageSpec spec{this->width_, this->height_, camera::PIXEL_FORMAT_RGB565};
  RawScreenshot raw(this->raw_, this->raw_size_);
  camera::EncoderError result = camera::ENCODER_ERROR_RETRY_FRAME;
  for (uint8_t attempt = 0; attempt < MAX_ENCODE_ATTEMPTS && result == camera::ENCODER_ERROR_RETRY_FRAME; attempt++)
    result = this->encoder_->encode_pixels(&spec, &raw);
  this->encode_ms_ = millis() - start;
  this->encode_result_ = result;
  this->state_.store(
      result == camera::ENCODER_ERROR_SUCCESS ? CaptureState::CAPTURE_STATE_DONE : CaptureState::CAPTURE_STATE_FAILED,
      std::memory_order_release);
}

void LvglScreenshot::publish_() {
  camera::EncoderBuffer *jpeg = this->encoder_->get_output_buffer();
  this->reusable_image_->set_frame(jpeg->get_data(), jpeg->get_size(), this->pending_requesters_);
  this->current_image_ = this->reusable_image_;
  this->status_clear_warning();
  ESP_LOGV(TAG, "Screenshot %ux%u, %zu bytes (render %" PRIu32 " ms, encode %" PRIu32 " ms)", this->width_,
           this->height_, jpeg->get_size(), this->render_ms_, this->encode_ms_);
  for (uint8_t i = 0; i < this->listener_count_; i++)
    this->listeners_[i]->on_camera_image(this->current_image_);
}

void LvglScreenshot::add_listener(camera::CameraListener *listener) {
  uint16_t capacity;
  this->listeners_ = static_cast<camera::CameraListener **>(
      callback_manager_grow(this->listeners_, this->listener_count_, capacity, sizeof(camera::CameraListener *)));
  this->listeners_[this->listener_count_++] = listener;
}

void LvglScreenshot::request_image(camera::CameraRequester requester) {
  this->single_requesters_ |= (1U << requester);
  this->enable_loop();
}

void LvglScreenshot::start_stream(camera::CameraRequester requester) {
  this->stream_requesters_ |= (1U << requester);
  this->enable_loop();
  this->defer([this]() {
    for (uint8_t i = 0; i < this->listener_count_; i++)
      this->listeners_[i]->on_stream_start();
  });
}

void LvglScreenshot::stop_stream(camera::CameraRequester requester) {
  this->stream_requesters_ &= ~(1U << requester);
  this->defer([this]() {
    for (uint8_t i = 0; i < this->listener_count_; i++)
      this->listeners_[i]->on_stream_stop();
  });
}

void ScreenshotImageReader::set_image(std::shared_ptr<camera::CameraImage> image) {
  this->image_ = std::move(image);
  this->offset_ = 0;
}

size_t ScreenshotImageReader::available() const {
  if (!this->image_)
    return 0;
  return this->image_->get_data_length() - this->offset_;
}

uint8_t *ScreenshotImageReader::peek_data_buffer() { return this->image_->get_data_buffer() + this->offset_; }

void ScreenshotImageReader::consume_data(size_t consumed) { this->offset_ += consumed; }

void ScreenshotImageReader::return_image() { this->image_.reset(); }

}  // namespace esphome::lvgl_screenshot

#endif
