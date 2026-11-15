//! Settings and their descriptions read at run time, so a new or changed setting shows up without rebuilding the
//! configurator. The embedded settings.json (the in-game menu's tabs, labels and translations, which only the C sources
//! hold) is the base; over it go, from the files found next to the game:
//!
//! 1. `totk-settings.json` (or `configurator/settings.json` of a source checkout): a newer settings.json replaces the
//!    embedded one whole.
//! 2. `totk.ini.example`: each "# key = default" line with the comment block above it. A key the base lacks becomes a
//!    setting of its own; a key it has gets this description and, outside the menu's table, this default.
//! 3. `docs/settings.md`: the table rows "| `key` (default `x`) | what | effect | safety |", the same way, with the
//!    safety mark and the section heading.
//! 4. The comments of totk.ini itself (the installer writes it from totk.ini.example): only keys and descriptions the
//!    others lack, as the file may be older than the game.
//!
//! The files are looked for in the game folder, the configurator's folder and their parents (a source checkout or the
//! release folder of the installer), the nearest first.
use crate::data::{Catalog, Kind, Setting};
use std::path::{Path, PathBuf};

/// What one document says about a key.
#[derive(Clone, Debug, PartialEq)]
pub struct DocEntry {
    pub key: String,
    pub default: Option<String>,
    pub text: String,
    pub section: Option<String>,
    pub safety: Option<String>,
}

/// Where the run-time descriptions came from, for the window.
#[derive(Clone, Debug, Default)]
pub struct Sources {
    pub json: Option<PathBuf>,
    pub example: Option<PathBuf>,
    pub settings_md: Option<PathBuf>,
    pub ini: Option<PathBuf>,
    /// Keys added from the documents (not in the embedded list).
    pub added: Vec<String>,
}

/// The entries of a totk.ini-format text. A commented "# key = value" line gives the key's default and takes the
/// comment block above it (up to a blank line, a section line or another key line); keys right after each other share
/// one block. Active "key = value" lines (a player's totk.ini) give the description only, never a default.
pub fn ini_entries(text: &str) -> Vec<DocEntry> {
    let mut out: Vec<DocEntry> = Vec::new();
    let mut block: Vec<String> = Vec::new();
    let mut last = String::new();
    let mut section: Option<String> = None;
    for line in text.lines() {
        let s = line.trim().trim_start_matches('\u{feff}');
        let commented = s.starts_with('#') || s.starts_with(';');
        let body = if commented { s[1..].trim_start() } else { s };
        if let Some((k, v)) = body.split_once(" = ") {
            let k = k.trim();
            if !k.is_empty() && k.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
                && k.as_bytes()[0].is_ascii_lowercase()
            {
                let text = if block.is_empty() { last.clone() } else { block.join(" ") };
                let key = k.to_ascii_uppercase();
                if !out.iter().any(|e| e.key == key) {
                    out.push(DocEntry {
                        key,
                        default: commented.then(|| v.trim().to_string()),
                        text: text.clone(),
                        section: section.clone(),
                        safety: None,
                    });
                }
                last = text;
                block.clear();
                continue;
            }
        }
        if s.starts_with('[') {
            section = s.trim_matches(['[', ']']).trim().to_string().into();
            block.clear();
            last.clear();
        } else if commented {
            block.push(body.to_string());
        } else if s.is_empty() {
            block.clear();
            last.clear();
        }
    }
    out
}

/// The table rows of docs/settings.md: "| `name` (default `x`) | what | effect | safety |", under "## heading".
pub fn settings_md_entries(text: &str) -> Vec<DocEntry> {
    let mut out = Vec::new();
    let mut heading: Option<String> = None;
    for line in text.lines() {
        if let Some(h) = line.strip_prefix("## ") {
            heading = Some(h.trim().to_string());
            continue;
        }
        let Some(rest) = line.strip_prefix("| `") else { continue };
        let Some((name, rest)) = rest.split_once('`') else { continue };
        if name.contains('<') || !name.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_') {
            continue;
        }
        let mut default = None;
        let mut rest = rest.trim_start();
        if let Some(r) = rest.strip_prefix("(default `") {
            if let Some((d, r)) = r.split_once("`)") {
                default = Some(d.to_string());
                rest = r.trim_start();
            }
        }
        let cells: Vec<&str> = rest.trim_start_matches('|').split(" | ").map(|c| c.trim().trim_end_matches('|').trim()).collect();
        if cells.len() < 3 {
            continue;
        }
        out.push(DocEntry {
            key: name.to_ascii_uppercase(),
            default,
            text: format!("{} {}", cells[0], cells[1]).replace('`', "").trim().to_string(),
            section: heading.clone(),
            safety: Some(cells[2].to_string()).filter(|s| !s.is_empty()),
        });
    }
    out
}

/// The tab of a key the base list lacks, by its totk.ini section (as tools/gen_configurator_settings.py SECTION_TAB).
fn tab_of_section(section: Option<&str>) -> &'static str {
    match section.unwrap_or("") {
        "display" => "screen",
        "frame rate" => "game",
        "audio" => "sound",
        "input" | "gameplay tweaks" => "controls",
        "language" | "discord" => "menu",
        "mods" => "mods",
        "performance" | "diagnostics" => "advanced",
        _ => "expert",
    }
}

/// The values a description gives a key the base list lacks: "0 or 1" is a switch, "a to b" a range when the default
/// is inside it; anything else stays free text.
fn infer_values(e: &DocEntry) -> Option<String> {
    let t = e.text.to_ascii_lowercase();
    let d = e.default.as_deref().unwrap_or("");
    if (t.contains("0 or 1") || t.contains("1 or 0") || t.contains("1 = on")) && matches!(d, "0" | "1" | "") {
        return Some("0|1".into());
    }
    let words: Vec<&str> = t
        .split(|c: char| c.is_whitespace() || c == '(' || c == ')' || c == ',' || c == ';')
        .map(|w| w.trim_end_matches(['.', ':']))
        .collect();
    for w in words.windows(3) {
        if w[1] == "to" {
            if let (Ok(lo), Ok(hi)) = (w[0].parse::<i64>(), w[2].parse::<i64>()) {
                if lo < hi && d.parse::<i64>().is_ok_and(|x| x >= lo && x <= hi) {
                    return Some(format!("{lo}..{hi}"));
                }
            }
        }
    }
    None
}

/// How strongly a document's word counts for a key the base already has.
#[derive(Clone, Copy, PartialEq)]
pub enum Weight {
    /// totk.ini.example, docs/settings.md: the description replaces the base's.
    Replace,
    /// a player's totk.ini: only fills what the base lacks.
    Fill,
}

/// Puts the entries over the catalog: descriptions, defaults outside the menu's table, safety marks; a key the
/// catalog lacks becomes a setting (tab by its section, control by its values). Returns the keys added.
pub fn overlay(cat: &mut Catalog, entries: &[DocEntry], weight: Weight, source: &str) -> Vec<String> {
    let mut added = Vec::new();
    for e in entries {
        if e.key.starts_with("CHEAT_") {
            continue;
        }
        if let Some(s) = cat.settings.iter_mut().find(|s| s.key == e.key) {
            let fill = weight == Weight::Fill;
            if !e.text.is_empty() && (!fill || s.doc.is_none()) {
                s.doc = Some(e.text.clone());
            }
            if !s.meta && !fill {
                if let Some(d) = e.default.as_ref().filter(|d| !d.is_empty()) {
                    s.default = d.clone();
                }
            }
            if e.safety.is_some() && (!fill || s.safety.is_none()) {
                s.safety = e.safety.clone();
                s.doc_group = e.section.clone();
            }
            continue;
        }
        // a key of the player's totk.ini with no comment above it says nothing new: it stays under Other keys
        if weight == Weight::Fill && e.text.is_empty() {
            continue;
        }
        let values = infer_values(e);
        let kind = match values.as_deref() {
            Some("0|1") => Kind::Toggle,
            Some(_) => Kind::Number,
            None if e.key.ends_with("_DIR") => Kind::Folder,
            None => Kind::Text,
        };
        // a key of a docs/settings.md table goes to Expert under its heading; one of totk.ini.example by its section
        let from_md = e.safety.is_some();
        let tab = if from_md { "expert" } else { tab_of_section(e.section.as_deref()) };
        cat.settings.push(Setting {
            key: e.key.clone(),
            meta: false,
            default: e.default.clone().unwrap_or_default(),
            values,
            live: false,
            section: if from_md { None } else { e.section.clone() },
            deck: None,
            kind,
            tab: tab.to_string(),
            order: 300_000 + cat.settings.len() as u32,
            in_menu: false,
            group: None,
            label: None,
            desc: None,
            doc: Some(e.text.clone()).filter(|t| !t.is_empty()),
            doc_group: if from_md { e.section.clone() } else { None },
            safety: e.safety.clone(),
            choices: None,
            min: None,
            max: None,
            step: None,
            format: None,
            fallback: None,
            set_or_not: None,
            keys_only: false,
            source: Some(source.to_string()),
        });
        added.push(e.key.clone());
    }
    added
}

/// The folders to look in: the game folder, the configurator's own folder, and up to four parents of each.
fn search_dirs(game_dir: &Path) -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    let exe_dir = std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf));
    for start in std::iter::once(game_dir.to_path_buf()).chain(exe_dir) {
        let mut d = Some(start.as_path());
        for _ in 0..5 {
            let Some(dir) = d else { break };
            if !dirs.iter().any(|x: &PathBuf| x == dir) {
                dirs.push(dir.to_path_buf());
            }
            d = dir.parent();
        }
    }
    dirs
}

fn first(dirs: &[PathBuf], names: &[&str]) -> Option<PathBuf> {
    dirs.iter().flat_map(|d| names.iter().map(move |n| d.join(n))).find(|p| p.is_file())
}

/// The catalog for a game folder: the embedded or a newer settings.json, with the documents found over it.
pub fn load(game_dir: Option<&Path>) -> (Catalog, Sources) {
    let mut src = Sources::default();
    let Some(game_dir) = game_dir else { return (Catalog::load(), src) };
    let dirs = search_dirs(game_dir);
    let mut cat = Catalog::load();
    if let Some(p) = first(&dirs, &["totk-settings.json", "configurator/settings.json"]) {
        if let Some(c) = std::fs::read_to_string(&p).ok().and_then(|t| serde_json::from_str::<Catalog>(&t).ok()) {
            cat = c;
            src.json = Some(p);
        }
    }
    if let Some(p) = first(&dirs, &["totk.ini.example"]) {
        if let Ok(t) = std::fs::read_to_string(&p) {
            src.added.extend(overlay(&mut cat, &ini_entries(&t), Weight::Replace, "totk.ini.example"));
            src.example = Some(p);
        }
    }
    if let Some(p) = first(&dirs, &["docs/settings.md", "settings.md"]) {
        if let Ok(t) = std::fs::read_to_string(&p) {
            src.added.extend(overlay(&mut cat, &settings_md_entries(&t), Weight::Replace, "settings.md"));
            src.settings_md = Some(p);
        }
    }
    let ini = game_dir.join("totk.ini");
    if let Ok(t) = std::fs::read_to_string(&ini) {
        src.added.extend(overlay(&mut cat, &ini_entries(&t), Weight::Fill, "totk.ini"));
        src.ini = Some(ini);
    }
    (cat, src)
}

#[cfg(test)]
mod tests {
    use super::*;

    const EXAMPLE: &str = "\
# header comment\n\
\n\
[frame rate]\n\
# Frame limit: 30 to 240.\n\
# fps_limit = 60\n\
# Older key.\n\
# fps60 = 1\n\
\n\
[display]\n\
# A brand new switch, 0 or 1.\n\
# shiny_new = 0\n\
# How many shiny things, 1 to 8.\n\
# shiny_count = 2\n\
# Where they go.\n\
# shiny_dir = shiny\n\
";

    #[test]
    fn reads_example_blocks() {
        let e = ini_entries(EXAMPLE);
        let get = |k: &str| e.iter().find(|x| x.key == k).unwrap().clone();
        assert_eq!(get("FPS_LIMIT").text, "Frame limit: 30 to 240.");
        assert_eq!(get("FPS_LIMIT").default.as_deref(), Some("60"));
        assert_eq!(get("FPS60").text, "Older key.");
        assert_eq!(get("SHINY_NEW").section.as_deref(), Some("display"));
        assert!(!e.iter().any(|x| x.key == "HEADER"));
    }

    #[test]
    fn player_ini_gives_no_defaults() {
        let e = ini_entries("[x]\n# My own key.\nmy_key = 5\n# other = 3\n");
        assert_eq!(e[0].key, "MY_KEY");
        assert_eq!(e[0].default, None);
        assert_eq!(e[0].text, "My own key.");
        assert_eq!(e[1].default.as_deref(), Some("3"));
    }

    #[test]
    fn reads_settings_md_rows() {
        let md = "## Diagnostics\n\n| Setting | What | Effect | Safety |\n| --- | --- | --- | --- |\n\
                  | `astc_time` | `1` times decodes. | Adds queries. | Caution |\n\
                  | `perf_slow_ms` (default `50`) | Frames this long are logged. | More log. | Safe |\n\
                  | `bind_<action>` | skipped | x | Safe |\n";
        let e = settings_md_entries(md);
        assert_eq!(e.len(), 2);
        assert_eq!(e[0].key, "ASTC_TIME");
        assert_eq!(e[0].text, "1 times decodes. Adds queries.");
        assert_eq!(e[0].safety.as_deref(), Some("Caution"));
        assert_eq!(e[0].section.as_deref(), Some("Diagnostics"));
        assert_eq!(e[1].default.as_deref(), Some("50"));
    }

    #[test]
    fn new_keys_become_settings_and_docs_update_known_ones() {
        let mut cat = Catalog::load();
        let before = cat.settings.len();
        let added = overlay(&mut cat, &ini_entries(EXAMPLE), Weight::Replace, "totk.ini.example");
        assert_eq!(added, vec!["SHINY_NEW", "SHINY_COUNT", "SHINY_DIR"]);
        assert_eq!(cat.settings.len(), before + 3);
        let new = cat.find("SHINY_NEW").unwrap();
        assert_eq!((new.kind, new.tab.as_str(), new.values.as_deref()), (Kind::Toggle, "screen", Some("0|1")));
        let count = cat.find("SHINY_COUNT").unwrap();
        assert_eq!((count.kind, count.values.as_deref()), (Kind::Number, Some("1..8")));
        assert_eq!(cat.find("SHINY_DIR").unwrap().kind, Kind::Folder);
        assert_eq!(cat.section_of("SHINY_NEW").as_deref(), Some("display"));
        assert_eq!(cat.find("FPS_LIMIT").unwrap().doc.as_deref(), Some("Frame limit: 30 to 240."));
        // a player's totk.ini only fills gaps
        let fill = overlay(&mut cat, &ini_entries("# Stale text.\n# fps_limit = 60\n# Mine.\nmine = 1\n"), Weight::Fill, "totk.ini");
        assert_eq!(fill, vec!["MINE"]);
        assert_eq!(cat.find("FPS_LIMIT").unwrap().doc.as_deref(), Some("Frame limit: 30 to 240."));
        assert_eq!(cat.find("MINE").unwrap().tab, "expert");
    }

    #[test]
    fn repo_documents_add_nothing_unknown() {
        // the checked-in settings.json was made from these very files: overlaying them must not invent settings
        let root = concat!(env!("CARGO_MANIFEST_DIR"), "/..");
        let mut cat = Catalog::load();
        let ex = std::fs::read_to_string(format!("{root}/totk.ini.example")).unwrap();
        let md = std::fs::read_to_string(format!("{root}/docs/settings.md")).unwrap();
        let mut added = overlay(&mut cat, &ini_entries(&ex), Weight::Replace, "totk.ini.example");
        added.extend(overlay(&mut cat, &settings_md_entries(&md), Weight::Replace, "settings.md"));
        assert!(added.is_empty(), "documents name keys settings.json lacks: {added:?}");
    }
}
