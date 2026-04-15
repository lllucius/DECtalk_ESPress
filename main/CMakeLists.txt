# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius

set(SRCS
    "dectalk_espress.c"
    "usb_cdc_transport.c"
    "diag_mem.c"
)

set(REQUIRES
    dectalk
    driver
    pthread
    esp_timer
    esp_driver_i2s
)

if(CONFIG_DECTALK_DAC_TLV320DAC3100)
    list(APPEND SRCS "tlv320dac3100.c")
    list(APPEND REQUIRES esp_driver_i2c)
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${REQUIRES}
)
