# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius

idf_component_register(
    SRCS
        "dectalk_espress.c"
        "usb_cdc_transport.c"
        "diag_mem.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        dectalk
        driver
        pthread
        esp_timer
        esp_driver_i2s
)
