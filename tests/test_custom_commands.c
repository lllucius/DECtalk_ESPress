// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// Host-native test harness for the custom command tokenizer/parser.
//
// Compiles and runs on the host (Linux/macOS) without ESP-IDF.
// Tests the tokenizer, argument splitter, and dispatch table by
// examining the job list produced for various input strings.
//
// Build:  make -C tests
// Run:    ./tests/test_custom_commands
// ----------------------------------------------------------------

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Pull in the implementation directly (no ESP-IDF needed)
#include "../main/dtesp_jobs.h"
#include "../main/custom_commands.h"
#include "../main/custom_actions.h"

// ----------------------------------------------------------------
// Test helpers
// ----------------------------------------------------------------

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_test_##name(void) \
    { \
        tests_run++; \
        printf("TEST %-50s ", #name); \
        test_##name(); \
        tests_passed++; \
        printf("PASS\n"); \
    } \
    static void test_##name(void)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            printf("FAIL\n  %s:%d: %s == %d, expected %d\n", \
                   __FILE__, __LINE__, #a, (int)(a), (int)(b)); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_STR_EQ(a, b) \
    do { \
        if (strcmp((a), (b)) != 0) { \
            printf("FAIL\n  %s:%d: %s == \"%s\", expected \"%s\"\n", \
                   __FILE__, __LINE__, #a, (a), (b)); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_NULL(a) \
    do { \
        if ((a) != NULL) { \
            printf("FAIL\n  %s:%d: %s is not NULL\n", \
                   __FILE__, __LINE__, #a); \
            tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_NOT_NULL(a) \
    do { \
        if ((a) == NULL) { \
            printf("FAIL\n  %s:%d: %s is NULL\n", \
                   __FILE__, __LINE__, #a); \
            tests_failed++; \
            return; \
        } \
    } while(0)

// ================================================================
// Test cases
// ================================================================

// ----------------------------------------------------------------
// 1. Plain text pass-through: no bracket-colon tokens
// ----------------------------------------------------------------
TEST(plain_text_passthrough)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("Hello world", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Hello world");
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 2. Native DECtalk command pass-through
// ----------------------------------------------------------------
TEST(native_dtesp_passthrough)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("Hello [:nb] world", &jobs);
    ASSERT_EQ(rc, 0);
    // The entire string should pass through as a single text job
    // because [:nb] is a native DECtalk command, not a [:fw ...] cmd.
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Hello [:nb] world");
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 3. Native DECtalk command with arguments
// ----------------------------------------------------------------
TEST(native_dtesp_with_args)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:ra 200] Testing [:dv ap 120]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "[:ra 200] Testing [:dv ap 120]");
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 4. Single [:fw ...] command
// ----------------------------------------------------------------
TEST(single_fw_command)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw gpio 2 on]", &jobs);
    ASSERT_EQ(rc, 0);
    // Should produce one ACTION job (no surrounding text)
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_ACTION);
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 5. Mixed speech/action/speech ordering
// ----------------------------------------------------------------
TEST(mixed_ordering)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("Hello [:fw gpio 2 on] world", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 3);
    // Job 0: speak "Hello "
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Hello ");
    // Job 1: GPIO action
    ASSERT_EQ(jobs.jobs[1]->type, DTESP_JOB_ACTION);
    // Job 2: speak " world"
    ASSERT_EQ(jobs.jobs[2]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[2]->text, " world");
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 6. Multiple fw commands in sequence
// ----------------------------------------------------------------
TEST(multiple_fw_commands)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "Start [:fw gpio 2 on] middle [:fw gpio 3 off] end", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 5);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Start ");
    ASSERT_EQ(jobs.jobs[1]->type, DTESP_JOB_ACTION);
    ASSERT_EQ(jobs.jobs[2]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[2]->text, " middle ");
    ASSERT_EQ(jobs.jobs[3]->type, DTESP_JOB_ACTION);
    ASSERT_EQ(jobs.jobs[4]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[4]->text, " end");
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 7. fw command mixed with native DECtalk command
// ----------------------------------------------------------------
TEST(fw_and_native_mixed)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "[:nb] Hello [:fw gpio 5 1] world [:ra 200]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 3);
    // Job 0: "[:nb] Hello " (native cmd stays in text)
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "[:nb] Hello ");
    // Job 1: GPIO action
    ASSERT_EQ(jobs.jobs[1]->type, DTESP_JOB_ACTION);
    // Job 2: " world [:ra 200]" (native cmd stays in text)
    ASSERT_EQ(jobs.jobs[2]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[2]->text, " world [:ra 200]");
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 8. Malformed [:fw — no closing bracket
// ----------------------------------------------------------------
TEST(malformed_no_close)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("Hello [:fw gpio 2 on", &jobs);
    ASSERT_EQ(rc, 0);
    // No closing bracket — entire string passes through as text
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Hello [:fw gpio 2 on");
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 9. Unknown fw sub-command
// ----------------------------------------------------------------
TEST(unknown_fw_subcommand)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("A [:fw unknown arg] B", &jobs);
    ASSERT_EQ(rc, 0);
    // Unknown command is consumed (not passed to DECtalk) but
    // no ACTION job is created. Text segments remain.
    ASSERT_EQ(jobs.count, 2);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "A ");
    ASSERT_EQ(jobs.jobs[1]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[1]->text, " B");
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 10. Voice handler
// ----------------------------------------------------------------
TEST(voice_handler)
{
    custom_actions_reset_session();
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw voice Betty]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_ACTION);
    // Prefix is only set once the action's execute callback runs.
    ASSERT_NULL(custom_action_get_voice_prefix());
    jobs.jobs[0]->action.execute(jobs.jobs[0]->action.ctx);
    const char *vp = custom_action_get_voice_prefix();
    ASSERT_NOT_NULL(vp);
    ASSERT_STR_EQ(vp, "[:nb]");
    dtesp_job_list_free(&jobs);
    custom_actions_reset_session();
}

// ----------------------------------------------------------------
// 11. Rate handler
// ----------------------------------------------------------------
TEST(rate_handler)
{
    custom_actions_reset_session();
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw rate 200]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_ACTION);
    ASSERT_NULL(custom_action_get_rate_prefix());
    jobs.jobs[0]->action.execute(jobs.jobs[0]->action.ctx);
    const char *rp = custom_action_get_rate_prefix();
    ASSERT_NOT_NULL(rp);
    ASSERT_STR_EQ(rp, "[:ra 200]");
    dtesp_job_list_free(&jobs);
    custom_actions_reset_session();
}

// ----------------------------------------------------------------
// 12. Rate out of range
// ----------------------------------------------------------------
TEST(rate_out_of_range)
{
    custom_actions_reset_session();
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw rate 9999]", &jobs);
    ASSERT_EQ(rc, 0);
    // Invalid rate — command consumed, no action job
    ASSERT_EQ(jobs.count, 0);
    ASSERT_NULL(custom_action_get_rate_prefix());
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 13. GPIO invalid arguments
// ----------------------------------------------------------------
TEST(gpio_invalid_args)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw gpio]", &jobs);
    ASSERT_EQ(rc, 0);
    // Missing args — no action job
    ASSERT_EQ(jobs.count, 0);
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 14. Empty input
// ----------------------------------------------------------------
TEST(empty_input)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 0);
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 15. Session reset clears prefixes
// ----------------------------------------------------------------
TEST(session_reset)
{
    custom_actions_reset_session();
    dtesp_job_list_t jobs;
    custom_commands_tokenize("[:fw voice Harry]", &jobs);
    // Run the action to apply the prefix.
    ASSERT_EQ(jobs.count, 1);
    jobs.jobs[0]->action.execute(jobs.jobs[0]->action.ctx);
    dtesp_job_list_free(&jobs);
    ASSERT_NOT_NULL(custom_action_get_voice_prefix());

    custom_commands_reset_session();
    ASSERT_NULL(custom_action_get_voice_prefix());
    ASSERT_NULL(custom_action_get_rate_prefix());
}

// ----------------------------------------------------------------
// 16. fw namespace does not match partial names
//     (e.g. [:fwx ...] should pass through as native)
// ----------------------------------------------------------------
TEST(namespace_partial_no_match)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("Hello [:fwx something] world", &jobs);
    ASSERT_EQ(rc, 0);
    // [:fwx ...] is not the "fw" namespace, so it passes through
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Hello [:fwx something] world");
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 17. Tone handler (stub)
// ----------------------------------------------------------------
TEST(tone_handler)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw tone 440 500]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_ACTION);
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 18. Job alloc/free smoke test
// ----------------------------------------------------------------
TEST(job_alloc_free)
{
    dtesp_job_t *j1 = dtesp_job_alloc_text("hello", 5);
    ASSERT_NOT_NULL(j1);
    ASSERT_EQ(j1->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(j1->text, "hello");
    dtesp_job_free(j1);

    dtesp_job_t *j2 = dtesp_job_alloc_flush();
    ASSERT_NOT_NULL(j2);
    ASSERT_EQ(j2->type, DTESP_JOB_FLUSH);
    dtesp_job_free(j2);

    dtesp_job_t *j3 = dtesp_job_alloc_action(NULL, NULL, NULL);
    ASSERT_NOT_NULL(j3);
    ASSERT_EQ(j3->type, DTESP_JOB_ACTION);
    dtesp_job_free(j3);
}

// ----------------------------------------------------------------
// 19. Adjacent fw commands with no text between
// ----------------------------------------------------------------
TEST(adjacent_fw_commands)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw gpio 2 on][:fw gpio 3 off]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 2);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_ACTION);
    ASSERT_EQ(jobs.jobs[1]->type, DTESP_JOB_ACTION);
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 20. Bracket-colon at end of buffer with no close
// ----------------------------------------------------------------
TEST(bracket_colon_at_end)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("Text [:", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Text [:");
    dtesp_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 21. Voice action applies in order (regression: voice prefix used
//     to be set at tokenization time, which broke when multiple
//     voice changes were interleaved with text in one flush).
// ----------------------------------------------------------------
TEST(voice_order_interleaved)
{
    custom_actions_reset_session();
    dtesp_job_list_t jobs;
    // betty, text1, paul, text2 — at tokenize time vp must not be set
    int rc = custom_commands_tokenize(
        "[:fw voice betty]hello[:fw voice paul]world", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 4);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_ACTION);
    ASSERT_EQ(jobs.jobs[1]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_EQ(jobs.jobs[2]->type, DTESP_JOB_ACTION);
    ASSERT_EQ(jobs.jobs[3]->type, DTESP_JOB_SPEAK_TEXT);
    ASSERT_NULL(custom_action_get_voice_prefix());

    // Simulate speech-task execution order: action → text → action → text.
    jobs.jobs[0]->action.execute(jobs.jobs[0]->action.ctx);
    ASSERT_STR_EQ(custom_action_get_voice_prefix(), "[:nb]");
    // After first text (which reads vp=[:nb]) execute second action.
    jobs.jobs[2]->action.execute(jobs.jobs[2]->action.ctx);
    ASSERT_STR_EQ(custom_action_get_voice_prefix(), "[:np]");
    dtesp_job_list_free(&jobs);
    custom_actions_reset_session();
}

// ----------------------------------------------------------------
// Codec fw commands (new in this PR): volume / profile /
// autoswitch / save.  On host builds the handlers only parse args
// and allocate ACTION jobs — the codec side is #ifdef'd out.
// ----------------------------------------------------------------

TEST(volume_handler_valid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw volume 7]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_ACTION);
    dtesp_job_list_free(&jobs);
}

TEST(volume_handler_out_of_range)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw volume 99]", &jobs);
    ASSERT_EQ(rc, 0);
    // Out-of-range level: handler rejects, token is consumed.
    ASSERT_EQ(jobs.count, 0);
    dtesp_job_list_free(&jobs);
}

TEST(volume_handler_non_numeric)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw volume loud]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 0);
    dtesp_job_list_free(&jobs);
}

TEST(profile_handler_speaker)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw profile speaker]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_ACTION);
    dtesp_job_list_free(&jobs);
}

TEST(profile_handler_headphone_alias)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw profile HP]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_ACTION);
    dtesp_job_list_free(&jobs);
}

TEST(profile_handler_invalid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw profile bluetooth]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 0);
    dtesp_job_list_free(&jobs);
}

TEST(autoswitch_handler_on_off)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw autoswitch on] [:fw autoswitch off]",
                                      &jobs);
    ASSERT_EQ(rc, 0);
    // Two action jobs plus the space between (text job).
    int action_count = 0;
    for (int i = 0; i < jobs.count; i++)
    {
        if (jobs.jobs[i]->type == DTESP_JOB_ACTION)
        {
            action_count++;
        }
    }
    ASSERT_EQ(action_count, 2);
    dtesp_job_list_free(&jobs);
}

TEST(autoswitch_handler_invalid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw autoswitch maybe]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 0);
    dtesp_job_list_free(&jobs);
}

TEST(save_handler)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw save]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_ACTION);
    dtesp_job_list_free(&jobs);
}

TEST(subcommand_case_insensitive)
{
    // The refactored dispatcher is case-insensitive for sub-command
    // names, so VOLUME / Volume / volume are all accepted.
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw VOLUME 3]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, DTESP_JOB_ACTION);
    dtesp_job_list_free(&jobs);
}

// ================================================================
// DSP / tone / EQ / DRC / spkgain / mute handlers
// ================================================================

static int action_count(const dtesp_job_list_t *jobs)
{
    int n = 0;
    for (int i = 0; i < jobs->count; i++)
    {
        if (jobs->jobs[i]->type == DTESP_JOB_ACTION)
        {
            n++;
        }
    }
    return n;
}

TEST(bass_handler_valid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "[:fw bass -3] [:fw bass 0] [:fw bass +12]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 3);
    dtesp_job_list_free(&jobs);
}

TEST(bass_handler_out_of_range)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw bass 13] [:fw bass -13]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 0);
    dtesp_job_list_free(&jobs);
}

TEST(bass_handler_bad_number)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw bass loud] [:fw bass]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 0);
    dtesp_job_list_free(&jobs);
}

TEST(treble_handler_valid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw treble +4]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 1);
    dtesp_job_list_free(&jobs);
}

TEST(treble_handler_invalid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw treble abc]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 0);
    dtesp_job_list_free(&jobs);
}

TEST(eq_band_valid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "[:fw eq 1 -6] [:fw eq 5 +4] [:fw eq 3 0]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 3);
    dtesp_job_list_free(&jobs);
}

TEST(eq_band_out_of_range)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "[:fw eq 0 3] [:fw eq 6 3] [:fw eq 2 99]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 0);
    dtesp_job_list_free(&jobs);
}

TEST(eq_reset_show)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw eq reset] [:fw eq show]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 2);
    dtesp_job_list_free(&jobs);
}

TEST(eq_preset_valid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "[:fw eq preset flat] [:fw eq preset speech] "
        "[:fw eq preset crisp] [:fw eq preset warm]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 4);
    dtesp_job_list_free(&jobs);
}

TEST(eq_preset_unknown)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw eq preset punchy]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 0);
    dtesp_job_list_free(&jobs);
}

TEST(eq_preset_missing_name)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw eq preset]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 0);
    dtesp_job_list_free(&jobs);
}

TEST(drc_on_off)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw drc on] [:fw drc off]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 2);
    dtesp_job_list_free(&jobs);
}

TEST(drc_preset_valid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "[:fw drc preset soft] [:fw drc preset speech] "
        "[:fw drc preset loud]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 3);
    dtesp_job_list_free(&jobs);
}

TEST(drc_preset_unknown)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw drc preset panic]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 0);
    dtesp_job_list_free(&jobs);
}

TEST(drc_invalid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw drc maybe]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 0);
    dtesp_job_list_free(&jobs);
}

TEST(spkgain_valid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "[:fw spkgain 6] [:fw spkgain 12] "
        "[:fw spkgain 18] [:fw spkgain 24]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 4);
    dtesp_job_list_free(&jobs);
}

TEST(spkgain_invalid)
{
    dtesp_job_list_t jobs;
    // 0 dB isn't reachable on the class-D amp; 9 dB isn't a valid step.
    int rc = custom_commands_tokenize(
        "[:fw spkgain 0] [:fw spkgain 9] [:fw spkgain 30] "
        "[:fw spkgain loud]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 0);
    dtesp_job_list_free(&jobs);
}

TEST(mute_on_off)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "[:fw mute on] [:fw mute off] [:fw mute 1] [:fw mute 0]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 4);
    dtesp_job_list_free(&jobs);
}

TEST(mute_invalid)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw mute yes]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 0);
    dtesp_job_list_free(&jobs);
}

TEST(dsp_commands_case_insensitive)
{
    dtesp_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "[:fw BASS -3] [:fw Treble +4] [:fw Eq Preset Speech]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(action_count(&jobs), 3);
    dtesp_job_list_free(&jobs);
}

// ================================================================
// Main
// ================================================================

int main(void)
{
    printf("=== Custom Command Parser Tests ===\n\n");

    run_test_plain_text_passthrough();
    run_test_native_dtesp_passthrough();
    run_test_native_dtesp_with_args();
    run_test_single_fw_command();
    run_test_mixed_ordering();
    run_test_multiple_fw_commands();
    run_test_fw_and_native_mixed();
    run_test_malformed_no_close();
    run_test_unknown_fw_subcommand();
    run_test_voice_handler();
    run_test_rate_handler();
    run_test_rate_out_of_range();
    run_test_gpio_invalid_args();
    run_test_empty_input();
    run_test_session_reset();
    run_test_namespace_partial_no_match();
    run_test_tone_handler();
    run_test_job_alloc_free();
    run_test_adjacent_fw_commands();
    run_test_bracket_colon_at_end();
    run_test_voice_order_interleaved();
    run_test_volume_handler_valid();
    run_test_volume_handler_out_of_range();
    run_test_volume_handler_non_numeric();
    run_test_profile_handler_speaker();
    run_test_profile_handler_headphone_alias();
    run_test_profile_handler_invalid();
    run_test_autoswitch_handler_on_off();
    run_test_autoswitch_handler_invalid();
    run_test_save_handler();
    run_test_subcommand_case_insensitive();

    run_test_bass_handler_valid();
    run_test_bass_handler_out_of_range();
    run_test_bass_handler_bad_number();
    run_test_treble_handler_valid();
    run_test_treble_handler_invalid();
    run_test_eq_band_valid();
    run_test_eq_band_out_of_range();
    run_test_eq_reset_show();
    run_test_eq_preset_valid();
    run_test_eq_preset_unknown();
    run_test_eq_preset_missing_name();
    run_test_drc_on_off();
    run_test_drc_preset_valid();
    run_test_drc_preset_unknown();
    run_test_drc_invalid();
    run_test_spkgain_valid();
    run_test_spkgain_invalid();
    run_test_mute_on_off();
    run_test_mute_invalid();
    run_test_dsp_commands_case_insensitive();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
