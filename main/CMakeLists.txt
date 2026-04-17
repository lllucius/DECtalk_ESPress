# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius

set(SRCS
    "dectalk_espress.c"
    "diag_mem.c"
    "custom_commands.c"
    "custom_actions.c"
)

set(REQUIRES
    dectalk
    driver
    pthread
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_gpio
    esp_timer
)

# Select the serial transport layer based on the target chip.
# ESP32-S3 has native USB-OTG and uses TinyUSB CDC-ACM.
# Chips with a built-in USB Serial/JTAG controller (e.g. ESP32-C6)
# use the lighter-weight usb_serial_jtag driver instead.
idf_build_get_property(__target IDF_TARGET)
if(__target STREQUAL "esp32c6")
    list(APPEND SRCS "jtag_serial_transport.c")
    list(APPEND REQUIRES esp_driver_usb_serial_jtag)
else()
    list(APPEND SRCS "usb_cdc_transport.c")
endif()

if(CONFIG_DECTALK_DAC_TLV320DAC3100)
    list(APPEND SRCS "tlv320dac3100.c")
    list(APPEND REQUIRES esp_driver_i2c)
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${REQUIRES}
)
