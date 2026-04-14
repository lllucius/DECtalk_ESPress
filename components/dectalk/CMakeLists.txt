# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius

set( DECTALK_COMPONENT_REQUIRES
    pthread
    esp_partition
    spiffs
)

# ESP-IDF loads component CMakeLists in script mode first to collect
# dependencies. Defer source-tree resolution until the normal configure pass so
# builds without a pre-configured path can still reach the FetchContent fallback.
if( CMAKE_SCRIPT_MODE_FILE )
    idf_component_register( REQUIRES ${DECTALK_COMPONENT_REQUIRES} )
    return()
endif()

include( FetchContent )

function( resolve_dectalk_source_tree out_src_dir candidate )
    set( resolved "" )
    if( candidate )
        set( source_dir "${candidate}" )
        if( NOT IS_ABSOLUTE "${source_dir}" )
            get_filename_component( source_dir "${source_dir}" ABSOLUTE
                                    BASE_DIR "${CMAKE_CURRENT_LIST_DIR}" )
        endif()

        if( EXISTS "${source_dir}/dapi/src" )
            set( resolved "${source_dir}" )
        elseif( EXISTS "${source_dir}/src/dapi/src" )
            set( resolved "${source_dir}/src" )
        endif()
    endif()

    set( ${out_src_dir} "${resolved}" PARENT_SCOPE )
endfunction()

set( DT_DIR "" )
set( DT_PATH_HINT "${CONFIG_DECTALK_SRC_ROOT}" )

# --- Step 1: try the explicitly configured source path -----------------------
if( DT_PATH_HINT )
    resolve_dectalk_source_tree( DT_DIR "${DT_PATH_HINT}" )
    if( NOT DT_DIR )
        message( WARNING
                 "Configured DECtalk source path '${DT_PATH_HINT}' does not "
                 "contain dapi/src or src/dapi/src.  Trying auto-detection "
                 "and FetchContent fallbacks.  If you see path errors "
                 "after moving the project, delete the sdkconfig file "
                 "and reconfigure." )
    endif()
endif()

# --- Step 2: FetchContent from GitHub ----------------------------------------
if( NOT DT_DIR AND CONFIG_DECTALK_FETCH_SOURCE )
    set( DECTALK_FETCH_GIT_TAG "${CONFIG_DECTALK_FETCH_GIT_TAG}" )
    if( NOT DECTALK_FETCH_GIT_TAG )
        set( DECTALK_FETCH_GIT_TAG "main" )
    endif()

    FetchContent_Declare(
        dectalk_upstream
        GIT_REPOSITORY https://github.com/dectalk/dectalk.git
        GIT_TAG "${DECTALK_FETCH_GIT_TAG}"
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties( dectalk_upstream )
    if( NOT dectalk_upstream_POPULATED )
        if( POLICY CMP0169 )
            cmake_policy( PUSH )
            cmake_policy( SET CMP0169 OLD )
        endif()
        FetchContent_Populate( dectalk_upstream )
        if( POLICY CMP0169 )
            cmake_policy( POP )
        endif()
    endif()
    resolve_dectalk_source_tree( DT_DIR "${dectalk_upstream_SOURCE_DIR}" )
endif()

# --- Final check -------------------------------------------------------------
# When the source tree is not found, register a minimal (empty) component and
# defer the error to build time via fail_at_build_time().  This keeps
# "idf.py menuconfig" working so the user can fix paths or enable the
# FetchContent fallback without being locked out by a configure-time fatal.
if( NOT DT_DIR )
    set( _dt_msgs )
    list( APPEND _dt_msgs "=====================================================" )
    list( APPEND _dt_msgs "Could not locate the DECtalk source tree." )
    if( DT_PATH_HINT )
        list( APPEND _dt_msgs
              "  Configured path '${DT_PATH_HINT}' is invalid." )
    endif()
    if( NOT CONFIG_DECTALK_FETCH_SOURCE )
        list( APPEND _dt_msgs
              "  GitHub FetchContent fallback is disabled." )
    endif()
    list( APPEND _dt_msgs
          ""
          "Solutions:"
          "  - Set a valid 'Path to DECtalk library source tree' in"
          "    DECtalk ESPress Configuration -> DECtalk distribution"
          "  - Enable 'Fetch DECtalk source from GitHub' in the same menu"
          "  - If you see path errors after moving the project,"
          "    delete the sdkconfig file and run idf.py menuconfig again"
          "=====================================================" )

    # Show warnings during configure so the user sees the issue immediately.
    message( WARNING "DECtalk source tree not found. "
                     "Run 'idf.py menuconfig' to configure." )

    # Register a minimal component so the rest of the build system proceeds.
    idf_component_register( REQUIRES ${DECTALK_COMPONENT_REQUIRES} )

    # Defer the hard error to build time so menuconfig remains accessible.
    fail_at_build_time( dectalk_missing_sources ${_dt_msgs} )

    return()
endif()

# Path to the DECTalk API source
set( DAPI_DIR "${DT_DIR}/dapi/src" )

# Path to the DECtalk dictionary source directory
set( DIC_DIR "${DAPI_DIR}/dic" )

# Map Kconfig language selection to compiler defines and lang code.
# Only one language is active at a time.
if( CONFIG_DECTALK_LANG_US )
    set( LANG_CODE "us" )
    set( LANG_DEFS ENGLISH ENGLISH_US ACNA )
elseif( CONFIG_DECTALK_LANG_UK )
    set( LANG_CODE "uk" )
    set( LANG_DEFS ENGLISH ENGLISH_UK )
elseif( CONFIG_DECTALK_LANG_SP )
    set( LANG_CODE "sp" )
    set( LANG_DEFS SPANISH SPANISH_SP )
elseif( CONFIG_DECTALK_LANG_GR )
    set( LANG_CODE "gr" )
    set( LANG_DEFS GERMAN )
elseif( CONFIG_DECTALK_LANG_LA )
    set( LANG_CODE "la" )
    set( LANG_DEFS SPANISH SPANISH_LA )
elseif( CONFIG_DECTALK_LANG_FR )
    set( LANG_CODE "fr" )
    set( LANG_DEFS FRENCH )
else()
    set( LANG_CODE "us" )
    set( LANG_DEFS ENGLISH ENGLISH_US ACNA )
endif()

# Define dictionary related paths
set( DIC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/dicts" )
set( DIC_COMPILER "${DIC_OUTPUT_DIR}/dic_${LANG_CODE}" )
set( DIC_OUTPUT "${DIC_OUTPUT_DIR}/dtalk_${LANG_CODE}.dic" )
set( DIC_SOURCE_TXT "${DIC_DIR}/Dic_${LANG_CODE}.txt" )

# Source files for the DECtalk library
set( API_SRCS
    ${DAPI_DIR}/api/ttsapi.c
    ${DAPI_DIR}/api/decstd97.c
    ${DAPI_DIR}/api/init.c
    ${DAPI_DIR}/api/crypt2.c
)

set( CMD_SRCS
    ${DAPI_DIR}/cmd/cm_char.c
    ${DAPI_DIR}/cmd/cm_main.c
    ${DAPI_DIR}/cmd/cm_text.c
    ${DAPI_DIR}/cmd/cm_pars.c
    ${DAPI_DIR}/cmd/cm_cmd.c
    ${DAPI_DIR}/cmd/cm_util.c
    ${DAPI_DIR}/cmd/cm_phon.c
    ${DAPI_DIR}/cmd/cm_copt.c
    ${DAPI_DIR}/cmd/par_pars.c
    ${DAPI_DIR}/cmd/par_ambi.c
    ${DAPI_DIR}/cmd/par_dict.c
    ${DAPI_DIR}/cmd/par_rule.c
    ${DAPI_DIR}/cmd/par_char.c
    ${DAPI_DIR}/cmd/cmd_wav.c
    ${DAPI_DIR}/cmd/cmd_init.c
)

set( LTS_SRCS
    ${DAPI_DIR}/lts/lsa_adju.c
    ${DAPI_DIR}/lts/lsa_coni.c
    ${DAPI_DIR}/lts/lsa_fr.c
    ${DAPI_DIR}/lts/lsa_gr.c
    ${DAPI_DIR}/lts/lsa_ir.c
    ${DAPI_DIR}/lts/lsa_it.c
    ${DAPI_DIR}/lts/lsa_ja.c
    ${DAPI_DIR}/lts/lsa_rtbi.c
    ${DAPI_DIR}/lts/lsa_rule.c
    ${DAPI_DIR}/lts/lsa_sl.c
    ${DAPI_DIR}/lts/lsa_sp.c
    ${DAPI_DIR}/lts/lsa_task.c
    ${DAPI_DIR}/lts/lsa_us.c
    ${DAPI_DIR}/lts/lsa_util.c
    ${DAPI_DIR}/lts/lsw_main.c
    ${DAPI_DIR}/lts/ls_chari.c
    ${DAPI_DIR}/lts/ls_dict.c
    ${DAPI_DIR}/lts/ls_homo.c
    ${DAPI_DIR}/lts/ls_math.c
    ${DAPI_DIR}/lts/ls_proc.c
    ${DAPI_DIR}/lts/ls_spel.c
    ${DAPI_DIR}/lts/ls_speli.c
    ${DAPI_DIR}/lts/ls_suff.c
    ${DAPI_DIR}/lts/ls_suffi.c
    ${DAPI_DIR}/lts/loaddict.c
)

set( PH_SRCS
    ${DAPI_DIR}/ph/ph_aloph.c
    ${DAPI_DIR}/ph/ph_claus.c
    ${DAPI_DIR}/ph/ph_draw.c
    ${DAPI_DIR}/ph/ph_drwt0.c
    ${DAPI_DIR}/ph/ph_inton.c
    ${DAPI_DIR}/ph/ph_main.c
    ${DAPI_DIR}/ph/ph_romi.c
    ${DAPI_DIR}/ph/ph_setar.c
    ${DAPI_DIR}/ph/ph_sort.c
    ${DAPI_DIR}/ph/ph_syl.c
    ${DAPI_DIR}/ph/ph_syntx.c
    ${DAPI_DIR}/ph/ph_task.c
    ${DAPI_DIR}/ph/ph_timng.c
    ${DAPI_DIR}/ph/ph_vdefi.c
    ${DAPI_DIR}/ph/ph_vset.c
    ${DAPI_DIR}/ph/phinit.c
    ${DAPI_DIR}/ph/phlog.c
    ${DAPI_DIR}/ph/phprint.c
)

set( KERNEL_SRCS
    ${DAPI_DIR}/kernel/services.c
    ${DAPI_DIR}/kernel/usa_init.c
)

set( NT_SRCS
    ${DAPI_DIR}/nt/opthread.c
    ${DAPI_DIR}/nt/spc.c
    ${DAPI_DIR}/nt/pipe.c
)

set( OSF_SRCS
    ${DAPI_DIR}/osf/playstub.c
    ${DAPI_DIR}/osf/dtmmio.c
)

set( VTM_SRCS
    ${DAPI_DIR}/vtm/vtm.c
    ${DAPI_DIR}/vtm/vtmiont.c
    ${DAPI_DIR}/vtm/sync.c
    ${DAPI_DIR}/vtm/playtone.c
)

set( HLSYN_SRCS
    ${DAPI_DIR}/hlsyn/acxf1c.c
    ${DAPI_DIR}/hlsyn/hlframe.c
    ${DAPI_DIR}/hlsyn/log10table.c
    ${DAPI_DIR}/hlsyn/brent.c
    ${DAPI_DIR}/hlsyn/inithl.c
    ${DAPI_DIR}/hlsyn/nasalf1x.c
    ${DAPI_DIR}/hlsyn/sqrttable.c
    ${DAPI_DIR}/hlsyn/circuit.c
)

set( LOCAL_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/src/libc_stubs.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/loaddict_wrappers.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/thread_wrappers.c"
)

set( INCLUDE_DIRS
    ${DT_DIR}
    ${DAPI_DIR}/include
    ${DAPI_DIR}/api
    ${DAPI_DIR}/cmd
    ${DAPI_DIR}/dic
    ${DAPI_DIR}/hlsyn
    ${DAPI_DIR}/kernel
    ${DAPI_DIR}/lts
    ${DAPI_DIR}/nt
    ${DAPI_DIR}/osf
    ${DAPI_DIR}/ph
    ${DAPI_DIR}/protos
    ${DAPI_DIR}/vtm
)

# ---------------------------------------------------------------------------
# Dictionary build - uses the host C compiler to compile dic.c into a
# language-specific dictionary compiler, then runs it on the source .txt
# to produce the .dic binary. The language is selected via menuconfig
# (see Kconfig.projbuild in this directory).
#
# This must happen before idf_component_register() if the output will be
# listed in EMBED_FILES, because ESP-IDF validates embedded file paths during
# component registration/generate time.
# ---------------------------------------------------------------------------

set( DIC_DEFINES
    ${LANG_DEFS}
    ACCESS32
    ANSI
    BLD_DECTALK_DLL
    LTSSIM
    TTSSIM
    TYPING_MODE
)
list( TRANSFORM DIC_DEFINES PREPEND "-D" )

set( DIC_INCLUDES ${INCLUDE_DIRS} )
list( TRANSFORM DIC_INCLUDES PREPEND "-I" )

# Build the dictionary compiler for the host using the host CC.
# We explicitly use "cc" (not CMAKE_C_COMPILER) because CMAKE_C_COMPILER
# is the ESP32 cross-compiler; the dictionary compiler must run on the host.
find_program( HOST_CC cc )

if( NOT HOST_CC )
    find_program( HOST_CC gcc )
endif()

if( NOT HOST_CC )
    message( FATAL_ERROR "No host C compiler found (tried cc, gcc). "
                         "Set HOST_CC to your host compiler." )
endif()

# Tell the host compiler to look for sub-programs (as, ld, …) in the same
# directory where it lives.  Inside the ESP-IDF Docker image the Xtensa
# cross-toolchain's "as" appears on the PATH before the system assembler,
# so the host GCC would otherwise invoke the wrong assembler and fail with
# "as: unrecognized option '--64'".
get_filename_component( HOST_CC_DIR "${HOST_CC}" DIRECTORY )

function( generate_dict_at_configure )
    file( MAKE_DIRECTORY "${DIC_OUTPUT_DIR}" )

    execute_process(
        COMMAND
            ${HOST_CC}
                "-B${HOST_CC_DIR}"
                ${DIC_INCLUDES}
                ${DIC_DEFINES}
                -o "${DIC_COMPILER}" "${DIC_DIR}/dic.c"
        RESULT_VARIABLE
            dic_cc_result
    )
    if( NOT dic_cc_result EQUAL 0 )
        message( FATAL_ERROR "Failed to build dictionary compiler: ${DIC_COMPILER}" )
    endif()

    execute_process(
        COMMAND
            ${DIC_COMPILER}
                "${DIC_SOURCE_TXT}"
                "dtalk_${LANG_CODE}.dic"
        WORKING_DIRECTORY
            "${DIC_OUTPUT_DIR}"
        RESULT_VARIABLE
            dic_build_result
    )
    if( NOT dic_build_result EQUAL 0 )
        message( FATAL_ERROR "Failed to build dictionary output: ${DIC_OUTPUT}" )
    endif()
endfunction()

# Ensure the dictionary exists before component registration if it will be
# embedded or flashed to a partition. This fixes clean builds where the
# build directory does not exist yet.
if( CONFIG_DECTALK_DICT_EMBED OR CONFIG_DECTALK_DICT_PART OR CONFIG_DECTALK_DICT_FILE )
    generate_dict_at_configure()
endif()

# Define the list of embedded dictionary files (if any)
set( EMBED_FILES )
if( CONFIG_DECTALK_DICT_EMBED )
    list( APPEND EMBED_FILES ${DIC_OUTPUT} )
endif()

# Register the main component
idf_component_register(
    SRCS
        ${API_SRCS}
        ${CMD_SRCS}
        ${LTS_SRCS}
        ${PH_SRCS}
        ${KERNEL_SRCS}
        ${NT_SRCS}
        ${OSF_SRCS}
        ${VTM_SRCS}
        ${HLSYN_SRCS}
        ${LOCAL_SRCS}
    INCLUDE_DIRS
        "include"
        ${INCLUDE_DIRS}
    EMBED_FILES
        ${EMBED_FILES}
    REQUIRES
        ${DECTALK_COMPONENT_REQUIRES}
)

target_compile_definitions(
    ${COMPONENT_LIB}
    PUBLIC
        ${LANG_DEFS}
        LANG_CODE=${LANG_CODE}
        ANSI=1
        BLD_DECTALK_DLL=1
        LTSSIM=1
        TTSSIM=1
        TYPING_MODE=1
        $<$<COMPILE_LANGUAGE:C>:__linux__=1>
)

target_compile_options(
    ${COMPONENT_LIB}
    PUBLIC
    PRIVATE
        -Wno-address
        -Wno-char-subscripts
        -Wno-comment
        -Wno-cpp
        -Wno-deprecated-declarations
        -Wno-empty-body
        -Wno-error=address
        -Wno-error=bool-compare
        -Wno-error=char-subscripts
        -Wno-error=comment
        -Wno-error=dangling-else
        -Wno-error=format
        -Wno-error=implicit-function-declaration
        -Wno-error=logical-not-parentheses
        -Wno-error=maybe-uninitialized
        -Wno-error=misleading-indentation
        -Wno-error=missing-braces
        -Wno-error=parentheses
        -Wno-error=pointer-sign
        -Wno-error=return-type
        -Wno-error=switch
        -Wno-error=tautological-compare
        -Wno-error=uninitialized
        -Wno-error=unused-but-set-parameter
        -Wno-error=unused-label
        -Wno-format
        -Wno-implicit-fallthrough
        -Wno-incompatible-pointer-types
        -Wno-int-to-pointer-cast
        -Wno-maybe-uninitialized
        -Wno-misleading-indentation
        -Wno-missing-braces
        -Wno-parentheses
        -Wno-pointer-sign
        -Wno-pointer-to-int-cast
        -Wno-return-type
        -Wno-tautological-compare
        -Wno-type-limits
        -Wno-uninitialized
        -Wno-unused-but-set-parameter
        -Wno-unused-but-set-variable
        -Wno-unused-function
        -Wno-unused-variable
        -fgnu89-inline
)

target_link_libraries(
    ${COMPONENT_LIB}
    INTERFACE
        "-Wl,--wrap=load_dictionary"
        "-Wl,--wrap=unload_dictionary"
)

# Rename the upstream OP_CreateThread() in opthread.c so our replacement in
# thread_wrappers.c is the sole definition.  The other functions in opthread.c
# (OP_CreateMutex, OP_CreateEvent, etc.) are unaffected.
set_source_files_properties(
    ${DAPI_DIR}/nt/opthread.c
    PROPERTIES COMPILE_DEFINITIONS "OP_CreateThread=_upstream_OP_CreateThread"
)

#
# Ensures any existing partitions are large enough and not missing. While doing
# this, we build a list of targets that will be defined to flash the resource
# files to the partition.
#
function( check_part lang type file )
    string( TOLOWER "${type}" type )

    # Get dictionary file size and calculate size of flash needed (rounded up to block size)
    file( SIZE "${file}" req )
    math( EXPR req "(((${req} + 4095) / 4096) * 4096)" )

    # Determine if we're supposed to automatically add the partitions to the partition table
    if( CONFIG_DECTALK_AUTOCREATE_PARTITIONS )
        # Check to see if already exists
        partition_table_get_partition_info( name
            "--partition-type data --partition-subtype ${type}"
            "name"
        )

        if( name )
            list( APPEND messages "    partition with subtype of ${type} already exists" )
        else()
            # Remember this new partition. It will be added to the partitions table below.
            set( name "${type}" )
            set( partitions "${partitions};${name},data,${type},,${req}\n" PARENT_SCOPE )
        endif()
    # Not added, so ensure predefined partitions exist in partition table
    else()
        # Retrieve predefined partition info
        partition_table_get_partition_info( name
            "--partition-type data --partition-subtype ${type}"
            "name"
        )
        partition_table_get_partition_info( offset
            "--partition-type data --partition-subtype ${type}"
            "offset"
        )
        partition_table_get_partition_info( size
            "--partition-type data --partition-subtype ${type}"
            "size"
        )

        # Ensure we have an offset AND size
        if( NOT offset OR NOT size )
            list( APPEND messages "    partition with subtype of ${type} is required" )
        endif()

        # Ensure the partitions is big enough to hold the resource file
        if( size LESS req )
            list( APPEND messages "    ${name} partition must be at least ${req} bytes" )
        endif()
    endif()

    # Remember new target
    list( APPEND flash_targets "${name}" "${type}" "${file}" )

    # Propogate new values to the caller
    set( flash_targets "${flash_targets}" PARENT_SCOPE )
    set( messages "${messages}" PARENT_SCOPE )
endfunction()

# Load the list of predefined partitions
if( CONFIG_DECTALK_AUTOCREATE_PARTITIONS OR CONFIG_DECTALK_DICT_FILE_AUTOCREATE_PARTITION )
    file( STRINGS "${PARTITION_CSV_PATH}" partitions )
endif()

# Check partitions for dictionary
if( CONFIG_DECTALK_DICT_PART )
    check_part( ${LANG_CODE} "udict" "${DIC_OUTPUT}" )
endif()

# Auto-create SPIFFS dictionary partition when using file-system storage
if( CONFIG_DECTALK_DICT_FILE AND CONFIG_DECTALK_DICT_FILE_AUTOCREATE_PARTITION )
    # Check if a "dictionary" spiffs partition already exists
    partition_table_get_partition_info( _spiffs_name
        "--partition-type data --partition-subtype spiffs"
        "name"
    )
    if( NOT _spiffs_name )
        # Calculate required size: dictionary file size + 50 % SPIFFS overhead,
        # rounded up to the nearest 64 KiB (65536 bytes) boundary, with a
        # 256 KiB (262144 bytes) minimum to allow room for filesystem metadata.
        file( SIZE "${DIC_OUTPUT}" _dic_sz )
        math( EXPR _spiffs_sz "((${_dic_sz} * 3 / 2 + 65535) / 65536) * 65536" )
        if( _spiffs_sz LESS 262144 )
            set( _spiffs_sz 262144 )
        endif()
        set( partitions "${partitions};dictionary,data,spiffs,,${_spiffs_sz}\n" )
    endif()
endif()

# If any problems were found, create a target to fail the build. This is done
# prevent locking the user out of "menuconfig".
if( messages )
    list( PREPEND messages "Problem(s) with DECtalk dictionary file:" )
    list( PREPEND messages "=====================================================" )
    list( APPEND messages "=====================================================" )
    fail_at_build_time( missing_dict_partitions ${messages})
else()
    # Add new partitions to the table and write them out
    if( partitions )
        string( REPLACE ";" "\n" partitions "${partitions}" )
        file( WRITE "${extra_parts}" "${partitions}" )
    endif()

    # Process flash target definitions
    if( flash_targets )
        while( flash_targets )
            # Get target info and remove from list
            list( POP_FRONT flash_targets name type file )

            # Create the target name
            string( REPLACE "_" "-" target "${type}-flash" )

            # Add it to the information message on what to flash
            list( APPEND cmds COMMAND ${CMAKE_COMMAND} -E echo "    idf.py ${target}" )

            # Create the CMake targets for flashing this partition
            esptool_py_flash_target( "${target}" "${main_args}" "${sub_args}" ALWAYS_PLAINTEXT )
            esptool_py_flash_to_partition( "${target}" "${name}" "${file}" )

            # And add it to the general flash target if the user wants it to be automatic
            if( CONFIG_DECTALK_FLASH_IN_PROJECT )
                esptool_py_flash_to_partition( flash "${name}" "${file}" )
            endif()
        endwhile()

        # Define target that shows commands to use when flashing the dictionary
        # file. This will (should) run at the end of the build so the user won't
        # miss it.
        add_custom_command(
            OUTPUT
                "Dictionary Flash Commands"
            COMMAND
                ${CMAKE_COMMAND} -E echo ""
            COMMAND
                ${CMAKE_COMMAND} -E echo "====================================================="
            COMMAND
                ${CMAKE_COMMAND} -E echo "To flash dictionary files, use:"
            COMMAND
                ${cmds}
            COMMAND
                ${CMAKE_COMMAND} -E echo "====================================================="
            DEPENDS
                bootloader app_check_size
            VERBATIM
        )

        add_custom_target(rebuild-partition-table ALL
            DEPENDS
                "Dictionary Flash Commands"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Create and optionally flash a SPIFFS partition image containing the
# dictionary file when using the file-system storage mode.
# ---------------------------------------------------------------------------
if( CONFIG_DECTALK_DICT_FILE )
    # Determine the source directory for the SPIFFS image.
    # An empty DECTALK_DICT_FILE_IMAGE_DIR defaults to the build
    # directory where the dictionary is generated automatically.
    # Relative paths are resolved against the project root.
    if( CONFIG_DECTALK_DICT_FILE_IMAGE_DIR AND NOT CONFIG_DECTALK_DICT_FILE_IMAGE_DIR STREQUAL "" )
        set( DICTIONARY_IMAGE_DIR "${CONFIG_DECTALK_DICT_FILE_IMAGE_DIR}" )
        if( NOT IS_ABSOLUTE "${DICTIONARY_IMAGE_DIR}" )
            get_filename_component( DICTIONARY_IMAGE_DIR "${DICTIONARY_IMAGE_DIR}" ABSOLUTE
                                    BASE_DIR "${PROJECT_DIR}" )
        endif()
    else()
        set( DICTIONARY_IMAGE_DIR "${DIC_OUTPUT_DIR}" )
    endif()

    if( CONFIG_DECTALK_DICT_FILE_FLASH_WITH_APP )
        spiffs_create_partition_image( dictionary "${DICTIONARY_IMAGE_DIR}" FLASH_IN_PROJECT )
    else()
        spiffs_create_partition_image( dictionary "${DICTIONARY_IMAGE_DIR}" )
    endif()
endif()
