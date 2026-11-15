//! totk.ini as the runtime reads and writes it (runtime/cfg.c cfg_load and cfg_save_to), so the configurator and the
//! in-game menu never fight over the file.
//!
//! Reading: "key = value" lines; a line starting with '#' or ';' is a comment and '[section]' lines only group. A key is
//! a variable name without "TOTK_" (an optional TOTK_ prefix is accepted), case-insensitive. The value is the rest of
//! the line, trimmed, optionally in double quotes; an empty value leaves the setting unset. A repeated key: the later
//! line wins. A UTF-8 BOM is skipped.
//!
//! Writing (`save_text`): only the keys whose value changed are touched. The last active line of a changed key gets
//! the new value with the line's own key spelling and indentation; a key without a line goes under its section, after
//! the commented example line of the key when the section has one, else after the section's last non-empty line; a
//! section the file lacks is added at the end. A key that is no longer set loses all its active lines; commented
//! example lines stay. Every other byte of the file stays as it was, line endings included.

/// Setting values in first-seen order: key upper case without the TOTK_ prefix, value non-empty.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Values(Vec<(String, String)>);

impl Values {
    pub fn new() -> Self {
        Values(Vec::new())
    }
    pub fn get(&self, key: &str) -> Option<&str> {
        self.0.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str())
    }
    /// Sets key to value; an empty value removes the key, as an empty value in the file means unset.
    pub fn set(&mut self, key: &str, value: &str) {
        if value.is_empty() {
            self.remove(key);
        } else if let Some(e) = self.0.iter_mut().find(|(k, _)| k == key) {
            e.1 = value.to_string();
        } else {
            self.0.push((key.to_string(), value.to_string()));
        }
    }
    pub fn remove(&mut self, key: &str) {
        self.0.retain(|(k, _)| k != key);
    }
    pub fn iter(&self) -> impl Iterator<Item = (&str, &str)> {
        self.0.iter().map(|(k, v)| (k.as_str(), v.as_str()))
    }
    pub fn len(&self) -> usize {
        self.0.len()
    }
}

/// cfg.c trim: blanks and tabs at the start, blanks, tabs and '\r' at the end.
fn trim(s: &str) -> &str {
    s.trim_start_matches([' ', '\t']).trim_end_matches([' ', '\t', '\r'])
}

/// cfg.c norm_key: upper case, without the TOTK_ prefix.
pub fn norm_key(key: &str) -> String {
    let up = key.to_ascii_uppercase();
    match up.strip_prefix("TOTK_") {
        Some(rest) => rest.to_string(),
        None => up,
    }
}

fn strip_bom(text: &str) -> &str {
    text.strip_prefix('\u{feff}').unwrap_or(text)
}

/// The file's values as cfg.c cfg_load reads them, before its migration of old values.
pub fn parse_raw(text: &str) -> Values {
    let mut v = Values::new();
    for line in strip_bom(text).split('\n') {
        let s = trim(line);
        if s.is_empty() || s.starts_with(['#', ';', '[']) {
            continue;
        }
        let Some(eq) = s.find('=') else { continue };
        let key = norm_key(trim(&s[..eq]));
        let mut value = trim(&s[eq + 1..]);
        if value.len() >= 2 && value.starts_with('"') && value.ends_with('"') {
            value = &value[1..value.len() - 1];
        }
        if key.is_empty() || value.is_empty() {
            continue;
        }
        v.set(&key, value);
    }
    v
}

/// The values the game uses: parse_raw plus cfg.c migrate, which reads 0.1.9's pad_layout = xbox_prompts as
/// pad_layout = nintendo with prompts = xbox (a prompts line of the file's own wins). Saving the result writes the
/// migrated lines, as the game's next cfg_save would.
pub fn parse(text: &str) -> Values {
    let mut v = parse_raw(text);
    if v.get("PAD_LAYOUT").is_some_and(|l| l.eq_ignore_ascii_case("xbox_prompts")) {
        v.set("PAD_LAYOUT", "nintendo");
        if v.get("PROMPTS").is_none() {
            v.set("PROMPTS", "xbox");
        }
    }
    v
}

/// The key of an active "key = value" line (comment = false) or of a commented "# key = value" line (comment = true),
/// normalised; None when the line is not one. cfg.c line_key: the key is letters, digits and '_' only.
fn line_key(text: &str, comment: bool) -> Option<String> {
    let mut t = text;
    if comment {
        t = t.strip_prefix(['#', ';'])?;
        t = t.trim_start_matches([' ', '\t']);
    }
    if t.is_empty() || t.starts_with(['#', ';', '[']) {
        return None;
    }
    let eq = t.find('=')?;
    let key = t[..eq].trim_end_matches([' ', '\t']);
    if key.is_empty() || key.len() >= 128 || !key.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_') {
        return None;
    }
    Some(norm_key(key))
}

/// The lower-case name of a "[name]" line.
fn line_section(text: &str) -> Option<String> {
    let rest = text.strip_prefix('[')?;
    let end = rest.find(']')?;
    Some(trim(&rest[..end]).to_ascii_lowercase())
}

/// A line of the file with its ending ('\n', and '\r' before it, when present).
struct Line<'a> {
    raw: &'a str,
}

impl Line<'_> {
    /// The text without its ending, BOM and surrounding blanks.
    fn text(&self, first: bool) -> &str {
        let mut s = self.raw;
        if first {
            s = strip_bom(s);
        }
        trim(s.trim_end_matches(['\n', '\r']))
    }
}

fn split_lines(text: &str) -> Vec<Line<'_>> {
    let mut lines = Vec::new();
    let mut start = 0;
    for (i, b) in text.bytes().enumerate() {
        if b == b'\n' {
            lines.push(Line { raw: &text[start..=i] });
            start = i + 1;
        }
    }
    if start < text.len() {
        lines.push(Line { raw: &text[start..] });
    }
    lines
}

/// Where a changed key goes.
#[derive(Clone, Copy, PartialEq)]
enum Place {
    Clean,
    Replace,
    After(usize),
    NewSection,
    Done,
}

/// The file text after changing its values from `old` to `new` (cfg.c cfg_save_to). `section_of` names the totk.ini
/// section of a key (cfg.c sections[]); None puts a key the file lacks at the end without a section.
pub fn save_text(original: &str, old: &Values, new: &Values, section_of: &dyn Fn(&str) -> Option<String>) -> String {
    // the changes in cfg.c terms: dirty entries carry a new value, removed ones lose their lines
    let mut dirty: Vec<(&str, &str)> = Vec::new();
    let mut removed: Vec<&str> = Vec::new();
    for (k, v) in new.iter() {
        if old.get(k) != Some(v) {
            dirty.push((k, v));
        }
    }
    for (k, _) in old.iter() {
        if new.get(k).is_none() {
            removed.push(k);
        }
    }
    // A "key =" line of a key that stays unset is no change and stays; once the key gets a value that line, as the
    // key's last line, carries it (as in cfg.c, where line_key matches it).
    let lines = split_lines(original);
    let texts: Vec<&str> = lines.iter().enumerate().map(|(i, l)| l.text(i == 0)).collect();
    let keys: Vec<Option<String>> = texts.iter().map(|t| line_key(t, false)).collect();
    if dirty.is_empty() && removed.is_empty() {
        return original.to_string();
    }
    let eol = if original.contains("\r\n") { "\r\n" } else { "\n" };
    let mut drop = vec![false; lines.len()];
    let mut replace: Vec<Option<usize>> = vec![None; lines.len()];
    let mut place = vec![Place::Clean; dirty.len()];
    for k in &removed {
        for (i, lk) in keys.iter().enumerate() {
            if lk.as_deref() == Some(*k) {
                drop[i] = true;
            }
        }
    }
    for (e, (key, _)) in dirty.iter().enumerate() {
        if let Some(last) = keys.iter().rposition(|k| k.as_deref() == Some(*key)) {
            replace[last] = Some(e);
            place[e] = Place::Replace;
            continue;
        }
        let want = section_of(key);
        let (mut inside, mut example, mut tail) = (false, None, None);
        if let Some(want) = &want {
            for (i, t) in texts.iter().enumerate() {
                if let Some(sec) = line_section(t) {
                    inside = &sec == want;
                    if inside {
                        tail = Some(i);
                    }
                    continue;
                }
                if !inside {
                    continue;
                }
                if !t.is_empty() {
                    tail = Some(i);
                }
                if line_key(t, true).as_deref() == Some(*key) {
                    example = Some(i);
                }
            }
        }
        place[e] = match example.or(tail) {
            Some(i) => Place::After(i),
            None => Place::NewSection,
        };
    }
    let mut out = String::with_capacity(original.len() + 64 * dirty.len());
    let entry = |out: &mut String, key: &str, value: &str| {
        out.push_str(&key.to_ascii_lowercase());
        out.push_str(" = ");
        out.push_str(value);
        out.push_str(eol);
    };
    for (i, line) in lines.iter().enumerate() {
        if drop[i] {
            // the line leaves the file
        } else if let Some(e) = replace[i] {
            // keep the line's indentation and key spelling, put the new value after " = "
            let mut s = line.raw;
            if i == 0 && s.starts_with('\u{feff}') {
                out.push('\u{feff}');
                s = &s[3..];
            }
            let eq = s.find('=').expect("an active line has '='");
            out.push_str(s[..eq].trim_end_matches([' ', '\t']));
            out.push_str(" = ");
            out.push_str(dirty[e].1);
            if s.ends_with('\n') {
                out.push_str(eol);
            }
        } else {
            out.push_str(line.raw);
        }
        for (e, (key, value)) in dirty.iter().enumerate() {
            if place[e] == Place::After(i) {
                if !out.is_empty() && !out.ends_with('\n') {
                    out.push_str(eol);
                }
                entry(&mut out, key, value);
            }
        }
    }
    // keys whose section the file lacks: one new section each, at the end
    for e in 0..dirty.len() {
        if place[e] != Place::NewSection {
            continue;
        }
        let want = section_of(dirty[e].0);
        if !out.is_empty() && !out.ends_with('\n') {
            out.push_str(eol);
        }
        if let Some(want) = &want {
            if !out.is_empty() {
                out.push_str(eol);
            }
            out.push('[');
            out.push_str(want);
            out.push(']');
            out.push_str(eol);
        }
        entry(&mut out, dirty[e].0, dirty[e].1);
        // the following keys of the same new section go right after this one
        if let Some(want) = &want {
            for f in e + 1..dirty.len() {
                if place[f] == Place::NewSection && section_of(dirty[f].0).as_ref() == Some(want) {
                    entry(&mut out, dirty[f].0, dirty[f].1);
                    place[f] = Place::Done;
                }
            }
        }
    }
    out
}

/// Writes text to path through a temporary file in the same folder and a rename, so a crash never leaves half a file.
pub fn write_atomic(path: &std::path::Path, text: &str) -> std::io::Result<()> {
    let tmp = path.with_extension("ini.tmp");
    std::fs::write(&tmp, text)?;
    std::fs::rename(&tmp, path).inspect_err(|_| {
        let _ = std::fs::remove_file(&tmp);
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sections(key: &str) -> Option<String> {
        match key {
            "FPS_LIMIT" | "FPS60" => Some("frame rate".into()),
            "FULLSCREEN" | "FOV" => Some("display".into()),
            "VOLUME" => Some("audio".into()),
            _ if key.starts_with("CHEAT_") => Some("cheats".into()),
            _ => None,
        }
    }

    fn edit(text: &str, f: impl FnOnce(&mut Values)) -> String {
        let old = parse(text);
        let mut new = old.clone();
        f(&mut new);
        save_text(text, &old, &new, &sections)
    }

    #[test]
    fn reads_like_cfg_load() {
        let v = parse("\u{feff}# c\n[display]\nTOTK_Fullscreen = 1\r\n fov=\"120\" \n; x = 1\nvolume =\nfov = 110\nbad line\n");
        assert_eq!(v.get("FULLSCREEN"), Some("1"));
        assert_eq!(v.get("FOV"), Some("110"));
        assert_eq!(v.get("VOLUME"), None);
        assert_eq!(v.get("X"), None);
        assert_eq!(v.len(), 2);
    }

    #[test]
    fn migrates_xbox_prompts() {
        let v = parse("pad_layout = xbox_prompts\n");
        assert_eq!(v.get("PAD_LAYOUT"), Some("nintendo"));
        assert_eq!(v.get("PROMPTS"), Some("xbox"));
        let v = parse("pad_layout = xbox_prompts\nprompts = pc\n");
        assert_eq!(v.get("PROMPTS"), Some("pc"));
    }

    #[test]
    fn unchanged_is_byte_identical() {
        let text = "# keep\r\n[frame rate]\r\n# fps_limit = 60\r\nfps_limit = 90\r\nunknown_key = 5\r\n";
        assert_eq!(edit(text, |_| {}), text);
    }

    #[test]
    fn replaces_last_line_keeping_spelling() {
        let text = "  FPS_Limit=30 # not a comment\nfps_limit = 45\n";
        assert_eq!(edit(text, |v| v.set("FPS_LIMIT", "120")), "  FPS_Limit=30 # not a comment\nfps_limit = 120\n");
    }

    #[test]
    fn inserts_after_commented_example() {
        let text = "[frame rate]\n# Frame limit.\n# fps_limit = 60\n# fps60 = 1\n\n[display]\n# fov = 100\n";
        let out = edit(text, |v| {
            v.set("FPS_LIMIT", "90");
            v.set("FULLSCREEN", "1");
        });
        assert_eq!(out, "[frame rate]\n# Frame limit.\n# fps_limit = 60\nfps_limit = 90\n# fps60 = 1\n\n[display]\n# fov = 100\nfullscreen = 1\n");
    }

    #[test]
    fn adds_missing_sections_at_the_end() {
        let out = edit("a = 1", |v| {
            v.set("VOLUME", "50");
            v.set("FOV", "120");
            v.set("FULLSCREEN", "1");
            v.set("CHEAT_X", "1");
            v.set("NOSECTION", "y");
        });
        assert_eq!(out, "a = 1\n\n[audio]\nvolume = 50\n\n[display]\nfov = 120\nfullscreen = 1\n\n[cheats]\ncheat_x = 1\nnosection = y\n");
    }

    #[test]
    fn reset_drops_active_lines_only() {
        let text = "[display]\n# fov = 100\nfov = 120\nfov = 130\nfullscreen = 1\n";
        assert_eq!(edit(text, |v| v.remove("FOV")), "[display]\n# fov = 100\nfullscreen = 1\n");
    }

    #[test]
    fn empty_file_and_crlf() {
        assert_eq!(edit("", |v| v.set("FOV", "90")), "[display]\nfov = 90\n");
        let out = edit("x = 1\r\n", |v| v.set("FOV", "90"));
        assert_eq!(out, "x = 1\r\n\r\n[display]\r\nfov = 90\r\n");
    }

    #[test]
    fn keeps_unknown_keys_and_comments_through_edits() {
        let text = "; my notes\n[display]\nfov = 100 \nmy_mod_key = keep me\n[cheats]\ncheat_inf = 1\n";
        let out = edit(text, |v| v.set("FOV", "125"));
        assert_eq!(out, "; my notes\n[display]\nfov = 125\nmy_mod_key = keep me\n[cheats]\ncheat_inf = 1\n");
        assert_eq!(parse(&out).get("MY_MOD_KEY"), Some("keep me"));
    }

    #[test]
    fn last_line_without_newline() {
        assert_eq!(edit("fov = 100", |v| v.set("FOV", "90")), "fov = 90");
        let out = edit("[display]\nfov = 100", |v| v.set("FULLSCREEN", "1"));
        assert_eq!(out, "[display]\nfov = 100\nfullscreen = 1\n");
    }
}
