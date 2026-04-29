# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius

set(SRCS
    "dtesp.c"
    "diag_mem.c"
    "custom_commands.c"
    "custom_actions.c"
    "dtesp_job_pool.c"
    "dtesp_audio.c"
)

set(REQUIRES
    dectalk
    driver
    esp_adc
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_ledc
    esp_timer
    nvs_flash
    pthread
)

# LEDC is needed for the tone handler.
if(CONFIG_DTESP_FW_CMD_TONE_ENABLE)
    list(APPEND REQUIRES esp_driver_ledc)
endif()

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

if(CONFIG_DTESP_DAC_TLV320)
    list(APPEND SRCS "tlv320.c" "tlv320_dsp.c" "fw_settings.c")
    list(APPEND REQUIRES esp_driver_i2c nvs_flash)
endif()

if(CONFIG_DTESP_VOLUME_KNOB_ENABLE)
    list(APPEND SRCS "volume_knob.c")
    list(APPEND REQUIRES esp_adc esp_timer)
else()
    # Always compile the translation unit so the no-op stubs are
    # available to any caller built unconditionally.
    list(APPEND SRCS "volume_knob.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${REQUIRES}
)
