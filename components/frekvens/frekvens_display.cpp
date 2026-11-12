#include "frekvens_display.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esphome/core/color.h"
#include "esphome/core/log.h"

#include "esp_rom_sys.h"
#include "esp_timer.h"

namespace esphome::frekvens {

static const char *const TAG = "frekvens";

// Length of the shortest BAM bitplane (plane 0, 2 ticks). The other planes
// double each time, so one frame takes 254 * BASE_TICK_US = 12.7 ms (~78 Hz)
// with 7 bitplanes (128 gray levels). Plane 0 lasts 100 us, which leaves
// enough headroom for the SPI transfer (~45 us) plus the task wake-up.
static constexpr uint32_t BASE_TICK_US = 50;

void FrekvensDisplay::setup() {
  this->init_internal_(PANEL_SIZE * PANEL_SIZE);
  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate the display buffer");
    this->mark_failed();
    return;
  }

  // Latch line: SCT2024 latches the shift register contents on a falling edge.
  gpio_config_t io_conf{};
  io_conf.pin_bit_mask = 1ULL << this->latch_pin_;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&io_conf) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure the latch pin");
    this->mark_failed();
    return;
  }
  gpio_set_level(static_cast<gpio_num_t>(this->latch_pin_), 0);

  if (this->enable_pin_ != 0xFF && this->enable_output_ == nullptr) {
    io_conf.pin_bit_mask = 1ULL << this->enable_pin_;
    if (gpio_config(&io_conf) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to configure the enable pin");
      this->mark_failed();
      return;
    }
    // Keep the panel dark until BAM is running.
    gpio_set_level(static_cast<gpio_num_t>(this->enable_pin_), this->enable_pin_inverted_ ? 1 : 0);
  }

  spi_bus_config_t bus_config{};
  bus_config.mosi_io_num = this->data_pin_;
  bus_config.miso_io_num = -1;
  bus_config.sclk_io_num = this->clock_pin_;
  bus_config.quadwp_io_num = -1;
  bus_config.quadhd_io_num = -1;
  bus_config.max_transfer_sz = PLANE_WORDS * 2;

  esp_err_t err = spi_bus_initialize(HSPI_HOST, &bus_config, SPI_DMA_DISABLED);
  if (err == ESP_ERR_INVALID_STATE) {
    // Another component already owns the HSPI bus. The configured pins are then
    // only trustworthy if that component uses the same ones.
    ESP_LOGW(TAG, "HSPI bus already initialized, reusing it");
  } else if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize SPI: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  spi_device_interface_config_t device_config{};
  device_config.clock_speed_hz = static_cast<int>(this->clock_speed_);
  device_config.mode = 0;
  device_config.spics_io_num = -1;
  device_config.queue_size = 1;
  // The panel chain is wired and tested as LSB-first; this matches the original
  // working black/white firmware.
  device_config.flags = SPI_DEVICE_BIT_LSBFIRST;

  err = spi_bus_add_device(HSPI_HOST, &device_config, &this->spi_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add the SPI device: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  this->bam_sem_ = xSemaphoreCreateBinary();
  if (this->bam_sem_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create the BAM semaphore");
    this->mark_failed();
    return;
  }

  gptimer_config_t timer_config{};
  timer_config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  timer_config.direction = GPTIMER_COUNT_UP;
  timer_config.resolution_hz = 1'000'000;  // 1 tick = 1 us
  err = gptimer_new_timer(&timer_config, &this->timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create the BAM timer: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  gptimer_event_callbacks_t callbacks{};
  callbacks.on_alarm = &FrekvensDisplay::bam_isr_;
  err = gptimer_register_event_callbacks(this->timer_, &callbacks, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register the BAM timer callback: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  gptimer_alarm_config_t alarm_config{};
  alarm_config.alarm_count = BASE_TICK_US;
  alarm_config.reload_count = 0;
  alarm_config.flags.auto_reload_on_alarm = true;
  err = gptimer_set_alarm_action(this->timer_, &alarm_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure the BAM timer alarm: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  err = gptimer_enable(this->timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable the BAM timer: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  this->initialized_ = true;
  this->last_stats_ = millis();

  BaseType_t created = xTaskCreatePinnedToCore(
      &FrekvensDisplay::display_task_, "frekvens_bam", 6144, this, 19, &this->task_handle_, 1);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create the BAM task");
    this->mark_failed();
    return;
  }

  err = gptimer_start(this->timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start the BAM timer: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  this->update_enable_output_();
  const uint32_t frame_rate = 1'000'000u / (BASE_TICK_US * 254u);
  ESP_LOGI(TAG, "Frekvens panel started (SPI %u Hz, %u bit BAM, %u Hz frame rate)",
           static_cast<unsigned>(this->clock_speed_), static_cast<unsigned>(BAM_BITS),
           static_cast<unsigned>(frame_rate));
}

void FrekvensDisplay::dump_config() {
  ESP_LOGCONFIG(TAG, "IKEA Frekvens 16x16 panel:");
  ESP_LOGCONFIG(TAG, "  Data pin: GPIO%u", this->data_pin_);
  ESP_LOGCONFIG(TAG, "  Clock pin: GPIO%u", this->clock_pin_);
  ESP_LOGCONFIG(TAG, "  Latch pin: GPIO%u", this->latch_pin_);
  if (this->enable_pin_ != 0xFF) {
    ESP_LOGCONFIG(TAG, "  Enable pin: GPIO%u (%s)", this->enable_pin_,
                  this->enable_pin_inverted_ ? "active low" : "active high");
  } else {
    ESP_LOGCONFIG(TAG, "  Enable pin: not configured");
  }
  ESP_LOGCONFIG(TAG, "  Clock speed: %u Hz", static_cast<uint32_t>(this->clock_speed_));
  ESP_LOGCONFIG(TAG, "  Brightness: %.0f%% (%s)", this->brightness_ * 100.0f,
                this->enable_output_ != nullptr ? "OE PWM" : "software");
  ESP_LOGCONFIG(TAG, "  BAM bit depth: %u", static_cast<unsigned>(BAM_BITS));
  const uint32_t frame_rate = 1'000'000u / (BASE_TICK_US * 254u);
  ESP_LOGCONFIG(TAG, "  Frame rate: %u Hz", static_cast<unsigned>(frame_rate));
  LOG_UPDATE_INTERVAL(this);
}

void FrekvensDisplay::loop() {
  if (!this->initialized_ || this->suspended_) {
    return;
  }

  if (this->brightness_dirty_) {
    this->brightness_dirty_ = false;
    if (this->enable_output_ != nullptr) {
      this->update_enable_output_();
    } else {
      this->update();
    }
  }

  const uint32_t now = millis();
  if (now - this->last_stats_ >= 10'000) {
    const uint32_t elapsed = now - this->last_stats_;
    const uint32_t frames = this->frames_;
    const uint32_t missed = this->missed_latches_;
    const uint32_t spi_errors = this->spi_errors_;
    const uint32_t max_wake = this->max_wake_us_;
    const uint32_t max_spi = this->max_spi_us_;
    const uint32_t wake_total = this->wake_total_us_;
    const uint32_t spi_total = this->spi_total_us_;
    const uint32_t spi_count = this->spi_count_;
    this->frames_ = 0;
    this->missed_latches_ = 0;
    this->spi_errors_ = 0;
    this->max_wake_us_ = 0;
    this->max_spi_us_ = 0;
    this->wake_total_us_ = 0;
    this->spi_total_us_ = 0;
    this->spi_count_ = 0;
    uint32_t plane_misses[BAM_BITS];
    for (int i = 0; i < BAM_BITS; i++) {
      plane_misses[i] = this->missed_per_plane_[i];
      this->missed_per_plane_[i] = 0;
    }
    this->last_stats_ = now;

    const uint32_t fps = elapsed > 0 ? static_cast<uint32_t>(frames * 1000ULL / elapsed) : 0;
    const uint32_t avg_wake = spi_count > 0 ? wake_total / spi_count : 0;
    const uint32_t avg_spi = spi_count > 0 ? spi_total / spi_count : 0;
    ESP_LOGD(TAG,
             "BAM: %u fps, %u late latch(es), avg wake %u us, avg SPI %u us, max wake %u us, max SPI %u us, "
             "cores isr=%d task=%d",
             static_cast<unsigned>(fps), static_cast<unsigned>(missed), static_cast<unsigned>(avg_wake),
             static_cast<unsigned>(avg_spi), static_cast<unsigned>(max_wake), static_cast<unsigned>(max_spi),
             this->isr_core_, this->task_core_);
    ESP_LOGD(TAG, "Late latches per plane: %u %u %u %u %u %u %u", static_cast<unsigned>(plane_misses[0]),
             static_cast<unsigned>(plane_misses[1]), static_cast<unsigned>(plane_misses[2]),
             static_cast<unsigned>(plane_misses[3]), static_cast<unsigned>(plane_misses[4]),
             static_cast<unsigned>(plane_misses[5]), static_cast<unsigned>(plane_misses[6]));
  }
}

void FrekvensDisplay::suspend() {
  if (!this->initialized_ || this->suspended_) {
    return;
  }
  this->suspended_ = true;
  gptimer_stop(this->timer_);
  this->update_enable_output_();
  ESP_LOGD(TAG, "BAM suspended");
}

void FrekvensDisplay::resume() {
  if (!this->initialized_ || !this->suspended_) {
    return;
  }
  this->suspended_ = false;

  // Drop a possibly pending latch event and start a clean frame.
  while (xSemaphoreTake(this->bam_sem_, 0) == pdTRUE) {
  }
  this->task_ready_ = true;
  this->bam_bit_ = 0;
  this->units_left_ = 1;
  this->frame_ready_.store(false, std::memory_order_release);

  gptimer_set_raw_count(this->timer_, 0);
  gptimer_start(this->timer_);
  this->update_enable_output_();
  ESP_LOGD(TAG, "BAM resumed");
}

void FrekvensDisplay::update() {
  if (!this->initialized_ || this->suspended_) {
    return;
  }
  // Keep at most one finished frame waiting. If the display task has not
  // consumed the previous one yet, try again on the next poll.
  if (this->frame_ready_.load(std::memory_order_acquire)) {
    ESP_LOGV(TAG, "Previous frame not consumed yet, skipping this update");
    return;
  }

  this->do_update_();
  this->encode_frame_();
}

void FrekvensDisplay::set_brightness(float brightness) {
  brightness = std::clamp(brightness, 0.0f, 1.0f);
  if (std::fabs(brightness - this->brightness_) < 0.001f) {
    return;
  }
  this->brightness_ = brightness;
  this->brightness_255_ = static_cast<uint8_t>(std::lround(brightness * 255.0f));
  this->brightness_dirty_ = true;
}

void FrekvensDisplay::update_enable_output_() {
  if (this->enable_output_ != nullptr) {
    this->enable_output_->set_level(this->suspended_ ? 0.0f : this->brightness_);
    return;
  }
  if (this->enable_pin_ == 0xFF) {
    return;
  }
  const bool on = this->brightness_255_ > 0 && !this->suspended_;
  const int level = this->enable_pin_inverted_ ? (on ? 0 : 1) : (on ? 1 : 0);
  gpio_set_level(static_cast<gpio_num_t>(this->enable_pin_), level);
}

void FrekvensDisplay::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= PANEL_SIZE || y >= PANEL_SIZE) {
    return;
  }

  // Grayscale displays get their value from the white channel. For RGB colors,
  // fall back to perceived luminance so existing display code works unchanged.
  const uint8_t luminance =
      static_cast<uint8_t>((static_cast<uint16_t>(color.r) * 77 + static_cast<uint16_t>(color.g) * 150 +
                            static_cast<uint16_t>(color.b) * 29) >>
                           8);
  this->buffer_[y * PANEL_SIZE + x] = std::max(color.white, luminance);
}

void FrekvensDisplay::encode_frame_() {
  const uint8_t back = this->back_.load(std::memory_order_relaxed);
  std::memset(this->bitplanes_[back], 0, sizeof(this->bitplanes_[back]));


  for (int y = 0; y < PANEL_SIZE; y++) {
    for (int x = 0; x < PANEL_SIZE; x++) {
      uint8_t value = this->buffer_[y * PANEL_SIZE + x];
      if (this->enable_output_ == nullptr && this->brightness_255_ != 255) {
        value = esp_scale8(value, this->brightness_255_);
      }
      if (value == 0) {
        continue;
      }
      value >>= 1;  // 7-bit BAM: the LSB (0.4% brightness) is not worth a 50 us slot
      if (value == 0) {
        continue;
      }

      // The panel is wired in four 8x8 quadrants. Each 16-bit word drives two
      // consecutive rows of one 8-column half; the two 8-column halves of a
      // logical row are in different words.
      int word = y >> 1;
      int bit = x & 0x7;
      if (y & 1) {
        bit += 8;
      }
      // x < 8 is the left half, x >= 8 the right half.
      if ((x & 0x8) != 0) {
        word += 8;
      }
      const uint16_t pixel_mask = static_cast<uint16_t>(1u << bit);
      for (int plane = 0; plane < BAM_BITS; plane++) {
        if (value & (1u << plane)) {
          this->bitplanes_[back][plane][word] |= pixel_mask;
        }
      }
    }
  }

  this->frame_ready_.store(true, std::memory_order_release);
}

void FrekvensDisplay::swap_buffers_() {
  if (!this->frame_ready_.load(std::memory_order_acquire)) {
    return;
  }
  const uint8_t old_front = this->front_.load(std::memory_order_relaxed);
  this->front_.store(this->back_.load(std::memory_order_relaxed), std::memory_order_release);
  this->back_.store(old_front, std::memory_order_relaxed);
  this->frame_ready_.store(false, std::memory_order_release);
}

void FrekvensDisplay::display_task_(void *arg) {
  auto *self = static_cast<FrekvensDisplay *>(arg);

  while (true) {
    if (xSemaphoreTake(self->bam_sem_, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    const uint8_t bit = self->bam_bit_;
    if (bit == 0) {
      // A new frame starts here; publish it before its first plane is sent.
      self->swap_buffers_();
    }

    const int64_t start_us = esp_timer_get_time();
    const int64_t isr_us = self->last_isr_us_;
    if (start_us > isr_us) {
      const auto wake_us = static_cast<uint32_t>(start_us - isr_us);
      self->wake_total_us_ += wake_us;
      if (wake_us > self->max_wake_us_) {
        self->max_wake_us_ = wake_us;
      }
    }
    self->task_core_ = xPortGetCoreID();

    spi_transaction_t transaction{};
    transaction.length = PLANE_WORDS * 16;  // bits
    transaction.tx_buffer = self->bitplanes_[self->front_.load(std::memory_order_relaxed)][bit];

    const esp_err_t err = spi_device_polling_transmit(self->spi_, &transaction);
    if (err != ESP_OK) {
      self->spi_errors_++;
    }
    const auto spi_us = static_cast<uint32_t>(esp_timer_get_time() - start_us);
    self->spi_total_us_ += spi_us;
    self->spi_count_++;
    if (spi_us > self->max_spi_us_) {
      self->max_spi_us_ = spi_us;
    }
    // The register now holds the plane that will be latched at the next BAM
    // boundary, so latches must not be deferred any more.
    self->task_ready_ = true;
  }
}

bool IRAM_ATTR FrekvensDisplay::bam_isr_(gptimer_handle_t timer, const gptimer_alarm_event_data_t *event,
                                         void *user_data) {
  (void) timer;
  (void) event;
  auto *self = static_cast<FrekvensDisplay *>(user_data);
  BaseType_t high_priority_task_woken = pdFALSE;

  if (self->units_left_ > 0) {
    self->units_left_--;
  }

  if (self->units_left_ == 0) {
    if (!self->task_ready_) {
      // The next plane is still being shifted in. Do not latch a partially
      // shifted frame; simply show the current plane for one more tick.
      self->missed_latches_++;
      if (self->bam_bit_ < BAM_BITS) {
        self->missed_per_plane_[self->bam_bit_]++;
      }
      return high_priority_task_woken == pdTRUE;
    }

    // SCT2024: outputs follow the shift register while LA is high and are
    // latched when LA goes low.
    gpio_set_level(static_cast<gpio_num_t>(self->latch_pin_), 1);
    esp_rom_delay_us(2);
    gpio_set_level(static_cast<gpio_num_t>(self->latch_pin_), 0);

    self->bam_bit_ = static_cast<uint8_t>(self->bam_bit_ + 1);
    if (self->bam_bit_ >= BAM_BITS) {
      self->bam_bit_ = 0;
      self->frames_++;
    }
    self->units_left_ = 2u << self->bam_bit_;

    self->task_ready_ = false;
    self->isr_core_ = xPortGetCoreID();
    self->last_isr_us_ = esp_timer_get_time();
    xSemaphoreGiveFromISR(self->bam_sem_, &high_priority_task_woken);
  }

  return high_priority_task_woken == pdTRUE;
}

}  // namespace esphome::frekvens
