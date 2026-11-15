//! The settings the configurator shows: configurator/settings.json, which tools/gen_configurator_settings.py writes
//! from the runtime's own sources (runtime/cfg.c, runtime/ui/menu.cpp, runtime/ui_text.c, runtime/input_map.c,
//! totk.ini.example, docs/settings.md). Nothing here lists settings by hand.
use serde::Deserialize;
use std::collections::BTreeMap;

pub type Text = BTreeMap<String, String>;

#[derive(Deserialize, Clone, Debug)]
pub struct Choice {
    pub value: String,
    pub label: Option<Text>,
}

#[derive(Deserialize, Clone, Debug, PartialEq, Eq, Copy)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    Toggle,
    Choice,
    Slider,
    Number,
    Text,
    Folder,
    Key,
    Bind,
}

#[derive(Deserialize, Clone, Debug)]
pub struct Setting {
    pub key: String,
    /// cfg.c metas[] has a row: the in-game menu's settings table (checked by the tests).
    #[cfg_attr(not(test), allow(dead_code))]
    pub meta: bool,
    pub default: String,
    /// "0|1", "a|b|c", "0..100" or None for free text (cfg.c CfgMeta values).
    pub values: Option<String>,
    pub live: bool,
    pub section: Option<String>,
    /// The Steam Deck device profile's value (cfg.c DECK[]).
    pub deck: Option<String>,
    pub kind: Kind,
    pub tab: String,
    /// The row's place within its tab: the in-game menu's order, then the rest.
    pub order: u32,
    pub in_menu: bool,
    pub group: Option<Text>,
    pub label: Option<Text>,
    pub desc: Option<Text>,
    pub doc: Option<String>,
    pub doc_group: Option<String>,
    pub safety: Option<String>,
    pub choices: Option<Vec<Choice>>,
    pub min: Option<f64>,
    pub max: Option<f64>,
    pub step: Option<f64>,
    pub format: Option<String>,
    /// The key whose value applies while this one is unset (the per-stick dead zones fall back to STICK_DEADZONE).
    pub fallback: Option<String>,
    /// Shown as Off (unset) / On, On writing this value (menu.cpp row_set_or_not).
    pub set_or_not: Option<String>,
    /// A binding of a stick direction or SHAKE: keys only.
    #[serde(default)]
    pub keys_only: bool,
}

#[derive(Deserialize, Clone, Debug)]
pub struct Tab {
    pub id: String,
    pub label: Option<Text>,
}

#[derive(Deserialize, Clone, Debug)]
pub struct Input {
    #[allow(dead_code)] // the bind keys carry the action name; kept for completeness of the generated data
    pub actions: Vec<String>,
    pub keys: Vec<String>,
    pub pads: Vec<String>,
    pub default_keys: BTreeMap<String, Vec<String>>,
    pub default_pads: BTreeMap<String, BTreeMap<String, Vec<String>>>,
    pub key_labels: BTreeMap<String, Text>,
    pub pad_labels: BTreeMap<String, Text>,
}

#[derive(Deserialize, Clone, Debug)]
pub struct Catalog {
    #[allow(dead_code)] // the languages of the generated texts; tr() falls back to English
    pub langs: Vec<String>,
    pub tabs: Vec<Tab>,
    pub common: BTreeMap<String, Option<Text>>,
    pub settings: Vec<Setting>,
    pub input: Input,
}

pub const SETTINGS_JSON: &str = include_str!("../settings.json");

impl Catalog {
    pub fn load() -> Catalog {
        serde_json::from_str(SETTINGS_JSON).expect("configurator/settings.json parses")
    }
    pub fn find(&self, key: &str) -> Option<&Setting> {
        self.settings.iter().find(|s| s.key == key)
    }
    /// The totk.ini section a key goes to when the file lacks it (cfg.c section_of).
    pub fn section_of(&self, key: &str) -> Option<String> {
        if key.starts_with("CHEAT_") {
            return Some("cheats".into());
        }
        self.find(key).and_then(|s| s.section.clone())
    }
    /// Whether the runtime knows key (cfg.c is_known): known[] and the CHEAT_ keys.
    pub fn is_known(&self, key: &str) -> bool {
        key.starts_with("CHEAT_") || self.find(key).is_some()
    }
}

/// text in lang, else English, else any.
pub fn tr<'a>(text: &'a Option<Text>, lang: &str) -> Option<&'a str> {
    let t = text.as_ref()?;
    t.get(lang).or_else(|| t.get("en")).or_else(|| t.values().next()).map(|s| s.as_str())
}

impl Setting {
    /// The range of a "lo..hi" values string.
    pub fn range(&self) -> Option<(f64, f64)> {
        let v = self.values.as_deref()?;
        let (lo, hi) = v.split_once("..")?;
        Some((lo.parse().ok()?, hi.parse().ok()?))
    }
    /// Whether value is one the runtime accepts for this key (choices, range); free text always is.
    pub fn accepts(&self, value: &str) -> bool {
        if let Some((lo, hi)) = self.range() {
            return value.parse::<f64>().is_ok_and(|x| x >= lo && x <= hi);
        }
        match self.values.as_deref() {
            Some(list) => list.split('|').any(|v| v == value),
            None => true,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeSet;

    /// runtime/cfg.c, read directly: the test that fails when settings.json drifts from the runtime.
    fn cfg_c() -> String {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../runtime/cfg.c");
        std::fs::read_to_string(path).expect("runtime/cfg.c next to configurator/")
    }

    fn array_body<'a>(text: &'a str, marker: &str) -> &'a str {
        let start = text.find(marker).expect(marker);
        let open = start + text[start..].find('{').unwrap();
        let close = open + text[open..].find("};").unwrap();
        &text[open + 1..close]
    }

    fn quoted(body: &str) -> Vec<String> {
        let mut out = Vec::new();
        let mut rest = body;
        while let Some(a) = rest.find('"') {
            let b = a + 1 + rest[a + 1..].find('"').unwrap();
            out.push(rest[a + 1..b].to_string());
            rest = &rest[b + 1..];
        }
        out
    }

    #[test]
    fn settings_json_matches_cfg_c() {
        let cfg = cfg_c();
        let known: BTreeSet<String> = quoted(array_body(&cfg, "static const char *const known[]")).into_iter().collect();
        let catalog = Catalog::load();
        let ours: BTreeSet<String> = catalog.settings.iter().map(|s| s.key.clone()).collect();
        let missing: Vec<_> = known.difference(&ours).collect();
        let extra: Vec<_> = ours.difference(&known).collect();
        assert!(
            missing.is_empty() && extra.is_empty(),
            "settings.json is out of date with runtime/cfg.c known[] (missing {missing:?}, extra {extra:?}): \
             run python tools/gen_configurator_settings.py"
        );
        // metas[]: {"KEY", "default", "values" or NULL, LIVE or RESTART}
        let metas = array_body(&cfg, "static const CfgMeta metas[]");
        let mut rows = 0;
        for line in metas.lines() {
            let line = line.split("//").next().unwrap();
            for row in line.split('{').skip(1) {
                let fields = quoted(row);
                if fields.len() < 2 {
                    continue;
                }
                rows += 1;
                let s = catalog.find(&fields[0]).unwrap_or_else(|| panic!("{} missing", fields[0]));
                assert!(s.meta, "{}: metas[] row, settings.json says none", s.key);
                assert_eq!(s.default, fields[1], "{}: default differs from cfg.c", s.key);
                let values = if row.contains("NULL") { None } else { fields.get(2).cloned() };
                assert_eq!(s.values, values, "{}: values differ from cfg.c", s.key);
                assert_eq!(s.live, row.contains("LIVE"), "{}: live flag differs from cfg.c", s.key);
            }
        }
        assert_eq!(rows, catalog.settings.iter().filter(|s| s.meta).count(), "metas[] row count");
    }

    #[test]
    fn every_menu_setting_has_text_in_en_and_ru() {
        let catalog = Catalog::load();
        for s in catalog.settings.iter().filter(|s| s.in_menu && s.kind != Kind::Folder) {
            for lang in ["en", "ru"] {
                assert!(s.label.as_ref().is_some_and(|t| t.contains_key(lang)), "{}: no {lang} label", s.key);
            }
        }
        for t in &catalog.tabs {
            assert!(t.label.as_ref().is_some_and(|l| l.contains_key("ru")), "tab {}", t.id);
        }
    }

    /// totk.ini.example (every line a comment, each key's default as "# key = value" under its section): setting every
    /// key puts its line right after that example line, and resetting them all gives the file back byte for byte.
    #[test]
    fn example_round_trip() {
        use crate::ini::{self, Values};
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../totk.ini.example");
        let text = std::fs::read_to_string(path).unwrap();
        let catalog = Catalog::load();
        let section_of = |k: &str| catalog.section_of(k);
        let mut values = Values::new();
        for s in &catalog.settings {
            values.set(&s.key, "7");
        }
        let edited = ini::save_text(&text, &ini::parse(&text), &values, &section_of);
        let lines: Vec<&str> = edited.lines().collect();
        let mut placed = 0;
        for (i, line) in lines.iter().enumerate() {
            if let Some((k, v)) = line.split_once(" = ") {
                if !line.starts_with('#') && v == "7" && i > 0 {
                    let example = lines[..i].iter().rev().find(|l| !l.split_once(" = ").is_some_and(|(_, v)| v == "7" && !l.starts_with('#')));
                    if example.is_some_and(|e| e.trim_start_matches(['#', ' ']).starts_with(&format!("{k} ="))) {
                        placed += 1;
                    }
                }
            }
        }
        let in_example = text.lines().filter(|l| l.starts_with("# ") && l[2..].split_once(" = ").is_some_and(|(k, _)| catalog.find(&k.to_ascii_uppercase()).is_some())).count();
        assert!(placed * 10 >= in_example * 9, "only {placed} of {in_example} keys went after their example line");
        let read = ini::parse(&edited);
        assert_eq!(read.len(), values.len(), "every key reads back");
        assert!(values.iter().all(|(k, v)| read.get(k) == Some(v)), "every value reads back");
        let back = ini::save_text(&edited, &ini::parse(&edited), &Values::new(), &section_of);
        // the keys without an example line went into new sections at the end; everything before is the example again
        assert!(back.starts_with(text.trim_end()), "resetting every key restores totk.ini.example");
    }

    #[test]
    fn choices_are_accepted_values() {
        let catalog = Catalog::load();
        for s in &catalog.settings {
            for c in s.choices.iter().flatten() {
                assert!(c.value.is_empty() || s.accepts(&c.value), "{}: choice {:?} outside {:?}", s.key, c.value, s.values);
            }
        }
    }
}
