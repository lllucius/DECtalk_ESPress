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
#include "../main/espress_jobs.h"
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
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("Hello world", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Hello world");
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 2. Native DECtalk command pass-through
// ----------------------------------------------------------------
TEST(native_dectalk_passthrough)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("Hello [:nb] world", &jobs);
    ASSERT_EQ(rc, 0);
    // The entire string should pass through as a single text job
    // because [:nb] is a native DECtalk command, not a [:fw ...] cmd.
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Hello [:nb] world");
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 3. Native DECtalk command with arguments
// ----------------------------------------------------------------
TEST(native_dectalk_with_args)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("[:ra 200] Testing [:dv ap 120]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "[:ra 200] Testing [:dv ap 120]");
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 4. Single [:fw ...] command
// ----------------------------------------------------------------
TEST(single_fw_command)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw gpio 2 on]", &jobs);
    ASSERT_EQ(rc, 0);
    // Should produce one ACTION job (no surrounding text)
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_ACTION);
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 5. Mixed speech/action/speech ordering
// ----------------------------------------------------------------
TEST(mixed_ordering)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("Hello [:fw gpio 2 on] world", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 3);
    // Job 0: speak "Hello "
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Hello ");
    // Job 1: GPIO action
    ASSERT_EQ(jobs.jobs[1]->type, ESPRESS_JOB_ACTION);
    // Job 2: speak " world"
    ASSERT_EQ(jobs.jobs[2]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[2]->text, " world");
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 6. Multiple fw commands in sequence
// ----------------------------------------------------------------
TEST(multiple_fw_commands)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "Start [:fw gpio 2 on] middle [:fw gpio 3 off] end", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 5);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Start ");
    ASSERT_EQ(jobs.jobs[1]->type, ESPRESS_JOB_ACTION);
    ASSERT_EQ(jobs.jobs[2]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[2]->text, " middle ");
    ASSERT_EQ(jobs.jobs[3]->type, ESPRESS_JOB_ACTION);
    ASSERT_EQ(jobs.jobs[4]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[4]->text, " end");
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 7. fw command mixed with native DECtalk command
// ----------------------------------------------------------------
TEST(fw_and_native_mixed)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize(
        "[:nb] Hello [:fw gpio 5 1] world [:ra 200]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 3);
    // Job 0: "[:nb] Hello " (native cmd stays in text)
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "[:nb] Hello ");
    // Job 1: GPIO action
    ASSERT_EQ(jobs.jobs[1]->type, ESPRESS_JOB_ACTION);
    // Job 2: " world [:ra 200]" (native cmd stays in text)
    ASSERT_EQ(jobs.jobs[2]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[2]->text, " world [:ra 200]");
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 8. Malformed [:fw — no closing bracket
// ----------------------------------------------------------------
TEST(malformed_no_close)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("Hello [:fw gpio 2 on", &jobs);
    ASSERT_EQ(rc, 0);
    // No closing bracket — entire string passes through as text
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Hello [:fw gpio 2 on");
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 9. Unknown fw sub-command
// ----------------------------------------------------------------
TEST(unknown_fw_subcommand)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("A [:fw unknown arg] B", &jobs);
    ASSERT_EQ(rc, 0);
    // Unknown command is consumed (not passed to DECtalk) but
    // no ACTION job is created. Text segments remain.
    ASSERT_EQ(jobs.count, 2);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "A ");
    ASSERT_EQ(jobs.jobs[1]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[1]->text, " B");
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 10. Voice handler
// ----------------------------------------------------------------
TEST(voice_handler)
{
    custom_actions_reset_session();
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw voice Betty]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_ACTION);
    // Check that voice prefix was set
    const char *vp = custom_action_get_voice_prefix();
    ASSERT_NOT_NULL(vp);
    ASSERT_STR_EQ(vp, "[:nb]");
    espress_job_list_free(&jobs);
    custom_actions_reset_session();
}

// ----------------------------------------------------------------
// 11. Rate handler
// ----------------------------------------------------------------
TEST(rate_handler)
{
    custom_actions_reset_session();
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw rate 200]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_ACTION);
    const char *rp = custom_action_get_rate_prefix();
    ASSERT_NOT_NULL(rp);
    ASSERT_STR_EQ(rp, "[:ra 200]");
    espress_job_list_free(&jobs);
    custom_actions_reset_session();
}

// ----------------------------------------------------------------
// 12. Rate out of range
// ----------------------------------------------------------------
TEST(rate_out_of_range)
{
    custom_actions_reset_session();
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw rate 9999]", &jobs);
    ASSERT_EQ(rc, 0);
    // Invalid rate — command consumed, no action job
    ASSERT_EQ(jobs.count, 0);
    ASSERT_NULL(custom_action_get_rate_prefix());
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 13. GPIO invalid arguments
// ----------------------------------------------------------------
TEST(gpio_invalid_args)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw gpio]", &jobs);
    ASSERT_EQ(rc, 0);
    // Missing args — no action job
    ASSERT_EQ(jobs.count, 0);
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 14. Empty input
// ----------------------------------------------------------------
TEST(empty_input)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 0);
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 15. Session reset clears prefixes
// ----------------------------------------------------------------
TEST(session_reset)
{
    custom_actions_reset_session();
    espress_job_list_t jobs;
    custom_commands_tokenize("[:fw voice Harry]", &jobs);
    espress_job_list_free(&jobs);
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
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("Hello [:fwx something] world", &jobs);
    ASSERT_EQ(rc, 0);
    // [:fwx ...] is not the "fw" namespace, so it passes through
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Hello [:fwx something] world");
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 17. Tone handler (stub)
// ----------------------------------------------------------------
TEST(tone_handler)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw tone 440 500]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_ACTION);
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 18. Job alloc/free smoke test
// ----------------------------------------------------------------
TEST(job_alloc_free)
{
    espress_job_t *j1 = espress_job_alloc_text("hello", 5);
    ASSERT_NOT_NULL(j1);
    ASSERT_EQ(j1->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(j1->text, "hello");
    espress_job_free(j1);

    espress_job_t *j2 = espress_job_alloc_flush();
    ASSERT_NOT_NULL(j2);
    ASSERT_EQ(j2->type, ESPRESS_JOB_FLUSH);
    espress_job_free(j2);

    espress_job_t *j3 = espress_job_alloc_action(NULL, NULL, NULL);
    ASSERT_NOT_NULL(j3);
    ASSERT_EQ(j3->type, ESPRESS_JOB_ACTION);
    espress_job_free(j3);
}

// ----------------------------------------------------------------
// 19. Adjacent fw commands with no text between
// ----------------------------------------------------------------
TEST(adjacent_fw_commands)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("[:fw gpio 2 on][:fw gpio 3 off]", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 2);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_ACTION);
    ASSERT_EQ(jobs.jobs[1]->type, ESPRESS_JOB_ACTION);
    espress_job_list_free(&jobs);
}

// ----------------------------------------------------------------
// 20. Bracket-colon at end of buffer with no close
// ----------------------------------------------------------------
TEST(bracket_colon_at_end)
{
    espress_job_list_t jobs;
    int rc = custom_commands_tokenize("Text [:", &jobs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(jobs.count, 1);
    ASSERT_EQ(jobs.jobs[0]->type, ESPRESS_JOB_SPEAK_TEXT);
    ASSERT_STR_EQ(jobs.jobs[0]->text, "Text [:");
    espress_job_list_free(&jobs);
}

// ================================================================
// Main
// ================================================================

int main(void)
{
    printf("=== Custom Command Parser Tests ===\n\n");

    run_test_plain_text_passthrough();
    run_test_native_dectalk_passthrough();
    run_test_native_dectalk_with_args();
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

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
