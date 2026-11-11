#pragma once

#include "esphome/core/component.h"
#include "esphome/components/light/addressable_light.h"

#define BITPLANE_CT 8
#define BAM_DIV 50 //1MHz/(BAM_DIV*(1<<bit))

namespace esphome {
namespace ikea_frekvens {

class IkeaFrekvensLightOutput : public light::AddressableLight {
  public:
    IkeaFrekvensLightOutput() {
      this->effect_data_ = new uint8_t[this->size()];
      this->raw_pixel_data_ = new uint8_t[4ULL * this->size()];
    }
    void setup() override;
    void clear_effect_data() override;

    inline int32_t size() const override {return 256;};
    light::LightTraits get_traits() override;

    void set_clock_pin(uint8_t clock_pin_);
    void set_data_pin(uint8_t data_pin_);
    void set_latch_pin(uint8_t latch_pin_);
    void set_max_refresh_rate(uint32_t interval_us) { max_refresh_rate_ = interval_us; };
    
    void write_state(light::LightState *state) override;
    void dump_config() override;

  protected:
    uint8_t* effect_data_{nullptr};
    uint8_t* raw_pixel_data_{nullptr};
    uint32_t last_refresh_;
    optional<uint32_t> max_refresh_rate_;

    light::ESPColorView get_view_internal(int32_t index) const override;
};

} //namespace ikea_frekvens
} //namespace esphome