#pragma once

#include <atomic>
#include <cstdint>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/components/output/float_output.h"

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace esphome::frekvens {

/**
 * IKEA Frekvens 16x16 LED panel.
 *
 * The panel is 16 SCT2024 16-bit constant-current shift registers in series,
 * driven through SPI (data/clock) plus a latch line. The shift registers only
 * support on/off, so grayscale is produced with Binary Code Modulation (BAM):
 * each frame is split into 7 bitplanes and every bitplane is displayed for
 * twice as long as the previous one.
 *
 * Timing cannot be done from the ESPHome main loop. A hardware timer latches
 * each bitplane and wakes a dedicated high-priority task that shifts the next
 * bitplane into the registers while the current one is visible.
 */
class FrekvensDisplay final : public display::DisplayBuffer {
 public:
  static constexpr int PANEL_SIZE = 16;
  static constexpr int BAM_BITS = 7;
  static constexpr int PLANE_WORDS = 16;  // 256 bits per plane

  void set_data_pin(uint8_t pin) { this->data_pin_ = pin; }
  void set_clock_pin(uint8_t pin) { this->clock_pin_ = pin; }
  void set_latch_pin(uint8_t pin) { this->latch_pin_ = pin; }
  void set_enable_pin(uint8_t pin) { this->enable_pin_ = pin; }
  void set_enable_pin_inverted(bool inverted) { this->enable_pin_inverted_ = inverted; }
  void set_clock_speed(uint32_t hz) { this->clock_speed_ = hz; }
  void set_enable_output(output::FloatOutput *output) { this->enable_output_ = output; }
  void set_brightness(float brightness);

  /// Stop the BAM timer and the panel output (used while OTA writes flash).
  void suspend();
  /// Restart the BAM timer after a suspended OTA attempt.
  void resume();

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  display::DisplayType get_display_type() override {
    return display::DisplayType::DISPLAY_TYPE_GRAYSCALE;
  }

 protected:
  int get_width_internal() override { return PANEL_SIZE; }
  int get_height_internal() override { return PANEL_SIZE; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  void encode_frame_();
  void swap_buffers_();
  void update_enable_output_();

  static void display_task_(void *arg);
  static bool IRAM_ATTR bam_isr_(gptimer_handle_t timer,
                                 const gptimer_alarm_event_data_t *event, void *user_data);

  uint8_t data_pin_{0};
  uint8_t clock_pin_{0};
  uint8_t latch_pin_{0};
  uint8_t enable_pin_{0xFF};
  bool enable_pin_inverted_{true};
  output::FloatOutput *enable_output_{nullptr};
  uint32_t clock_speed_{10'000'000};

  float brightness_{1.0f};
  uint8_t brightness_255_{255};
  bool brightness_dirty_{false};

  // Two frame buffers. The display task owns front_/back_ and swaps them at a
  // frame boundary; update() only builds the buffer that is not displayed.
  uint16_t bitplanes_[2][BAM_BITS][PLANE_WORDS]{};
  std::atomic<uint8_t> front_{0};
  std::atomic<uint8_t> back_{1};
  std::atomic<bool> frame_ready_{false};

  // Written by the timer ISR / display task respectively.
  volatile bool task_ready_{true};
  volatile uint8_t bam_bit_{0};
  volatile uint32_t units_left_{1};
  volatile uint32_t frames_{0};
  volatile uint32_t missed_latches_{0};
  volatile uint32_t spi_errors_{0};
  volatile int64_t last_isr_us_{0};
  volatile uint32_t max_wake_us_{0};
  volatile uint32_t max_spi_us_{0};
  volatile uint32_t wake_total_us_{0};
  volatile uint32_t spi_total_us_{0};
  volatile uint32_t spi_count_{0};
  volatile uint32_t missed_per_plane_[BAM_BITS]{};
  volatile int isr_core_{-1};
  volatile int task_core_{-1};

  SemaphoreHandle_t bam_sem_{nullptr};
  gptimer_handle_t timer_{nullptr};
  spi_device_handle_t spi_{nullptr};
  TaskHandle_t task_handle_{nullptr};
  uint32_t last_stats_{0};
  bool initialized_{false};
  bool suspended_{false};
};

template<typename... Ts>
class FrekvensSetBrightnessAction : public Action<Ts...>, public Parented<FrekvensDisplay> {
 public:
  TEMPLATABLE_VALUE(float, brightness)

  void play(Ts... x) override {
    this->parent_->set_brightness(this->brightness_.value(x...));
  }
};

}  // namespace esphome::frekvens
