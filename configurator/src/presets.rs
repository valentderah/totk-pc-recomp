//! Starting points for a new profile: a handful of settings over the current ones; the player tunes the rest.
//! The Steam Deck one is the runtime's own device profile (cfg.c DECK[], carried in settings.json).
use crate::data::Catalog;
use crate::ini::Values;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Preset {
    /// The settings totk.ini holds now.
    Current,
    /// Nothing set: every setting at its default.
    Defaults,
    Quality,
    Performance,
    SteamDeck,
}

pub const ALL: [Preset; 5] = [Preset::Current, Preset::Defaults, Preset::Quality, Preset::Performance, Preset::SteamDeck];

/// Sharper and further than the game on a desktop GPU: full render scale, 16x filtering, larger and sharper shadows,
/// grass further out, contact shadows.
const QUALITY: &[(&str, &str)] = &[
    ("RENDER_SCALE", "100"),
    ("DYNAMIC_RES", "0"),
    ("ANISO", "16"),
    ("SHADOW_RES", "2048"),
    ("SHADOW_SCALE", "2"),
    ("SHADOW_RANGE", "150"),
    ("GRASS_RANGE", "150"),
    ("CONTACT_SHADOWS", "50"),
];

/// Lighter than the game for a weak GPU: 75 % render scale with dynamic resolution and FSR, the game's own shadows and
/// filtering, and the frame queue for fewer dropped frames at the limit.
const PERFORMANCE: &[(&str, &str)] = &[
    ("RENDER_SCALE", "75"),
    ("DYNAMIC_RES", "1"),
    ("UPSCALER", "fsr"),
    ("ANISO", "game"),
    ("SHADOW_RES", "game"),
    ("SHADOW_SCALE", "1"),
    ("GRASS_RANGE", "100"),
    ("CONTACT_SHADOWS", "0"),
    ("PCSS", "0"),
    ("FRAME_QUEUE", "1"),
];

/// The new profile's settings. Quality, Performance and Steam Deck change only their own keys on top of the current
/// settings, so the language, the mod order, the bindings and the rest stay as the player has them.
pub fn values(preset: Preset, catalog: &Catalog, current: &Values) -> Values {
    let mut v = current.clone();
    let list: Vec<(String, String)> = match preset {
        Preset::Current => return v,
        Preset::Defaults => return Values::new(),
        Preset::Quality => pairs(QUALITY),
        Preset::Performance => pairs(PERFORMANCE),
        Preset::SteamDeck => catalog.settings.iter().filter_map(|s| Some((s.key.clone(), s.deck.clone()?))).collect(),
    };
    for (k, val) in list {
        v.set(&k, &val);
    }
    v
}

fn pairs(list: &[(&str, &str)]) -> Vec<(String, String)> {
    list.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn presets_use_real_settings_and_values() {
        let catalog = Catalog::load();
        for p in ALL {
            for (k, v) in values(p, &catalog, &Values::new()).iter() {
                let s = catalog.find(k).unwrap_or_else(|| panic!("{p:?}: {k} is not a setting"));
                assert!(s.accepts(v), "{p:?}: {k} = {v} is not accepted ({:?})", s.values);
            }
        }
        assert!(values(Preset::SteamDeck, &catalog, &Values::new()).len() >= 4, "cfg.c DECK[] in settings.json");
        let mut mine = Values::new();
        mine.set("LANG", "ru");
        mine.set("RENDER_SCALE", "50");
        let q = values(Preset::Quality, &catalog, &mine);
        assert_eq!(q.get("LANG"), Some("ru"), "a preset keeps the player's other settings");
        assert_eq!(q.get("RENDER_SCALE"), Some("100"), "and sets its own");
        assert_eq!(values(Preset::Defaults, &catalog, &mine).len(), 0);
    }
}
