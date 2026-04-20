# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius

# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly

cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# GNU ld --wrap rewrites unresolved references at the final app/ELF link, so
# attach it at the ESP-IDF build level instead of only on a component archive.
#idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=load_dictionary" APPEND)

project(dtesp)
