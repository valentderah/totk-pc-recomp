// The Cheats tab of the in-game menu: the cheats of the mods' cheat files (cheats.c), a heading per mod in the Mods
// tab's order, then a row per cheat in file order and a line per group label of the file; a cheat that cannot be
// switched here is greyed out with the reason. While code cheats wait for a rebuild, a row on top restarts the game
// into it.
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/option_rows.h"

extern "C" {
#include "cheats.h"
#include "cfg.h"
#include "host.h"
#include "ui_text.h"
}

typedef struct {
    const char *set;
    const char *key;
    const char *label;
    const char *desc;
    int idx;
} BuiltinCheat;

static BuiltinCheat kBuiltin[] = {
    {"bcheat_health", "TOTK_CHEAT_BUILTIN_HEALTH", "Infinite health",
     "Keeps Link's health from dropping.", -2},
    {"bcheat_stamina", "TOTK_CHEAT_BUILTIN_STAMINA", "Infinite stamina",
     "Stamina actions no longer drain stamina.", -2},
    {"bcheat_battery", "TOTK_CHEAT_BUILTIN_BATTERY", "Infinite battery",
     "Zonai battery charge stays full.", -2},
    {"bcheat_durability", "TOTK_CHEAT_BUILTIN_DURABILITY", "Infinite equipment durability",
     "Weapons, shields and bows do not lose durability from use.", -2},
    {"bcheat_horse_stamina", "TOTK_CHEAT_BUILTIN_HORSE_STAMINA", "Infinite horse stamina",
     "Horse sprint stamina does not drain.", -2},
    {"bcheat_stealth", "TOTK_CHEAT_BUILTIN_STEALTH", "Stealth mode",
     "Greatly lowers enemy detection.", -2},
    {"bcheat_thermo", "TOTK_CHEAT_BUILTIN_THERMO", "Perfect thermometer",
     "Disables heat and cold penalties.", -2},
    {"bcheat_gloom", "TOTK_CHEAT_BUILTIN_GLOOM", "Ignore gloom effects",
     "Disables gloom damage and blocking.", -2},
};

static bool builtin_rows(void) {
    bool any = false;
    for (int i = 0; i < (int)(sizeof kBuiltin / sizeof *kBuiltin); i++) {
        if (kBuiltin[i].idx == -2) kBuiltin[i].idx = rt_patch_set(kBuiltin[i].set);
        if (kBuiltin[i].idx < 0) continue;
        any = true;
    }
    if (!any) return false;
    opt_group_text("Built-in");
    for (int i = 0; i < (int)(sizeof kBuiltin / sizeof *kBuiltin); i++) {
        BuiltinCheat *c = &kBuiltin[i];
        if (c->idx < 0) continue;
        int on = rt_patch_enabled(c->idx);
        if (!opt_toggle_text(c->key, c->label, c->desc, on)) continue;
        int next = !on;
        rt_patch_switch(c->idx, next);
        cfg_set(c->key, next ? "1" : "0");
        opt_save_soon();
    }
    return true;
}

// Returns true when "Rebuild the game now" was chosen: the menu then asks to restart (ui/menu.cpp restart_confirm).
extern "C" bool tab_cheats(float scale) {
    opt_begin("##cheats", scale);
    static char line[512], desc[768], buttons[192];
    int n = cheats_count();
    bool have_builtin = builtin_rows();
    if (!n && !have_builtin) {
        snprintf(line, sizeof line, menu_text(UI_MENU_CHEATS_NONE), cheats_file()[0] ? cheats_file() : "<build id>.txt");
        opt_text_str(line);
        opt_end();
        return false;
    }
    bool rebuild =
        cheats_rebuild_pending() && opt_button("##restart", UI_MENU_CHEATS_REBUILD, UI_MENU_CHEATS_REBUILD_DESC);
    for (int i = 0; i < n; i++) {
        const CheatInfo *c = cheats_info(i);
        if (!i || strcmp(c->mod, cheats_info(i - 1)->mod)) {
            int listed = 0;
            for (int j = i; j < n && !strcmp(cheats_info(j)->mod, c->mod); j++)
                listed += cheats_info(j)->kind != CHEAT_HEADING;
            snprintf(line, sizeof line, menu_text(UI_MENU_CHEATS_MOD), c->mod, listed);
            opt_group_text(line);
        }
        UiTextId text = UI_MENU_CHEAT_CODE_DESC;
        switch (c->kind) {
        case CHEAT_HEADING:
            if (!strstr(c->name, "TID=")) opt_text_str(c->name);   // the file's title line only names the game
            continue;
        case CHEAT_CODE: break;
        case CHEAT_KEY: text = UI_MENU_CHEAT_KEY_DESC; break;
        case CHEAT_DATA: text = UI_MENU_CHEAT_DATA_DESC; break;
        case CHEAT_MASTER: text = UI_MENU_CHEAT_MASTER_DESC, opt_disable_next(UI_MENU_CHEAT_MASTER); break;
        case CHEAT_RESTORE: text = UI_MENU_CHEAT_RESTORE_DESC, opt_disable_next(UI_MENU_CHEAT_RESTORE); break;
        case CHEAT_REBUILD: text = UI_MENU_CHEAT_REBUILD_DESC, opt_disable_next(UI_MENU_CHEAT_REBUILD); break;
        case CHEAT_UNSUPPORTED:
            text = UI_MENU_CHEAT_UNSUPPORTED_DESC, opt_disable_next(UI_MENU_CHEAT_UNSUPPORTED);
            break;
        }
        if (c->kind == CHEAT_KEY)
            snprintf(desc, sizeof desc, menu_text(text), cheats_buttons(c->buttons, buttons, sizeof buttons));
        else if (c->kind == CHEAT_UNSUPPORTED)
            snprintf(desc, sizeof desc, "%s %s", menu_text(text), c->note);
        else
            snprintf(desc, sizeof desc, "%s", menu_text(text));
        if (opt_toggle_text(c->key, c->name, desc, cheats_on(i) != 0)) {
            cheats_set(i, !cheats_on(i));
            opt_save_soon();
        }
    }
    opt_end();
    return rebuild;
}
