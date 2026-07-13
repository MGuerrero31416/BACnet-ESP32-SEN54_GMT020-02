idf_component_register(SRCS "display.cpp" "binary_output.c" "binary_input.c" "analog_input.c" "binary_value.c" "analog_value.c" "main.c" "wifi_helper.c" "mstp_rs485.c" "User_Settings.c"
                       REQUIRES bacnet-stack TFT_eSPI espressif__arduino-esp32 sen54
                       PRIV_REQUIRES spi_flash nvs_flash esp_event esp_wifi esp_netif driver esp_timer
                       INCLUDE_DIRS "" "fonts")

# Add linker flags for Arduino log wrapper compatibility
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_log_write"
    "-Wl,--wrap=esp_log_writev"
)

# Force the linker to include Arduino log wrapper symbols
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=__wrap_esp_log_write"
    "-Wl,--undefined=__wrap_esp_log_writev"
)

# Ensure BACnet MS/TP remains enabled and RS-485 PHY is enabled.
# Restore BOARD_RS485_PHY_ENABLED=1 so GPIO5 is used for MAX485 DE/RE.
target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_BACNET_MSTP_ENABLED=1)
if(CONFIG_DISPLAY_ST7789_240X320)
    # Ensure RS485 PHY enabled even for provisional display (TFT CS moved to GPIO25)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_RS485_PHY_ENABLED=1)
endif()
