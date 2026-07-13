## BACnet Objects Configuration Guide

This project now includes 27 BACnet objects across 5 types:

### Object Types Summary

| Type | Instances | Access | Description |
|------|-----------|--------|-------------|
| **Analog Values (AV)** | AV1-AV16 | R/W | External analog values (e.g., PM2.5 sensor) |
| **Binary Values (BV)** | BV1-BV4 | R/W | External binary states (e.g., sensors) |
| **Analog Inputs (AI)** | AI1 | Read-only | Read-only analog sensor input |
| **Binary Inputs (BI)** | BI1-BI4 | Read-only | Read-only binary sensor states |
| **Binary Outputs (BO)** | BO1 | R/W | Writable binary output (e.g., relay control) |

### Configuration Locations

#### Analog Values (AV1-AV16)
- **File**: `main/analog_value.c`
- **Configuration Section**: Lines 17-56 (marked as `ANALOG VALUE CONFIGURATION`)
- **Customizable**:
  - Names (AV_NAMES)
  - Descriptions (AV_DESCRIPTIONS)
  - Units (AV_UNITS)
  - COV increments (AV_COV_INCREMENTS)
  - Initial values (AV_INITIAL_VALUES)

#### Binary Values (BV1-BV4)
- **File**: `main/binary_value.c`
- **Configuration Section**: Lines 16-50 (marked as `BINARY VALUE CONFIGURATION`)
- **Customizable**:
  - Names (BV_NAMES)
  - Descriptions (BV_DESCRIPTIONS)
  - Active/Inactive text (BV_ACTIVE_TEXT, BV_INACTIVE_TEXT)
  - Initial values (BV_INITIAL_VALUES)

#### Analog Inputs (AI1)
- **File**: `main/analog_input.c`
- **Configuration Section**: Lines 15-59 (marked as `ANALOG INPUT CONFIGURATION`)
- **Customizable**:
  - Names (AI_NAMES)
  - Descriptions (AI_DESCRIPTIONS)
  - Units (AI_UNITS)
  - COV increments (AI_COV_INCREMENTS)
  - Initial values (AI_INITIAL_VALUES)

#### Binary Inputs (BI1-BI4)
- **File**: `main/binary_input.c`
- **Configuration Section**: Lines 14-56 (marked as `BINARY INPUT CONFIGURATION`)
- **Customizable**:
  - Names (BI_NAMES)
  - Descriptions (BI_DESCRIPTIONS)
  - Active/Inactive text (BI_ACTIVE_TEXT, BI_INACTIVE_TEXT)
  - Initial values (BI_INITIAL_VALUES)

#### Binary Outputs (BO1)
- **File**: `main/binary_output.c`
- **Configuration Section**: Lines 14-56 (marked as `BINARY OUTPUT CONFIGURATION`)
- **Customizable**:
  - Names (BO_NAMES)
  - Descriptions (BO_DESCRIPTIONS)
  - Active/Inactive text (BO_ACTIVE_TEXT, BO_INACTIVE_TEXT)
  - Initial values (BO_INITIAL_VALUES)

### Features

#### Change of Value (COV)
All objects support BACnet COV notifications:
- Analog objects notify on changes exceeding the COV increment
- Binary objects notify on any state change
- Uses existing `bacnet_cov_task` infrastructure
- No additional configuration needed

#### Non-Volatile Storage (NVS)
All objects persist their configuration:
- Names and descriptions survive power cycles
- Values can be written via BACnet Write Property
- Auto-loaded on startup (unless OVERRIDE_NVS_ON_FLASH=1)
- Approximately 6-8 KB of the 64 KB NVS partition used

#### Current Display Integration
  - **Display shows**: AV1-AV4, BV1-BV4 (limited screen space)
  - **Not displayed**: AI1, BI1-BI4, BO1
- **Accessible via**: BACnet services (all objects fully functional)

### Updating Values Programmatically

To update object values from application code:

**Analog Values/Inputs**:
```c
Analog_Value_Present_Value_Set(1, 25.5f);      // AV1 = 25.5
Analog_Input_Present_Value_Set(1, 22.3f);      // AI1 = 22.3
```

**Binary Values/Inputs/Outputs**:
```c
Binary_Value_Present_Value_Set(1, BINARY_ACTIVE);       // BV1 = ACTIVE
Binary_Input_Present_Value_Set(1, BINARY_INACTIVE);     // BI1 = INACTIVE
Binary_Output_Present_Value_Set(1, BINARY_ACTIVE);      // BO1 = ACTIVE
```

### Examples

#### Example 1: Map PMS5003 Sensor Data to Multiple AV Objects
Modify `pms5003_task()` in `main.c`:
```c
// Currently writes only PM2.5 to AV1
// Add these lines to write all measurements:
Analog_Value_Present_Value_Set(1, (float)sensor_data.pm2_5_atm);     // PM2.5
Analog_Value_Present_Value_Set(2, (float)sensor_data.pm1_0_atm);     // PM1.0
Analog_Value_Present_Value_Set(3, (float)sensor_data.pm10_atm);      // PM10
```

#### Example 2: Create GPIO Read Task for BI Objects
```c
static void gpio_read_task(void *pvParameters)
{
    while (1) {
        uint32_t gpio_state = gpio_get_level(GPIO_NUM_XX);
        Binary_Input_Present_Value_Set(1, 
            gpio_state ? BINARY_ACTIVE : BINARY_INACTIVE);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

#### Example 3: Create GPIO Control Task for BO Objects
```c
static void gpio_control_task(void *pvParameters)
{
    while (1) {
        BACNET_BINARY_PV state = Binary_Output_Present_Value(1);
        gpio_set_level(GPIO_NUM_XX, state == BINARY_ACTIVE ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

### Memory Status

- **Flash Used**: 936 KB (44.9% of 2 MB available)
- **Flash Remaining**: 941 KB (safe margin for future features)
- **SRAM Headroom**: 200+ KB available
- **NVS Usage**: ~6-8 KB of 64 KB partition

All additions fit comfortably within available resources.

### Testing with BACnet Tools

Using YABE (Yet Another BACnet Explorer):

1. **Discover device**: Device-31416 (ESP32-BACnet)
2. **List objects**: Should see all 20 objects (AV1-4, BV1-4, AI1-4, BI1-4, BO1-4)
3. **Test read**: All objects should return current values
4. **Test write**: AV, BV, and BO objects should accept writes
5. **Test COV**: Subscribe to any object and change its value

### Troubleshooting

**Objects not appearing in BACnet tool:**
- Check ESP32 serial output for creation messages
- Verify WiFi connection is stable
- Ensure BBMD is reachable (10.113.33.1:47808)

**Values not persisting after reboot:**
- Verify NVS partition is available (check serial output on startup)
- Check that `override_nvs_on_flash` is 0 in main.c
- Ensure BACnet Write Property successfully changed the value

**COV notifications not received:**
- Verify subscription was accepted (YABE shows "Active" subscriptions)
- Check that value changes exceed the COV increment
- Monitor serial output for COV transmission messages

