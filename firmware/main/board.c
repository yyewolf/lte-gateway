#include "board.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

void board_early_init(void)
{
    // GPIO43 is the ROM's UART TX and idles high after reset, which holds PWRKEY pressed.
    // Take it over before anything else. The modem supply is already off (R105).
    gpio_set_level(PIN_MODEM_PWRKEY, 0);
    gpio_set_level(PIN_MODEM_RESET, 0);
    gpio_set_level(PIN_MODEM_DTR, 0);
    gpio_set_level(PIN_MODEM_PWR_EN, 0);
    gpio_set_level(PIN_LED_GREEN, 0);
    gpio_set_level(PIN_LED_RED, 0);
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_MODEM_PWRKEY) | (1ULL << PIN_MODEM_RESET) | (1ULL << PIN_MODEM_DTR) |
                        (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_RED) | (1ULL << PIN_MODEM_PWR_EN),
        .mode = GPIO_MODE_INPUT_OUTPUT,   // so gpio_get_level() reads back what we drive
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_MODEM_STATUS) | (1ULL << PIN_MODEM_NETLIGHT) | (1ULL << PIN_MODEM_RI),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
}

void board_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t ac = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ac, &s_adc));
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &cc);   // GPIO1
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_1, &cc);   // GPIO2
    adc_cali_curve_fitting_config_t cal = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12,
                                            .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
}

static int adc_mv(adc_channel_t ch)
{
    int raw = 0, mv = 0;
    if (!s_adc) return 0;
    adc_oneshot_read(s_adc, ch, &raw);
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv;
    return raw * 3100 / 4095;
}

float board_vbus(void) { return adc_mv(ADC_CHANNEL_0) / 1000.0f * 147.0f / 47.0f; }
float board_vbat(void) { return adc_mv(ADC_CHANNEL_1) / 1000.0f * 2.0f; }
void board_led(int pin, bool on) { gpio_set_level(pin, on); }
