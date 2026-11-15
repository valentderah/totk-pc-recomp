//! Settings profiles: named sets of settings in `profiles/<name>.ini` next to totk.ini, in the totk.ini format.
//! `profiles/active.txt` names the profile whose settings totk.ini holds. Switching stores totk.ini's settings into
//! the profile that was active (so changes made in the in-game menu stay with it) and writes the chosen profile's
//! settings into totk.ini.
use crate::ini::{self, Values};
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

pub const DIR: &str = "profiles";
const ACTIVE: &str = "active.txt";

pub struct Store {
    pub dir: PathBuf,
}

/// A profile name that makes a portable file name: 1 to 64 characters, no path separators or characters Windows
/// refuses, no leading dot or trailing blank or dot.
pub fn valid_name(name: &str) -> bool {
    let n = name.trim();
    !n.is_empty()
        && n == name
        && n.chars().count() <= 64
        && !n.starts_with('.')
        && !n.ends_with('.')
        && !n.chars().any(|c| c.is_control() || "\\/:*?\"<>|".contains(c))
        && !matches!(n.to_ascii_uppercase().as_str(), "CON" | "PRN" | "AUX" | "NUL" | "COM1" | "LPT1" | "ACTIVE")
}

impl Store {
    pub fn new(game_dir: &Path) -> Store {
        Store { dir: game_dir.join(DIR) }
    }

    pub fn path(&self, name: &str) -> PathBuf {
        self.dir.join(format!("{name}.ini"))
    }

    /// The profile names, sorted case-insensitively.
    pub fn list(&self) -> Vec<String> {
        let mut names: Vec<String> = fs::read_dir(&self.dir)
            .into_iter()
            .flatten()
            .flatten()
            .filter_map(|e| {
                let p = e.path();
                (p.extension()? == "ini").then(|| p.file_stem()?.to_str().map(str::to_string)).flatten()
            })
            .filter(|n| valid_name(n))
            .collect();
        names.sort_by_key(|n| n.to_lowercase());
        names
    }

    pub fn active(&self) -> Option<String> {
        let name = fs::read_to_string(self.dir.join(ACTIVE)).ok()?;
        let name = name.trim().to_string();
        (valid_name(&name) && self.path(&name).exists()).then_some(name)
    }

    pub fn set_active(&self, name: Option<&str>) -> io::Result<()> {
        match name {
            Some(n) => {
                fs::create_dir_all(&self.dir)?;
                fs::write(self.dir.join(ACTIVE), format!("{n}\n"))
            }
            None => match fs::remove_file(self.dir.join(ACTIVE)) {
                Err(e) if e.kind() != io::ErrorKind::NotFound => Err(e),
                _ => Ok(()),
            },
        }
    }

    pub fn read(&self, name: &str) -> io::Result<String> {
        fs::read_to_string(self.path(name))
    }

    /// Writes values into the profile's file, keeping its comments and lines like totk.ini (ini::save_text). A new
    /// file starts with a comment that says what it is.
    pub fn write(&self, name: &str, values: &Values, section_of: &dyn Fn(&str) -> Option<String>) -> io::Result<()> {
        fs::create_dir_all(&self.dir)?;
        let path = self.path(name);
        let text = match fs::read_to_string(&path) {
            Ok(t) => t,
            Err(e) if e.kind() == io::ErrorKind::NotFound => format!(
                "# TotK settings profile \"{name}\" (totk-config). Same format as totk.ini; the configurator copies \
                 these settings into totk.ini when the profile is chosen.\n"
            ),
            Err(e) => return Err(e),
        };
        let old = ini::parse(&text);
        ini::write_atomic(&path, &ini::save_text(&text, &old, values, section_of))
    }

    pub fn create(&self, name: &str, values: &Values, section_of: &dyn Fn(&str) -> Option<String>) -> io::Result<()> {
        if self.path(name).exists() {
            return Err(io::Error::new(io::ErrorKind::AlreadyExists, format!("profile {name} exists")));
        }
        self.write(name, values, section_of)
    }

    pub fn rename(&self, from: &str, to: &str) -> io::Result<()> {
        if self.path(to).exists() && !from.eq_ignore_ascii_case(to) {
            return Err(io::Error::new(io::ErrorKind::AlreadyExists, format!("profile {to} exists")));
        }
        let was_active = self.active().as_deref() == Some(from);
        fs::rename(self.path(from), self.path(to))?;
        if was_active {
            self.set_active(Some(to))?;
        }
        Ok(())
    }

    pub fn duplicate(&self, from: &str, to: &str) -> io::Result<()> {
        if self.path(to).exists() {
            return Err(io::Error::new(io::ErrorKind::AlreadyExists, format!("profile {to} exists")));
        }
        fs::copy(self.path(from), self.path(to)).map(|_| ())
    }

    pub fn delete(&self, name: &str) -> io::Result<()> {
        let was_active = self.active().as_deref() == Some(name);
        fs::remove_file(self.path(name))?;
        if was_active {
            self.set_active(None)?;
        }
        Ok(())
    }
}

/// A free name like base, "base 2", "base 3".
pub fn free_name(store: &Store, base: &str) -> String {
    let names: Vec<String> = store.list().iter().map(|n| n.to_lowercase()).collect();
    if !names.contains(&base.to_lowercase()) {
        return base.to_string();
    }
    (2..).map(|i| format!("{base} {i}")).find(|n| !names.contains(&n.to_lowercase())).unwrap()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("totk-config-test-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn names() {
        assert!(valid_name("Steam Deck-like"));
        assert!(valid_name("Качество"));
        for bad in ["", " x", "x ", "a/b", "a:b", ".hidden", "x.", "CON", "active"] {
            assert!(!valid_name(bad), "{bad:?}");
        }
    }

    #[test]
    fn create_rename_duplicate_delete() {
        let dir = temp_dir("profiles");
        let store = Store::new(&dir);
        let none = |_: &str| None;
        let mut v = Values::new();
        v.set("FPS_LIMIT", "120");
        store.create("Quality", &v, &none).unwrap();
        assert!(store.create("Quality", &v, &none).is_err());
        store.set_active(Some("Quality")).unwrap();
        assert_eq!(store.active().as_deref(), Some("Quality"));
        store.duplicate("Quality", "Copy").unwrap();
        assert_eq!(ini::parse(&store.read("Copy").unwrap()).get("FPS_LIMIT"), Some("120"));
        store.rename("Quality", "Best").unwrap();
        assert_eq!(store.active().as_deref(), Some("Best"));
        assert_eq!(store.list(), vec!["Best".to_string(), "Copy".to_string()]);
        assert_eq!(free_name(&store, "Copy"), "Copy 2");
        store.delete("Best").unwrap();
        assert_eq!(store.active(), None);
        assert_eq!(store.list(), vec!["Copy".to_string()]);
        // a profile file keeps its own comments when written again
        let path = store.path("Copy");
        let text = fs::read_to_string(&path).unwrap() + "# my note\n";
        fs::write(&path, &text).unwrap();
        v.set("FPS_LIMIT", "60");
        store.write("Copy", &v, &none).unwrap();
        let after = fs::read_to_string(&path).unwrap();
        assert!(after.contains("# my note") && after.contains("fps_limit = 60"), "{after}");
        let _ = fs::remove_dir_all(&dir);
    }
}
