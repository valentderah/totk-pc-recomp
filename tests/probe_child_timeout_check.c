// Real self-spawned children exercise production output capture and its deadline. No game is run.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win32.c"
#else
#include "platform/linux.c"
#endif

#define CHILD_MODE "FIXTURE_CHILD_MODE"
static int checks, failures;
#define CHECK(condition, name) do { \
    checks++; \
    if (!(condition)) { failures++; printf("FAIL probe child timeout: %s\n", name); } \
} while (0)

// The holder deliberately inherits this child's redirected output without writing to it. Its bounded lifetime
// lets the test prove that waiting for pipe EOF is different from waiting for the direct child to exit.
static int spawn_output_holder(void) {
    if (!plat_setenv(CHILD_MODE, "holder")) return 0;
#ifdef _WIN32
    STARTUPINFOW holder_startup = {.cb = sizeof holder_startup, .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = GetStdHandle(STD_INPUT_HANDLE), .hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE),
        .hStdError = GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION holder_process;
    wchar_t *holder_command = _wcsdup(GetCommandLineW());
    int started = holder_command && CreateProcessW(NULL, holder_command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                                   NULL, NULL, &holder_startup, &holder_process);
    free(holder_command);
    if (started) {
        CloseHandle(holder_process.hThread);
        CloseHandle(holder_process.hProcess);
    }
#else
    char holder_exe[PATH_MAX];
    int started = 0;
    if (plat_exe_path(holder_exe, sizeof holder_exe)) {
        char *holder_args[] = {holder_exe, NULL};
        pid_t holder_pid;
        started = posix_spawn(&holder_pid, holder_exe, NULL, NULL, holder_args, environ) == 0;
    }
#endif
    plat_setenv(CHILD_MODE, "descendant");
    return started;
}
static int child(const char *mode) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (!strcmp(mode, "holder")) {
        plat_sleep_ms(1200);
    } else if (!strcmp(mode, "descendant")) {
        if (!spawn_output_holder()) return 8;
        fputs("hello", stdout);
    } else if (!strcmp(mode, "quiet")) {
        plat_sleep_ms(1500);
    } else if (!strcmp(mode, "flood") || !strcmp(mode, "flood-hang")) {
        char block[1024];
        for (size_t i = 0; i < sizeof block; i++) block[i] = (char)('A' + i % 26);
        for (int i = 0; i < 64; i++)
            if (fwrite(block, 1, sizeof block, stdout) != sizeof block) return 8;
        if (!strcmp(mode, "flood-hang")) plat_sleep_ms(1500);
    } else if (!strcmp(mode, "close-output")) {
        fclose(stdout);
        fclose(stderr);
        plat_sleep_ms(1500);
    } else if (!strcmp(mode, "delay")) {
        plat_sleep_ms(30);
        fputs("hello", stdout);
    } else if (!strcmp(mode, "normal")) {
        fputs("hello", stdout);
    } else {
        return 8;
    }
    return 7;
}

static void timeout_case(const char *mode, const char *description, int expect_prefix) {
    char out[16]; memset(out, '!', sizeof out);
    uint64_t start = plat_now_ns();
    int code = plat_run_self(CHILD_MODE, mode, out, sizeof out, 100);
    uint64_t elapsed_ms = (plat_now_ns() - start) / 1000000;
    printf("%s: exit=%d elapsed=%llu ms\n", mode, code, (unsigned long long)elapsed_ms);
    CHECK(code >= 0 && code != 7 && elapsed_ms < 900, description);
    if (expect_prefix)
        CHECK(!memcmp(out, "ABCDEFGHIJKLMNO\0", sizeof out), "timed-out noisy child retains a terminated prefix");
}

int main(void) {
    const char *mode = getenv(CHILD_MODE);
    if (mode) return child(mode);
    setvbuf(stdout, NULL, _IONBF, 0);
    timeout_case("quiet", "a quiet child with an open output pipe is killed at the deadline", 0);
    timeout_case("flood-hang", "draining output beyond capacity still enforces the deadline", 1);
    timeout_case("close-output", "the deadline also applies after the child closes its output", 0);

    char out[32]; memset(out, '!', sizeof out);
    int code = plat_run_self(CHILD_MODE, "normal", out, sizeof out, 1000);
    CHECK(code == 7 && !memcmp(out, "hello\0", 6), "normal child output and exit status are preserved");
    char prefix[16]; memset(prefix, '!', sizeof prefix);
    code = plat_run_self(CHILD_MODE, "flood", prefix, sizeof prefix, 1000);
    CHECK(code == 7 && !memcmp(prefix, "ABCDEFGHIJKLMNO\0", sizeof prefix),
          "64 KiB output is fully drained while retaining the first cap-1 bytes");
    code = plat_run_self(CHILD_MODE, "normal", NULL, 0, 1000);
    CHECK(code == 7, "zero output capacity discards all bytes without dereferencing NULL");
    char one = '!';
    code = plat_run_self(CHILD_MODE, "normal", &one, 1, 1000);
    CHECK(code == 7 && one == 0, "capacity one produces an empty terminated output");
    memset(out, '!', sizeof out);
    code = plat_run_self(CHILD_MODE, "delay", out, sizeof out, 0);
    CHECK(code == 7 && !memcmp(out, "hello\0", 6), "zero timeout waits for a delayed normal child");

    memset(out, '!', sizeof out);
    uint64_t descendant_start = plat_now_ns();
    code = plat_run_self(CHILD_MODE, "descendant", out, sizeof out, 0);
    uint64_t descendant_ms = (plat_now_ns() - descendant_start) / 1000000;
    printf("descendant: exit=%d elapsed=%llu ms\n", code, (unsigned long long)descendant_ms);
    CHECK(code == 7 && !memcmp(out, "hello\0", 6), "an exited child's buffered output and exit status are preserved");
    CHECK(descendant_ms < 900, "an inherited output writer does not extend the direct child's wait");
    // The holder exits on its own. Wait before the driver removes its temporary executable (locked on Windows).
    plat_sleep_ms(1400);
    printf("probe child timeout: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}