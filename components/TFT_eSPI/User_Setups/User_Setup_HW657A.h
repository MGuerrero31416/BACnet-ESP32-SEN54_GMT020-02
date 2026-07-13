// Preserved original HW657A TFT_eSPI setup
#define USER_SETUP_INFO "HW657A 170x320 display (preserved original)"

// Driver
#define ST7789_DRIVER

// Resolution
#define TFT_WIDTH  170
#define TFT_HEIGHT 320

// SPI pins
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4

// Backlight (active-high)
#define TFT_BL   32
#define TFT_BACKLIGHT_ON HIGH

// SPI speed
#define SPI_FREQUENCY 20000000

// Color / panel options
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_ON
#define TFT_OFFSET_X 0
#define TFT_OFFSET_Y 0

// Fonts
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// Touch disabled
#define TOUCH_CS -1
