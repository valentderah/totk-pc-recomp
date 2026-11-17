// The bug this pins: cfg_get returns NULL for a key that is neither in the environment nor in totk.ini, while
// cfg_value falls back to the default in cfg.c's table. A setting read with cfg_get and then tested as
// "not the string 0" therefore treats an unset key as ON, whatever the table says.
//
// blueprint_hint shipped that way: the table said 0, the row was drawn for everyone. The review asked for it to be
// off by default, and the fix is to read it through cfg_bool (which goes through cfg_value), so this test asserts
// both halves of that: the accessor difference itself, and that blueprint_hint resolves to off when unset.
//
// Built and run by tests/blueprint_hint_default_test.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "platform/platform.h"

static int failures, checks;

static void set(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void unset(const char *name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

static void check(int cond, const char *what) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

int main(void) {
    // Nothing is set: no environment, no ini. cfg.c reads the environment with plain getenv, so the test
    // manipulates the real one (_putenv_s on Windows, setenv elsewhere) rather than a stub.
    unset("TOTK_BLUEPRINT_HINT");
    cfg_load("no-such-file.ini");

    // The accessor difference, which is the whole reason this was wrong.
    check(cfg_get("TOTK_BLUEPRINT_HINT") == NULL, "cfg_get returns NULL for an unset key");
    check(strcmp(cfg_value("TOTK_BLUEPRINT_HINT"), "0") == 0, "cfg_value returns the table default for an unset key");

    // The reader the row uses: unset has to mean off.
    check(cfg_bool("TOTK_BLUEPRINT_HINT") == 0, "blueprint_hint is off when unset");

    // And an explicit 1 still turns it on, or the feature would be unreachable.
    set("TOTK_BLUEPRINT_HINT", "1");
    check(cfg_bool("TOTK_BLUEPRINT_HINT") == 1, "blueprint_hint is on when set to 1");

    // 0 keeps it off, which is what a player who tried it and did not want it writes.
    set("TOTK_BLUEPRINT_HINT", "0");
    check(cfg_bool("TOTK_BLUEPRINT_HINT") == 0, "blueprint_hint is off when set to 0");

    // The trap itself, written out: this is the expression the old code used, and it says ON for NULL.
    unset("TOTK_BLUEPRINT_HINT");
    {
        const char *v = cfg_get("TOTK_BLUEPRINT_HINT");
        const int old_reading = !(v && !strcmp(v, "0"));
        check(old_reading == 1, "the old cfg_get expression reads an unset key as on (the defect)");
        check(cfg_bool("TOTK_BLUEPRINT_HINT") != old_reading, "cfg_bool disagrees with it, which is the fix");
    }

    printf("blueprint hint default: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}