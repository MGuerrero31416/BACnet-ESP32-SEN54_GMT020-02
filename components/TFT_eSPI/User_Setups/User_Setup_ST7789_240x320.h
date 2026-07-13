// Provisional ST7789 240x320 setup (from proven Arduino config)
#ifndef USER_SETUP_H
#define USER_SETUP_H

#define TFT_RGB_ORDER TFT_BGR

#define ST7789_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   25   // Changed from GPIO5 to avoid MAX485 conflict
#define TFT_DC   27
#define TFT_RST  33

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY      40000000
#define SPI_READ_FREQUENCY 20000000

#endif
