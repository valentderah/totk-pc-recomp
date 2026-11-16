//! Starting points for a new profile: a handful of settings over the current ones; the player tunes the rest. Each
//! change says why it is there (shown in the New profile window). Every value comes from what runtime/cfg.c metas[],
//! totk.ini.example and docs/settings.md say about the key; the tests check each key and value against settings.json.
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
    /// GPUs with about 2 GB of video memory.
    Potato,
    SteamDeck,
}

pub const ALL: [Preset; 6] =
    [Preset::Current, Preset::Defaults, Preset::Quality, Preset::Performance, Preset::Potato, Preset::SteamDeck];

/// One change of a preset: the key gets `value`, or with None goes back to its default (its line leaves totk.ini).
pub struct Change {
    pub key: &'static str,
    pub value: Option<&'static str>,
    pub why: &'static str,
}

const fn set(key: &'static str, value: &'static str, why: &'static str) -> Change {
    Change { key, value: Some(value), why }
}
const fn reset(key: &'static str, why: &'static str) -> Change {
    Change { key, value: None, why }
}

const QUALITY: &[Change] = &[
    set("RENDER_SCALE", "100", "The scene at the full game resolution instead of the game's 5/6."),
    set("DYNAMIC_RES", "0", "Keeps the scene at 100 %: the game's cost estimate was made for the Switch."),
    set("ANISO", "16", "Ground, roads and walls stay sharp at shallow angles."),
    set("SHADOW_RES", "2048", "A fixed sun shadow map instead of the game's, which drops to about 400 near the ground."),
    set("SHADOW_SCALE", "2", "Sun cascades twice as large on each side: sharper shadow edges."),
    set("SHADOW_RANGE", "150", "Shadows turn softer further away."),
    set("GRASS_RANGE", "150", "Grass grows further out in meadows."),
    set("CONTACT_SHADOWS", "50", "Small shadows at feet, under stones and in grass that the shadow maps miss."),
];

const PERFORMANCE: &[Change] = &[
    set("RENDER_SCALE", "75", "The scene at 75 % of the game resolution, a little below the game's own 5/6."),
    set("DYNAMIC_RES", "1", "The game may lower the scene further when a frame runs long."),
    set("UPSCALER", "fsr", "FSR 1 scales the picture to the window and sharpens it."),
    reset("ANISO", "The game's own texture filtering."),
    reset("SHADOW_RES", "The game's own sun shadow map size."),
    reset("SHADOW_SCALE", "The game's own 960x960 sun cascades."),
    set("GRASS_RANGE", "100", "Grass as far as the game grows it."),
    set("CONTACT_SHADOWS", "0", "No contact shadow pass."),
    set("PCSS", "0", "The game's own shadow edge filter."),
    set("FRAME_QUEUE", "1", "Far fewer dropped frames at the limit, for up to one frame more input delay."),
];

/// For a GPU with about 2 GB of video memory (an RX 550, a GTX 750 Ti, a GT 1030): less to draw and less to hold in
/// video memory. Each value is the lightest the docs describe without a risk mark; the video memory keys of
/// docs/settings.md (tex_vram_pressure, tex_released_mib, vram_*) are marked Caution there and stay at their defaults.
const POTATO: &[Change] = &[
    set("RENDER_SCALE", "67", "The scene at 2/3 of the game resolution (the game's own is 5/6): less to draw and smaller render targets in video memory."),
    reset("DYNAMIC_RES", "On (the default): the game may lower the scene further to 90 or 80 % when a frame runs long."),
    set("FPS_LIMIT", "30", "Half the frames of 60: a steady 30 instead of a frame rate that swings below 60."),
    set("GRASS_RANGE", "50", "Grass grows half as far as the game's 62.5 m: less grass to draw and a smaller grass heap."),
    reset("SHADOW_RES", "The game's own sun shadow map size; fixed sizes above 960 grow the texture heap."),
    reset("SHADOW_SCALE", "The game's own 960x960 sun cascades; larger ones cost video memory and time."),
    reset("SHADOW_RANGE", "The game's own shadow cascade range."),
    reset("CONTACT_SHADOWS", "Off (the default): no contact shadow pass after the lighting."),
    reset("PCSS", "Off (the default): the game's own four-tap shadow filter."),
    reset("ANISO", "The level the game asks for, as on the Switch: the cheapest texture filtering."),
    reset("FXAA", "On (the default): the game's own FXAA, the cheapest anti-aliasing."),
    reset("DLAA", "Off (the default): DLSS needs an NVIDIA RTX GPU."),
    reset("LOD_STEADY", "Off (the default): the game lowers model and texture detail when the frame rate drops, which keeps it up on weak PCs."),
    reset("SYNC_AHEAD", "Off (the default): it needs shadow storage, about 1.1 GB more memory."),
    reset("SHADOW", "The platform's default: off on Windows, where shadow storage doubles the memory pools (about 1.1 GB)."),
    reset("REPLAY_WORKERS", "One recording thread (the default); more threads only help on 8 or more logical processors."),
];

impl Preset {
    pub fn changes(self) -> &'static [Change] {
        match self {
            Preset::Quality => QUALITY,
            Preset::Performance => PERFORMANCE,
            Preset::Potato => POTATO,
            _ => &[],
        }
    }
    pub fn label(self) -> &'static str {
        match self {
            Preset::Current => "Current settings",
            Preset::Defaults => "Defaults",
            Preset::Quality => "Quality",
            Preset::Performance => "Performance",
            Preset::Potato => "Potato mode",
            Preset::SteamDeck => "Steam Deck-like",
        }
    }
    pub fn description(self) -> &'static str {
        match self {
            Preset::Current => "A copy of the settings totk.ini holds now.",
            Preset::Defaults => "No setting set: the game's and the port's defaults.",
            Preset::Quality => "Sharper and further than the game, for a desktop GPU with memory to spare.",
            Preset::Performance => "Lighter than the game for a weak GPU, with the frame queue for steadier frames.",
            Preset::Potato => "For GPUs with about 2 GB of video memory: a smaller scene, 30 fps, short grass and every extra effect off.",
            Preset::SteamDeck => "The runtime's Steam Deck profile: 1280x800, fullscreen, VSync, 40 fps.",
        }
    }
}

/// The new profile's settings. Quality, Performance, Potato and Steam Deck change only their own keys on top of the
/// current settings, so the language, the mod order, the bindings and the rest stay as the player has them.
pub fn values(preset: Preset, catalog: &Catalog, current: &Values) -> Values {
    let mut v = current.clone();
    match preset {
        Preset::Current => {}
        Preset::Defaults => return Values::new(),
        Preset::SteamDeck => {
            for s in &catalog.settings {
                if let Some(d) = &s.deck {
                    v.set(&s.key, d);
                }
            }
        }
        _ => {
            for c in preset.changes() {
                match c.value {
                    Some(val) => v.set(c.key, val),
                    None => v.remove(c.key),
                }
            }
        }
    }
    v
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
            for c in p.changes() {
                let s = catalog.find(c.key).unwrap_or_else(|| panic!("{p:?}: {} is not in cfg.c known[]", c.key));
                let value = c.value.unwrap_or(&s.default);
                assert!(value.is_empty() || s.accepts(value), "{p:?}: {} = {value} outside {:?}", c.key, s.values);
                assert!(!c.why.is_empty(), "{p:?}: {} says nothing", c.key);
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

    /// Potato mode lowers what costs the GPU: every value it sets is at or below the default of a numeric key, and
    /// none of its keys carries a risk mark in docs/settings.md.
    #[test]
    fn potato_only_lowers_and_is_safe() {
        let catalog = Catalog::load();
        let mut heavy = Values::new();
        for (k, v) in [("SHADOW_SCALE", "4"), ("CONTACT_SHADOWS", "100"), ("PCSS", "200"), ("SHADOW", "1"), ("SYNC_AHEAD", "1"), ("LOD_STEADY", "1"), ("GRASS_RANGE", "400"), ("LANG", "ru")] {
            heavy.set(k, v);
        }
        let p = values(Preset::Potato, &catalog, &heavy);
        for k in ["SHADOW_SCALE", "CONTACT_SHADOWS", "PCSS", "SHADOW", "SYNC_AHEAD", "LOD_STEADY"] {
            assert_eq!(p.get(k), None, "{k} back to its default");
        }
        assert_eq!((p.get("GRASS_RANGE"), p.get("FPS_LIMIT"), p.get("LANG")), (Some("50"), Some("30"), Some("ru")));
        for c in POTATO {
            let s = catalog.find(c.key).unwrap();
            assert!(matches!(s.safety.as_deref(), None | Some("Safe")), "{} is marked {:?}", c.key, s.safety);
            if let (Some(v), Ok(d)) = (c.value, s.default.parse::<f64>()) {
                if let Ok(x) = v.parse::<f64>() {
                    assert!(x <= d || c.key == "DYNAMIC_RES", "{} = {v} is above its default {d}", c.key);
                }
            }
        }
    }
}
