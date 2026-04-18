// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Firmware Commands — [:fw ...] command parser
//
// Scans text destined for the TTS engine, extracts any [:fw ...]
// command sequences, dispatches them through a table-driven handler,
// and strips the consumed sequences from the text so the remaining
// content can be spoken normally.
//
// The command table is easily extended: add a new row to the static
// dispatch table in fw_commands.c and implement the handler.
// ----------------------------------------------------------------

#ifndef FW_COMMANDS_H
#define FW_COMMANDS_H

#include <stdbool.h>

/**
 * @brief Initialise the fw command subsystem.
 *
 * Must be called once at startup (after fw_settings_init()).
 */
void fw_commands_init(void);

/**
 * @brief Scan text for [:fw ...] commands and process them.
 *
 * Any recognised command sequences are dispatched to their handlers
 * and removed from @p text in-place.  The caller should check whether
 * any speakable text remains before passing the result to the TTS
 * engine.
 *
 * @param text  Mutable NUL-terminated text buffer.
 * @return true if one or more fw commands were found and processed.
 */
bool fw_commands_process_text(char *text);

#endif // FW_COMMANDS_H
