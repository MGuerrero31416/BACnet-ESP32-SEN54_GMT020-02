idf_component_register(
    SRCS
        "sen54.c"
        "sen54_temperature_compensation.c"
        "sensirion/sen5x_i2c.c"
        "sensirion/sensirion_common.c"
        "sensirion/sensirion_i2c.c"
        "sensirion/sensirion_i2c_hal.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "sensirion"
    REQUIRES driver freertos
)
