#pragma once

#include "esphome/core/component.h"
#include "esphome/components/light/addressable_light.h"
#include "esphome/components/spi/spi.h"
#include "esphome/components/output/float_output.h"

namespace esphome {
namespace ikea_frekvens_simple {

static const char *TAG = "ikea_frekvens_simple.light";

class IkeaFrekvensSimpleLightOutput : public light::AddressableLight,
                                      public spi::SPIDevice<spi::BIT_ORDER_LSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                                            spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
  public:
    IkeaFrekvensSimpleLightOutput() {
      this->effect_data_ = new uint8_t[this->size()];
      this->raw_pixel_data_ = new uint8_t[4ULL * this->size()];
    }

    void set_latch_pin(int latch_pin_) {
      latch_pin = (gpio_num_t)latch_pin_;
    }
    void set_output(output::FloatOutput *output) { output_ = output; }

    int32_t size() const override {return 256;}

    void setup() {
      gpio_reset_pin(latch_pin);
      gpio_set_direction(latch_pin, GPIO_MODE_OUTPUT);

      for(int i = 0; i<16; i++){
        pixels[i] = 0;
      }
    }

    light::LightTraits get_traits() override {
      auto traits = light::LightTraits();
      traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
      return traits;
    }

    void write_state(light::LightState *state) override {
      this->mark_shown_();

      if (this->output_ != nullptr) {
        float bright;
        state->current_values_as_brightness(&bright);
        this->output_->set_level(bright);
      }

      for(int i = 0; i<16; i++){
        pixels[i] = 0;
      }

      for(int i = 0; i<256; i++){
        light::ESPColorView pixel = this->get_view_internal(i);
        if (pixel.get_white() > 0) {
          int x = i & 0b1111;
          int y = i >> 4;

          //Refrobnicating coordinates because pixels are located weirdly in hardware
          y ^= 1;
          int bx=(x&7);
          int by=y/2;
          if (y&1) bx+=8;
          if ((x&8)==0) by+=8;

          pixels[by] |= (1<<bx);
        }
      }

      this->write_array16(pixels, 16);
      ets_delay_us(10);
      gpio_set_level((gpio_num_t)latch_pin, 1);
      ets_delay_us(10);
      gpio_set_level((gpio_num_t)latch_pin, 0);
    }

    void clear_effect_data() override {
      for (int i = 0; i < this->size(); i++)
        this->effect_data_[i] = 0;
    }
  
  protected:
    gpio_num_t latch_pin;
    uint16_t pixels[16];
    uint8_t* effect_data_{nullptr};
    uint8_t* raw_pixel_data_{nullptr};
    output::FloatOutput *output_{nullptr};

    light::ESPColorView get_view_internal(int32_t index) const override {
      uint8_t *base = this->raw_pixel_data_ + 4ULL * index;
      return light::ESPColorView(base + 0, base + 1, base + 2, base + 3, &this->effect_data_[index], &this->correction_);
    }
};

} //namespace ikea_frekvens_simple
} //namespace esphome