# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius

#
# Declare extra partition subtypes that we'll use for the resource files
#
idf_build_set_property( EXTRA_PARTITION_SUBTYPES "data, udict, 0x40" APPEND )

#
# Copy the original partitions CSV in case we need to add partitions. Override
# the original path to force usage of this one.
#
idf_build_get_property( build_dir BUILD_DIR )
set( extra_parts "${build_dir}/extra_parts.csv" )

file( COPY_FILE "${PARTITION_CSV_PATH}" "${extra_parts}" )

set( PARTITION_CSV_PATH "${extra_parts}" CACHE INTERNAL "extra partitions" FORCE )
set( PARTITION_CSV_PATH "${extra_parts}" )

