// Pin map of the LTE SMS gateway rev0.6 (read from the rev0.6 Gerber netlist).
#pragma once
#include <stdbool.h>

// Modem control (through NPN transistors / load switch)
#define PIN_MODEM_PWR_EN   7    // high = modem supply (U108) on; R105 pulls it low at reset
#define PIN_MODEM_PWRKEY   43   // high = PWRKEY pressed (Q301). Also the ROM's UART0 TX at boot.
#define PIN_MODEM_RESET    14   // high = RESET held (Q302). Never together with PWRKEY.
#define PIN_MODEM_STATUS   42   // input, high = modem running
#define PIN_MODEM_NETLIGHT 21
#define PIN_MODEM_RI       40
#define PIN_MODEM_DTR      41   // keep low

// UART to the modem, through the TXS0108E level shifter
#define MODEM_UART         UART_NUM_1
#define PIN_MODEM_TX       47   // ESP32 TX -> modem RXD
#define PIN_MODEM_RX       48   // ESP32 RX <- modem TXD
#define MODEM_BAUD         115200

// LEDs (high = on)
#define PIN_LED_GREEN      4
#define PIN_LED_RED        5

// BOOT button (SW202), low = pressed. Hold 5 s while running = setup mode.
#define PIN_BOOT_BUTTON    0

// ES8311 codec
#define PIN_I2C_SCL        12
#define PIN_I2C_SDA        13
#define PIN_I2S_MCLK       11
#define PIN_I2S_BCLK       10
#define PIN_I2S_LRCK       46
#define PIN_I2S_DOUT       3    // ESP32 -> codec DAC (uplink: what the caller hears)
#define PIN_I2S_DIN        9    // codec ADC -> ESP32 (downlink: the caller's voice)
#define ES8311_I2C_ADDR    0x18

// Supply sense (ADC1)
#define PIN_VBUS_SENSE     1    // x 147/47
#define PIN_VBAT_SENSE     2    // x 2, on +3V8 after the modem switch

#define AUDIO_RATE_HZ      8000 // narrowband, same as G.711

// board.c
void board_early_init(void);   // first thing in app_main: PWRKEY/RESET/PWR_EN low, LEDs off
void board_adc_init(void);
float board_vbus(void);        // volts at the USB input
float board_vbat(void);        // volts on +3V8 after the modem supply switch
void board_led(int pin, bool on);
