// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Custom Action Handlers for [:fw ...] Commands
//
// Each handler parses its arguments and returns an dtesp_job_t
// ACTION job that will be executed in order on the speech task.
//
// Handlers:
//   gpio       <pin> <on|off|0|1>         – set a GPIO pin level
//   voice      <name>                      – change DECtalk voice
//   rate       <75..600>                   – change DECtalk speaking rate
//   tone       <freq_hz> <duration_ms>    – play a tone (stub/TODO)
//   volume     <0..9>                      – set codec digital volume
//   profile    <speaker|headphone>         – set codec output profile
//   autoswitch <on|off>                    – enable/disable headset auto-switch
//   save                                   – persist codec settings to NVS
//   bass       <-12..+12>                 – low-shelf tone control (dB)
//   treble     <-12..+12>                 – high-shelf tone control (dB)
//   eq         <1..5> <-12..+12>          – set peaking EQ band gain (dB)
//   eq reset                              – flatten every EQ slot
//   eq show                               – log the current EQ / DRC state
//   eq preset  <flat|speech|crisp|warm>   – load a named preset
//   drc        <on|off>                   – enable/disable DRC
//   drc preset <soft|speech|loud>         – select DRC preset
//   spkgain    <6|12|18|24>               – class-D speaker amp gain (dB)
//   mute       <on|off>                   – soft-mute the codec
//
// Adding a new sub-command:
//   1. Add a `custom_action_<name>()` below that parses argv and
//      returns a job (or NULL on invalid args).
//   2. Add one row to the table in custom_actions.c.
// ----------------------------------------------------------------

#ifndef CUSTOM_ACTIONS_H
#define CUSTOM_ACTIONS_H

#include "dtesp_jobs.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------
// Action handlers.  Each returns a heap-allocated ACTION job, or
// NULL if the arguments are invalid (the caller logs the error).
// argv[0] is the sub-command name (e.g. "gpio").
// ----------------------------------------------------------------

dtesp_job_t *custom_action_gpio(int argc, const char **argv);
dtesp_job_t *custom_action_voice(int argc, const char **argv);
dtesp_job_t *custom_action_rate(int argc, const char **argv);
dtesp_job_t *custom_action_tone(int argc, const char **argv);
dtesp_job_t *custom_action_volume(int argc, const char **argv);
dtesp_job_t *custom_action_profile(int argc, const char **argv);
dtesp_job_t *custom_action_autoswitch(int argc, const char **argv);
dtesp_job_t *custom_action_save(int argc, const char **argv);
dtesp_job_t *custom_action_bass(int argc, const char **argv);
dtesp_job_t *custom_action_treble(int argc, const char **argv);
dtesp_job_t *custom_action_eq(int argc, const char **argv);
dtesp_job_t *custom_action_drc(int argc, const char **argv);
dtesp_job_t *custom_action_spkgain(int argc, const char **argv);
dtesp_job_t *custom_action_mute(int argc, const char **argv);

// ----------------------------------------------------------------
// Single entry point for the tokenizer: look up `argv[0]` in the
// action table and invoke the matching handler.  Returns NULL when
// the sub-command is unknown or the handler rejects the arguments.
//
// Matching is case-insensitive.
// ----------------------------------------------------------------
dtesp_job_t *custom_actions_dispatch(int argc, const char **argv);

// ----------------------------------------------------------------
// Session state
// ----------------------------------------------------------------

// Return the current voice prefix string (e.g. "[:nb]") that
// should be prepended to text jobs, or NULL if no override is set.
const char *custom_action_get_voice_prefix(void);

// Return the current rate prefix string (e.g. "[:ra 200]") that
// should be prepended to text jobs, or NULL if no override is set.
const char *custom_action_get_rate_prefix(void);

// Reset all session-level action state (voice prefix, rate prefix).
void custom_actions_reset_session(void);

#ifdef __cplusplus
}
#endif

#endif // CUSTOM_ACTIONS_H
