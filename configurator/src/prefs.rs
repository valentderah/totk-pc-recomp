//! The configurator's own settings, apart from totk.ini (the game never reads them): `configurator.json` in the
//! per-user folder the installer keeps its state in (`%LOCALAPPDATA%\TotK Recomp`, main.rs state_dir).
use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
pub struct Prefs {
    /// Ask GitHub for the latest release when the window opens (off until the player turns it on).
    #[serde(default)]
    pub check_updates: bool,
}

fn path() -> Option<PathBuf> {
    crate::state_dir().map(|d| d.join("configurator.json"))
}

impl Prefs {
    pub fn load() -> Prefs {
        path().and_then(|p| std::fs::read_to_string(p).ok()).and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
    }
    pub fn save(&self) -> std::io::Result<()> {
        let p = path().ok_or_else(|| std::io::Error::other("no per-user folder"))?;
        if let Some(d) = p.parent() {
            std::fs::create_dir_all(d)?;
        }
        std::fs::write(p, serde_json::to_string_pretty(self).unwrap_or_default())
    }
}
