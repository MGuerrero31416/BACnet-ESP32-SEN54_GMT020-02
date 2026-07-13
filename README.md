# ESP32 BACnet/IP & MS/TP Air Quality Monitor (SEN54 + ST7789)

**Current firmware:** v1.4.0

ESP32-based BACnet/IP and BACnet MS/TP air quality monitor using the Sensirion SEN54 and a 1.9" 170×320 ST7789 TFT display.

The device simultaneously supports BACnet/IP (Wi-Fi) and BACnet MS/TP (RS-485), exposing real-time air quality measurements, diagnostics, and maintenance controls as standard BACnet objects.

In addition to being a complete air quality monitor, this project serves as a flexible ESP32 BACnet template for custom sensors, GPIO expansion, and building automation applications.

## Highlights

- Dual BACnet/IP and BACnet MS/TP operation
- Sensirion SEN54 air quality sensor
- 27 BACnet objects (1 Device object + 26 application objects)
- Real-time ST7789 TFT display
- BACnet Change of Value (COV) support
- NVS persistence for configurable BACnet objects
- BACnet maintenance and diagnostic objects for the SEN54
- ESP-IDF 5.5 with Arduino as a component
- Easily extensible for additional BACnet objects and hardware

## Features

- **BACnet/IP Protocol** – Full BACnet/IP implementation over Wi-Fi
- **BACnet MS/TP** – RS-485 MS/TP support running simultaneously with BACnet/IP
- **Live Display** – Real-time display of selected BACnet object values on a 170×320 ST7789 TFT
- **24 BACnet Objects**
  - 1 Device object
  - 16 Analog Values (AV1–AV16)
  - 4 Binary Values (BV1–BV4)
  - 1 Analog Input (AI1)
  - 4 Binary Inputs (BI1–BI4)
  - 1 Binary Output (BO1)
- **Writable Metadata** – BACnet `Object Name` and `Description` are writable for AV, AI, BV, BI, and BO objects
- **BACnet Change of Value (COV)** – Efficient real-time notifications
- **Persistent Storage** – BACnet-configurable values are automatically stored in ESP32 Non-Volatile Storage (NVS)
- **NVS Override** – Setting `USER_OVERRIDE_NVS_ON_FLASH=1` restores factory defaults while preserving Wi-Fi credentials when appropriate
- **Centralized Configuration** – Most project settings are defined in [main/User_Settings.c](main/User_Settings.c)
- **ESP32 Wi-Fi** – Built-in Wi-Fi for BACnet/IP communication
- **Arduino Framework** – Uses Arduino as an ESP-IDF component

### SEN54 Integration

The integrated Sensirion SEN54 driver provides:

- Continuous PM1.0, PM2.5, PM4.0 and PM10 measurements
- Temperature and relative humidity measurements
- VOC Index measurement
- CRC-8 validation on all I²C communications
- Automatic sensor disconnect detection
- Thread-safe FreeRTOS implementation
- BACnet-integrated maintenance commands
- BACnet-integrated diagnostic status

### BACnet Maintenance Objects

| Object | Function |
|---------|----------|
| BV1 | Full sensor reset |
| BV2 | Enable/disable measurements |
| BV3 | Start manual fan cleaning |
| BV4 | Clear device status flags |
| AV7 | Read/write automatic fan cleaning interval |

### BACnet Diagnostic Objects

| Object | Status |
|---------|--------|
| BI1 | Fan failure |
| BI2 | Laser error |
| BI3 | VOC sensor error |
| BI4 | RHT sensor error |

## Photos
![Device](docs/images/01.jpg)
![Device](docs/images/03.jpg)
![Device](docs/images/04.jpg)
![Wiring](docs/images/ESP32-SEN54_WROM32_pinout.jpg)


## Hardware Requirements

- **Development Board:** ESP32-WROOM-32 DevKit (or compatible ESP32 module)
- **Display:** ST7789 SPI TFT (170×320 pixels)
- **Air Quality Sensor:** Sensirion SEN54
- **RS-485 Transceiver:** MAX485 (or compatible)

## Hardware Components

### ST7789 TFT Display

- Resolution: 170×320 pixels
- Driver: TFT_eSPI
- Display rotation: 1

#### Connections

| Display Pin | ESP32 GPIO |
|-------------|-----------:|
| MOSI | GPIO23 |
| SCLK | GPIO18 |
| CS | GPIO15 |
| DC | GPIO2 |
| RST | GPIO4 |
| Backlight | GPIO32 |

### Sensirion SEN54 Air Quality Sensor

- Communication: I²C (100 kHz, address `0x69`)
- Supply Voltage: 3.3 V or 5 V

#### Connections

| SEN54 Pin | ESP32 GPIO |
|-----------|-----------:|
| SDA | GPIO13 |
| SCL | GPIO14 |
| VCC | 3.3 V or 5 V |
| GND | GND |

### Default BACnet Mapping

| BACnet Object | Measurement |
|---------------|-------------|
| AV1 | Temperature (°C) |
| AV2 | Relative Humidity (%RH) |
| AV3 | PM2.5 (µg/m³) |
| AV4 | VOC Index |
| AV5 | PM1.0 (µg/m³) |
| AV6 | PM4.0 (µg/m³) |
| AV7 | Automatic Fan Cleaning Interval (seconds) |

> **Note**
>
> By default, SEN54 measurements are mapped to **Analog Value** objects rather than **Analog Input** objects. This makes it easy to remap sensors, simulate values during testing, or replace the data source without changing the BACnet object database.

PM10 and NOx Index are supported by the SEN54 driver and can be mapped to BACnet objects if required.

### SEN54 Features

- Continuous measurement every 2 seconds
- Device status polling every 5 seconds
- CRC-8 validation (Sensirion polynomial `0x31`)
- Thread-safe FreeRTOS mutex protection
- Automatic sensor disconnect detection
- Full sensor reset (`0xD304`)
- Manual fan cleaning (`0x5607`)
- Clear device status (`0xD210`)
- Read/write automatic fan cleaning interval (`0x8004`)

The default BACnet mapping can be modified by editing `sen54_task()` in [main/main.c](main/main.c).

### Wi-Fi Connectivity

- Built-in ESP32 Wi-Fi
- BACnet/IP communication
- Configured in [main/User_Settings.c](main/User_Settings.c)
- Optional static IP support using `USER_WIFI_USE_STATIC_IP`

### BACnet MS/TP (RS-485)

- Transceiver: MAX485 (or compatible)
- UART: UART2
- Default baud rate: **38400**
- Default MAC Address: **96**
- Default Max Master: **127**
- Default Max Info Frames: **80**

#### Connections

| MAX485 Pin | ESP32 GPIO |
|------------|-----------:|
| DE/RE | GPIO5 |
| DI (TX) | GPIO16 |
| RO (RX) | GPIO17 |

> **Note**
>
> Some BACnet MS/TP supervisors (such as Johnson Controls NAE) require devices to be added manually to the MS/TP field bus.

## GPIO Summary

| GPIO | Component | Signal |
|------|-----------|--------|
| GPIO13 | SEN54 | SDA |
| GPIO14 | SEN54 | SCL |
| GPIO2 | ST7789 | DC |
| GPIO4 | ST7789 | Reset |
| GPIO15 | ST7789 | Chip Select |
| GPIO18 | ST7789 | SPI Clock |
| GPIO23 | ST7789 | SPI MOSI |
| GPIO32 | ST7789 | Backlight |
| GPIO5 | MAX485 | DE / RE |
| GPIO16 | MAX485 | UART TX |
| GPIO17 | MAX485 | UART RX |

## Build Requirements

- **ESP-IDF:** v5.5.x
- **Python:** 3.11+
- **Toolchain:** xtensa-esp-elf (ESP32)

## Configuration

### Display Driver Settings

Display initialization and pin mapping are configured in [components/TFT_eSPI/User_Setup.h](components/TFT_eSPI/User_Setup.h) and [main/display.cpp](main/display.cpp), including:

- `TFT_MOSI 23`
- `TFT_SCLK 18`
- `TFT_CS 15`
- `TFT_DC 2`
- `TFT_RST 4`
- `TFT_BL 32`
- `tft.setRotation(1)`

### FreeRTOS Configuration

Arduino framework requires FreeRTOS tick rate of 1000Hz. This is set in [sdkconfig](sdkconfig):

```
CONFIG_FREERTOS_HZ=1000
```

### User Settings (Centralized Configuration)

Most user-configurable settings are centralized in [main/User_Settings.c](main/User_Settings.c) and declared in [main/User_Settings.h](main/User_Settings.h), including:

- WiFi SSID/password and static IP settings
- BACnet Device Instance and BBMD registration
- BACnet/IP and MS/TP enable flags (`USER_ENABLE_BACNET_IP`, `USER_ENABLE_BACNET_MSTP`)
- MS/TP parameters (MAC, baud rate, max master, max info frames)
- Default object names, descriptions, units, and initial values

### BACnet Object Configuration

- **Analog Values (AV1-7)**: Configure names, descriptions, units, and initial values in [main/User_Settings.c](main/User_Settings.c)

- **Binary Values (BV1-4)**: Configure names, descriptions, active/inactive text, and initial states in [main/User_Settings.c](main/User_Settings.c)

- **Analog Inputs (AI1-4)**: Configure names, descriptions, units, and COV increments in [main/User_Settings.c](main/User_Settings.c). Read-only inputs suitable for sensor integration.

- **Binary Inputs (BI1-4)**: Configure names, descriptions, active/inactive text in [main/User_Settings.c](main/User_Settings.c). Read-only binary states.

- **Binary Outputs (BO1-4)**: Configure names, descriptions, active/inactive text, and initial states in [main/User_Settings.c](main/User_Settings.c). Writable control outputs with priority support.

### Sensor Data Mapping

- **SEN54 Parameters**: Select which sensor measurement (PM1.0, PM2.5, PM4.0, PM10, temperature, humidity, VOC index, or NOx index) to map to each Analog Value object in [main/main.c](main/main.c) — look for the `sen54_task()` function. Current default mapping: AV1=Temperature, AV2=Humidity, AV3=PM2.5, AV4=VOC Index, AV5=PM1.0, AV6=PM4.0, AV7=Auto Cleaning Interval (seconds).

## Architecture

### Components

- **[components/bacnet-stack](components/bacnet-stack)** - BACnet/IP stack (modified from bacnet-stack/bacnet-stack)
- **[components/sen54](components/sen54)** - Sensirion SEN54 I2C driver
- **[components/Adafruit_BusIO](components/Adafruit_BusIO)** - Adafruit BusIO support library
- **[components/Adafruit_GFX_Library](components/Adafruit_GFX_Library)** - Adafruit graphics primitives
- **[components/Adafruit_ST7735_and_ST7789_Library](components/Adafruit_ST7735_and_ST7789_Library)** - Adafruit ST77xx driver library
- **[main](main/)** - Application code
  - `main.c` - BACnet initialization and main loop
  - `analog_value.c/h` - Analog Value object creation and NVS persistence
  - `binary_value.c/h` - Binary Value object creation and NVS persistence
  - `analog_input.c/h` - Analog Input object creation and NVS persistence
  - `binary_input.c/h` - Binary Input object creation and NVS persistence
  - `binary_output.c/h` - Binary Output object creation and NVS persistence
  - `display.cpp` - TFT display driver
  - `wifi_helper.c` - WiFi configuration helpers

### Display Layout

| Item | Type | Display |
|------|------|---------|
| AV1 | Analog Value | Temp, numeric (1 decimal) |
| AV2 | Analog Value | %HR, numeric (1 decimal) |
| AV3 | Analog Value | PM2.5, numeric (0 decimals) |
| AV4 | Analog Value | VOC, numeric (0 decimals) |
| Footer | Status Text | BACnet ID and MS/TP MAC |

## BACnet Integration

The device broadcasts its Device ID and manages BACnet objects that can be read/written by any BACnet/IP or BACnet MS/TP client (e.g., YABE, Tridium Niagara, Metasys).

### BACnet Objects Exposed

- **Device**: 55596 (configurable in [main/User_Settings.c](main/User_Settings.c))
- **Analog Values**: Instance 1, 2, 3, 4, 5, 6, 7
- **Binary Values**: Instance 1, 2, 3, 4
- **Analog Inputs**: Instance 1, 2, 3, 4
- **Binary Inputs**: Instance 1, 2, 3, 4
- **Binary Outputs**: Instance 1, 2, 3, 4

## Modifications to bacnet-stack

This project uses the official bacnet-stack with the following modifications:

- **[components/bacnet-stack/](components/bacnet-stack/)** - Configured as ESP-IDF component
- Simplified for embedded systems (reduced features, optimized for ESP32)
- WiFi-based BACnet/IP instead of Ethernet

The stack is configured as a local ESP-IDF component in [components/bacnet-stack](components/bacnet-stack).

## Development Notes

### Display Boundary Constants

The display code uses boundary constants for easy layout modification:

```c
#define DISP_X0    0
#define DISP_Y0    0
#define DISP_X1    319
#define DISP_Y1    169
#define DISP_WIDTH 320
#define DISP_HEIGHT 170
```

Position all elements relative to these constants to avoid hardcoding coordinates.

## Troubleshooting

### Display orientation or color issues
If display output looks mirrored, rotated, or has swapped colors, adjust ST7789 init parameters and rotation in [main/display.cpp](main/display.cpp) and recompile.

### WiFi connection fails
Check SSID/password in [main/User_Settings.c](main/User_Settings.c), then verify WiFi init/connection flow in [main/wifi_helper.c](main/wifi_helper.c).

### Linker errors with Arduino
Ensure `CONFIG_FREERTOS_HZ=1000` is set in [sdkconfig](sdkconfig) and rebuild with `idf.py fullclean && idf.py build`.


## References

- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/stable/)


## Display selection & recent changes

A reversible build-time display selection was added so you can choose between the original HW657A display and a provisional 240×320 ST7789 display from `menuconfig`.

- Menu: `Application Configuration` → `Display hardware`
- Configuration symbols added: `CONFIG_DISPLAY_HW657A`, `CONFIG_DISPLAY_ST7789_240X320` (provisional is the temporary default)

Files created
- [components/TFT_eSPI/User_Setups/User_Setup_ST7789_240x320.h](components/TFT_eSPI/User_Setups/User_Setup_ST7789_240x320.h) — provisional ST7789 setup (TFT_CS moved to GPIO25)
- [components/TFT_eSPI/User_Setups/User_Setup_HW657A.h](components/TFT_eSPI/User_Setups/User_Setup_HW657A.h) — preserved copy of the original setup
- [main/Kconfig.projbuild](main/Kconfig.projbuild) — added display selection

Files modified
- [components/TFT_eSPI/User_Setup_Select.h](components/TFT_eSPI/User_Setup_Select.h) — selects setup header by `CONFIG_` symbol
- [main/CMakeLists.txt](main/CMakeLists.txt) — ensured `BOARD_RS485_PHY_ENABLED=1` and defined `BOARD_BACNET_MSTP_ENABLED=1`
- [main/display.cpp](main/display.cpp) — conditional initialization, one-time startup info log, and layout adapted to use `tft.width()`/`tft.height()` where practical

Final pin assignments (current)

Provisional ST7789 display

 - `TFT_MOSI` = GPIO23
 - `TFT_SCLK` = GPIO18
 - `TFT_CS`   = GPIO25
 - `TFT_DC`   = GPIO27
 - `TFT_RST`  = GPIO33

MAX485 / BACnet MS/TP

 - UART TX = GPIO17
 - UART RX = GPIO16
 - DE/RE (direction) = GPIO5

RS-485 and BACnet

- `BOARD_RS485_PHY_ENABLED` is restored to `1` (RS-485 transceiver enabled).
- `BOARD_BACNET_MSTP_ENABLED` is defined as `1` so BACnet MS/TP remains active.
- The MS/TP code (`main/mstp_rs485.c`) restores the normal DE/RE GPIO configuration and UART pin assignments; no MS/TP protocol, object, or state-machine code was changed.

Build and verify locally

```powershell
idf.py fullclean
idf.py build

# To switch displays:
idf.py menuconfig
# Application Configuration -> Display hardware -> choose the desired profile
idf.py fullclean
idf.py build
```

Notes

- The provisional display configuration intentionally does not assume a backlight GPIO, touch controller, or panel offsets.
- No BACnet, SEN54, Wi‑Fi, NVS, or MS/TP protocol logic was modified — only component selection, a preserved setup header, and a small display-init adaptation were added.


