#pragma once


#define I2C_DRIVER I2CD1
#define I2C1_SDA_PIN GP6
#define I2C1_SCL_PIN GP7


#define WS2812_PIO_USE_PIO1

// SK6812MINI-E: VIH is 0.65*VDD (~3.25V), barely below the RP2040's 3.3V output.
// Use the longest in-spec high times so late-detected edges still decode correctly.
#define WS2812_TIMING 1300
#define WS2812_T0H 400
#define WS2812_T1H 1000

#define ENCODER_RESOLUTION 4

// Keep the Spotify widget on instead of blanking after 60s without key presses.
#define OLED_TIMEOUT 0
