// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// GNU ld Symbol Wrapping for Dictionary Loading
//
// This file is intentionally local to the ESP-IDF project so we can
// override DECtalk behavior without modifying upstream sources or
// copying loaddict.c.
//
// Uses the GNU linker --wrap mechanism to intercept the upstream
// non-WIN32 load_dictionary() and unload_dictionary() calls and
// redirect them to ESP32-specific storage backends:
//
//   CONFIG_DECTALK_DICT_FILE  - SPIFFS filesystem (runtime mount)
//   CONFIG_DECTALK_DICT_EMBED - binary embedded in firmware image
//   CONFIG_DECTALK_DICT_PART  - dedicated flash partition (mmap'd)
//
// Requires the final link to include:
//   -Wl,--wrap=load_dictionary
//   -Wl,--wrap=unload_dictionary
// ----------------------------------------------------------------

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "esp_log.h"
#if defined(CONFIG_DECTALK_DICT_PART)
#include "esp_partition.h"
#endif
#if defined(CONFIG_DECTALK_DICT_FILE)
#include "esp_spiffs.h"
#endif

#include "dectalkf.h"
#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "dtmmedefs.h"
#include "opthread.h"
#include <unistd.h>
#include "ls_def.h"

static const char *TAG = "DECtalk Wrapper";

// ----------------------------------------------------------------
// load_dictionary()
//
// Wrapped replacement for the upstream load_dictionary().  Reads the
// dictionary header (entry count + byte count), allocates index and
// data buffers, and populates them from the configured storage
// backend (SPIFFS file, embedded binary, or mmap'd partition).
//
// Arguments:
//   dict_index       - receives pointer to the index array
//   dict_data        - receives pointer to the data block
//   dict_siz         - receives number of dictionary entries
//   dict_bytes       - receives byte size of dictionary data
//   dict_nam         - bare file name of the dictionary
//   bRequired        - if true, missing dictionary is an error
//   dicMapObject     - partition mmap handle (partition mode)
//   dicFileHandle    - unused (kept for signature compat)
//   dicMapStartAddr  - receives mmap base address (partition mode)
//   dict_map         - memory map type (unused on ESP32)
//
// Returns:
//   MMSYSERR_NOERROR    - success
//   MMSYSERR_ERROR      - I/O or mount error
//   MMSYSERR_INVALPARAM - path too long or file not found
//   MMSYSERR_NOMEM      - allocation failure
// ----------------------------------------------------------------
int __wrap_load_dictionary(void **dict_index,
                           void **dict_data,
                           unsigned int *dict_siz,
                           unsigned int *dict_bytes,
                           char *dict_nam,
                           int bRequired,
                           DT_HANDLE *dicMapObject,
                           DT_HANDLE *dicFileHandle,
                           LPVOID *dicMapStartAddr,
                           MEMMAP_T dict_map)
{
    S32 *dict_index_buffer;
    unsigned char *dict_data_buffer;
    S32 entries;
    S32 bytes;
    S32 pointer_list_size;

    // set error return values
    if (*dict_siz > 0)
    {
        return MMSYSERR_ERROR;
    }

    *dict_siz = 0;
    *dict_bytes = 0;
    dict_index_buffer = NULL;
    dict_data_buffer = NULL;

#if defined(CONFIG_DECTALK_DICT_FILE)

    // Auto-mount the SPIFFS dictionary partition if it isn't mounted yet.
    if (!esp_spiffs_mounted("dictionary"))
    {
        ESP_LOGI(TAG, "Mounting SPIFFS dictionary partition...");
        esp_vfs_spiffs_conf_t spiffs_conf =
        {
            .base_path = CONFIG_DECTALK_DICT_ROOT,
            .partition_label = "dictionary",
            .max_files = 5,
            .format_if_mount_failed = false,
        };

        esp_err_t spiffs_ret = esp_vfs_spiffs_register(&spiffs_conf);
        if (spiffs_ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to mount SPIFFS dictionary partition (%s)",
                     esp_err_to_name(spiffs_ret));
            return MMSYSERR_ERROR;
        }

        size_t total = 0;
        size_t used = 0;
        esp_spiffs_info(spiffs_conf.partition_label, &total, &used);
        ESP_LOGI(TAG, "SPIFFS mounted at %s (total: %zu, used: %zu)",
                 CONFIG_DECTALK_DICT_ROOT,
                 total,
                 used);
    }

    // Build the full path from CONFIG_DECTALK_DICT_ROOT and the bare
    // dictionary file name supplied by the caller.
    char dict_path[256];
    int path_len = snprintf(dict_path,
                            sizeof(dict_path),
                            "%s/%s",
                            CONFIG_DECTALK_DICT_ROOT,
                            dict_nam);
    if (path_len < 0 || (size_t)path_len >= sizeof(dict_path))
    {
        ESP_LOGE(TAG, "Dictionary path too long: %s/%s",
                 CONFIG_DECTALK_DICT_ROOT,
                 dict_nam);
        return MMSYSERR_INVALPARAM;
    }

    FILE *dict_file = fopen(dict_path, "rb");
    if (dict_file == NULL)
    {
        if (bRequired)
        {
            ESP_LOGE(TAG, "Failed to open dictionary file %s", dict_path);
            return MMSYSERR_INVALPARAM;

        }
        return MMSYSERR_NOERROR;
    }

    // Read in file header
    if (fread(&entries, 4, 1, dict_file) != 1)
    {
        ESP_LOGE(TAG, "Error reading dictionary database %s", dict_path);
        fclose(dict_file);
        return MMSYSERR_ERROR;
    }

    if (entries == 0)
    {
        fclose(dict_file);
        return MMSYSERR_NOERROR;
    }

    if (fread(&bytes, 4, 1, dict_file) != 1)
    {
        ESP_LOGE(TAG, "Error reading dictionary database %s", dict_path);
        fclose(dict_file);
        return MMSYSERR_ERROR;
    }

    pointer_list_size = (entries * sizeof(S32));

    dict_index_buffer = (S32 *) malloc(pointer_list_size);
    if (dict_index_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate %zd bytes of memory to load dictionary", pointer_list_size);
        fclose(dict_file);
        return MMSYSERR_NOMEM;
    }

    dict_data_buffer = (unsigned char *) malloc(bytes + 1);
    if (dict_data_buffer == NULL)
    {
        free(dict_index_buffer);
        ESP_LOGE(TAG, "Failed to allocate %zd bytes of memory to load dictionary", bytes + 1);
        fclose(dict_file);
        return MMSYSERR_NOMEM;
    }

    // read in the index table
    if (fread(dict_index_buffer, 4, entries, dict_file) != (unsigned)entries)
    {
        ESP_LOGE(TAG, "Error reading dictionary indexes: %s", dict_path);
        free(dict_data_buffer);
        free(dict_index_buffer);
        fclose(dict_file);
        return MMSYSERR_ERROR;
    }

    // Read in the rest of the dictionary
    if (fread(dict_data_buffer, bytes, 1, dict_file) != 1)
    {
        ESP_LOGE(TAG, "Error reading dictionary data: %s", dict_path);
        free(dict_data_buffer);
        free(dict_index_buffer);
        fclose(dict_file);
        return MMSYSERR_ERROR;
    }

    fclose(dict_file);

#else

    S32 *dict_mem = NULL;

#if defined(CONFIG_DECTALK_DICT_EMBED)

    // Build the linker symbol for the embedded dictionary binary.
    // ESP-IDF's EMBED_FILES generates symbols named
    //   _binary_<filename_with_dots_as_underscores>_start
    // The dictionary file is dtalk_<LANG_CODE>.dic, so the symbol is
    //   _binary_dtalk_<LANG_CODE>_dic_start
    // LANG_CODE is set by CMake as a compile definition (us, uk, sp, ...).
#define _DIC_PASTE(a, b, c) a ## b ## c
#define _DIC_SYM(lang)      _DIC_PASTE(_binary_dtalk_, lang, _dic_start)
#define _DIC_STR2(x)        #x
#define _DIC_STR(x)         _DIC_STR2(x)
#define DIC_SYMBOL_NAME     _DIC_STR(_DIC_SYM(LANG_CODE))

    extern const uint8_t dtalk_dict[] asm(DIC_SYMBOL_NAME);
    dict_mem = (S32 *) dtalk_dict;

#elif defined(CONFIG_DECTALK_DICT_PART)

    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_ANY,
                                 "udict");
    if (partition == NULL)
    {
        ESP_LOGE(TAG, "Failed to locate dictionary partition");
        return MMSYSERR_ERROR;
    }

    esp_err_t err = esp_partition_mmap(partition, 
                                       0,
                                       partition->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       dicMapStartAddr,
                                       dicMapObject);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to map dictionary partition");
        return MMSYSERR_ERROR;
    }

    dict_mem = (S32 *) *dicMapStartAddr;

#else

#error "Define one of DECTALK_DICT_EMBED, DECTALK_DICT_PART, or DECTALK_DICT_FILE"

#endif

    entries = dict_mem[0];
    bytes = dict_mem[1];

    pointer_list_size = entries * sizeof(S32);

    dict_index_buffer = &dict_mem[2];
    dict_data_buffer = &((unsigned char *)dict_index_buffer)[pointer_list_size];

#endif

    // write output parameters
    *dict_index = dict_index_buffer;
    *dict_data = dict_data_buffer;
    *dict_siz = entries;
    *dict_bytes = bytes;

    return MMSYSERR_NOERROR;
}

// ----------------------------------------------------------------
// unload_dictionary()
//
// Wrapped replacement for the upstream unload_dictionary().  Frees
// resources allocated by __wrap_load_dictionary():
//   - SPIFFS file mode: frees the heap-allocated index and data
//   - Partition mode:   unmaps the flash partition
//   - Embedded mode:    nothing to free (data lives in .rodata)
//
// Arguments:
//   dict_index       - pointer to the index array to release
//   dict_data        - pointer to the data block to release
//   dict_siz         - entry count (unused here)
//   dict_bytes       - byte count (unused here)
//   dicMapStartAddr  - mmap base address to unmap (partition mode)
//   dicMapObject     - mmap handle to release (partition mode)
//   dicFileHandle    - unused (kept for signature compat)
//   dict_map         - memory map type (unused on ESP32)
// ----------------------------------------------------------------
void __wrap_unload_dictionary(void **dict_index,
                              void **dict_data,
                              unsigned int *dict_siz,
                              unsigned int * dict_bytes,
                              LPVOID *dicMapStartAddr,
                              DT_HANDLE *dicMapObject,
                              DT_HANDLE *dicFileHandle,
                              MEMMAP_T dict_map)
{

#if defined(CONFIG_DECTALK_DICT_FILE)

    free(*dict_data);
    free(*dict_index);    

#elif defined(CONFIG_DECTALK_DICT_PART)

    esp_partition_munmap((esp_partition_mmap_handle_t) *dicMapObject);

#endif

    *dicMapStartAddr=NULL;
    *dicMapObject=0;
    *dicFileHandle=0;
}

