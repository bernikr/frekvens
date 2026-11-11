//Driver for the LED board in an IKEA Frekvens LED display block thing. This driver does
//8-bit BAM in order to get grayscales.

/*
 * Adapted by @bernikr from:
 *
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */

#include "ikea_frekvens.h"

#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/timer.h"

namespace esphome {
namespace ikea_frekvens {

static const char *TAG = "ikea_frekvens.light";

#define TIMER_GPOUP_NUMBER 1
#define TIMER_NUMBER 0

#if TIMER_GPOUP_NUMBER == 0
#define TIMER_GROUP TIMER_GROUP_0
#define TIMERG TIMERG0
#else
#define TIMER_GROUP TIMER_GROUP_1
#define TIMERG TIMERG1
#endif

#if TIMER_NUMBER == 0
#define TIMER TIMER_0
#else
#define TIMER TIMER_1
#endif


static uint8_t clock_pin;
static uint8_t data_pin;
static uint8_t latch_pin;

static volatile int bam_bit;
static SemaphoreHandle_t bam_sema = NULL;
static uint16_t bam_bitplane_a[BITPLANE_CT*16];
static uint16_t bam_bitplane_b[BITPLANE_CT*16];
static uint16_t* bam_bitplane_cur=bam_bitplane_a;
static uint16_t* bam_bitplane_next=bam_bitplane_a;
static uint16_t* bam_bitplane_draw=bam_bitplane_b;

void IkeaFrekvensLightOutput::set_clock_pin(uint8_t clock_pin_) { clock_pin = clock_pin_; };
void IkeaFrekvensLightOutput::set_data_pin(uint8_t data_pin_) { data_pin = data_pin_; };
void IkeaFrekvensLightOutput::set_latch_pin(uint8_t latch_pin_) { latch_pin = latch_pin_; };

light::LightTraits IkeaFrekvensLightOutput::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
  return traits;
};

void IkeaFrekvensLightOutput::clear_effect_data() {
  for (int i = 0; i < this->size(); i++)
    this->effect_data_[i] = 0;
};

void IkeaFrekvensLightOutput::write_state(light::LightState *state) {
  // protect from refreshing too often
  uint32_t now = micros();
  if (*this->max_refresh_rate_ != 0 && (now - this->last_refresh_) < *this->max_refresh_rate_) {
    // try again next loop iteration, so that this change won't get lost
    this->schedule_show();
    return;
  }
  this->last_refresh_ = now;
  this->mark_shown_();

  for(int i = 0; i<256; i++){
    light::ESPColorView pixel = this->get_view_internal(i);
    int v = pixel.get_white();
    int x = i & 0b1111;
    int y = i >> 4;

    y ^= 1;
    int bx=(x&7);
    int by=y/2;
    if (y&1) bx+=8;
    if ((x&8)==0) by+=8;

    int bitmask=1;
    int pixmask=(1<<bx);
    for (int i=0; i<BITPLANE_CT; i++) {
      if (v&bitmask) {
        bam_bitplane_draw[by+i*16] |= pixmask;
      } else {
        bam_bitplane_draw[by+i*16] &= ~pixmask;
      }
      bitmask<<=1;
    }
  }

  ESP_LOGI(TAG, "Switching bitplanes to new image");
  bam_bitplane_draw = bam_bitplane_next;
  if (bam_bitplane_next == bam_bitplane_a) {
    bam_bitplane_next=bam_bitplane_b;
  } else {
    bam_bitplane_next=bam_bitplane_a;
  }
};

void IkeaFrekvensLightOutput::dump_config() {
  ESP_LOGCONFIG(TAG, "IKEA Frekvens led matrix:");
  ESP_LOGCONFIG(TAG, "  Max refresh rate: %u", *this->max_refresh_rate_);
};

light::ESPColorView IkeaFrekvensLightOutput::get_view_internal(int32_t index) const {
  uint8_t *base = this->raw_pixel_data_ + 4ULL * index;
  return light::ESPColorView(base + 0, base + 1, base + 2, base + 3, &this->effect_data_[index], &this->correction_);
};

static void IRAM_ATTR bam_timer_isr(void *para) {
  //Latch in what the shifter task shifted out.
  gpio_set_level((gpio_num_t)latch_pin, 1);
  ets_delay_us(10);
  gpio_set_level((gpio_num_t)latch_pin, 0);
  ets_delay_us(1);

  #if TIMER_NUMBER == 0
  TIMERG.int_clr_timers.t0 = 1;
  #else
  TIMERG.int_clr_timers.t1 = 1;
  #endif

  //Set alarm value according what current BAM bit is
  uint64_t alrm=(1<<bam_bit)*BAM_DIV;
  TIMERG.hw_timer[TIMER_NUMBER].alarm_high = (uint32_t) (alrm >> 32);
  TIMERG.hw_timer[TIMER_NUMBER].alarm_low = (uint32_t) alrm;

  /* After the alarm has been triggered
    we need enable it again, so it is triggered the next time */
  TIMERG.hw_timer[TIMER_NUMBER].config.alarm_en = TIMER_ALARM_EN;

  //update bam_bit for what is going to be sent next
  bam_bit=bam_bit+1;
  if (bam_bit==BITPLANE_CT) {
    bam_bit=0;
  }

  //Poke the display task so it can send over the pixel data for the next bit.
  xSemaphoreGiveFromISR(bam_sema, NULL);

  //All done.
  portYIELD_FROM_ISR();
}

static void display_task(void *arg) {
  spi_device_handle_t spi;
  esp_err_t ret;

  //latch is used as a gpio in the timer int
  gpio_pad_select_gpio(latch_pin);
  gpio_set_direction((gpio_num_t)latch_pin, GPIO_MODE_OUTPUT);

  bam_bit=0;

  spi_bus_config_t buscfg;
  buscfg.miso_io_num=-1;
  buscfg.mosi_io_num=data_pin;
  buscfg.sclk_io_num=clock_pin;
  buscfg.quadwp_io_num=-1;
  buscfg.quadhd_io_num=-1;
  buscfg.max_transfer_sz=16*16;

  spi_device_interface_config_t devcfg;
  devcfg.clock_speed_hz=10*1000*1000;			//Clock out at 10 MHz
  devcfg.mode=0;								//SPI mode 0
  devcfg.spics_io_num=-1;						//we'll handle latch in the timer
  devcfg.queue_size=2;

  //Initialize the SPI bus
  ret=spi_bus_initialize(HSPI_HOST, &buscfg, 2);
  ESP_ERROR_CHECK(ret);
  //Attach the device to the SPI bus
  ret=spi_bus_add_device(HSPI_HOST, &devcfg, &spi);
  ESP_ERROR_CHECK(ret);
  
  bam_sema=xSemaphoreCreateBinary();
  
  /* Select and initialize basic parameters of the timer */
  timer_config_t config;
  config.divider = 80; //1MHz
  config.counter_dir = TIMER_COUNT_UP;
  config.counter_en = TIMER_PAUSE;
  config.alarm_en = TIMER_ALARM_EN;
  config.intr_type = TIMER_INTR_LEVEL;
  config.auto_reload = 1; //auto-reset on int
  //		.clk_src = TIMER_SRC_CLK_APB,

  timer_init(TIMER_GROUP, TIMER, &config);
  //counter start
  timer_set_counter_value(TIMER_GROUP, TIMER, 0x00000000ULL);

  /* Configure the alarm value and the interrupt on alarm. */
  timer_set_alarm_value(TIMER_GROUP, TIMER, 1000);
  timer_isr_register(TIMER_GROUP, TIMER, bam_timer_isr, NULL, ESP_INTR_FLAG_IRAM, NULL);
  timer_start(TIMER_GROUP, TIMER);
  timer_enable_intr(TIMER_GROUP, TIMER);

  spi_transaction_t trans;
  trans.length=16*16; //we send 256 bits per update

  while(1) {
    //Send the next bitplane
    trans.tx_buffer=&bam_bitplane_cur[bam_bit*16];
    ret=spi_device_polling_transmit(spi, &trans);  //Transmit!
    assert(ret==ESP_OK);            //Should have had no issues.
    if (bam_bit==0) {
      //buffer flip, if needed
      bam_bitplane_cur=bam_bitplane_next;
    }
    //wait for timer to latch this data
    xSemaphoreTake(bam_sema, portMAX_DELAY);
  }
}

void IkeaFrekvensLightOutput::setup() {
  ESP_LOGI(TAG, "Setup lights");
  for (int i = 0; i < this->size(); i++) {
    (*this)[i] = Color(0, 0, 0, 0);
  }

  xTaskCreate(
    &display_task,
    "frekvens_task",     // name
    1024,                // stack size
    nullptr,             // task pv params
    12,                  // priority
    nullptr              // handle
  );
}

} //namespace ikea_frekvens
} //namespace esphome