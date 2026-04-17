// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Custom Action Handlers for [:fw ...] Commands
//
// Each handler parses its arguments and returns an espress_job_t
// ACTION job that will be executed in order on the speech task.
//
// Handlers:
//   gpio  <pin> <on|off|0|1>        – set a GPIO pin level
//   voice <name>                     – change DECtalk voice
//   rate  <75..600>                  – change DECtalk speaking rate
//   tone  <freq_hz> <duration_ms>   – play a tone (stub/TODO)
// ----------------------------------------------------------------

#ifndef CUSTOM_ACTIONS_H
#define CUSTOM_ACTIONS_H

#include "espress_jobs.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------
// Action handlers.  Each returns a heap-allocated ACTION job, or
// NULL if the arguments are invalid (the caller logs the error).
// argv[0] is the sub-command name (e.g. "gpio").
// ----------------------------------------------------------------

espress_job_t *custom_action_gpio(int argc, const char **argv);
espress_job_t *custom_action_voice(int argc, const char **argv);
espress_job_t *custom_action_rate(int argc, const char **argv);
espress_job_t *custom_action_tone(int argc, const char **argv);

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
