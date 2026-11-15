//! totk-config: the settings configurator of the TotK PC recomp. It sits next to totk.exe and edits totk.ini (and the
//! settings profiles in profiles/) before the game starts. Every setting it shows comes from settings.json, which
//! tools/gen_configurator_settings.py writes from the runtime's sources.
//!
//!     totk-config [--dir <game folder>] [--lang en|ru|pt-BR|zh-Hans]
//!     totk-config [--dir <folder>] --set fps_limit=120 [--set fov=] ...   (no window; an empty value resets)
//!     totk-config [--dir <folder>] --new-profile <name> [--from current|defaults|quality|performance|deck]
//!     totk-config [--dir <folder>] --profile <name>                       (switch: totk.ini gets its settings)
//!     totk-config --dir <folder> --screenshot <out.bmp> [--tab <tab>] [--search <text>]   (docs and tests)
#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

mod app;
mod data;
mod docs;
mod i18n;
mod ini;
mod presets;
mod profiles;
mod theme;

use std::path::{Path, PathBuf};

pub struct Args {
    pub dir: Option<PathBuf>,
    pub sets: Vec<(String, String)>,
    pub profile: Option<String>,
    pub new_profile: Option<String>,
    pub from: Option<String>,
    pub lang: Option<String>,
    pub screenshot: Option<PathBuf>,
    pub tab: Option<String>,
    pub search: Option<String>,
    pub expand: Option<String>,
}

fn parse_args() -> Args {
    let mut a = Args {
        dir: None,
        sets: Vec::new(),
        profile: None,
        new_profile: None,
        from: None,
        lang: None,
        screenshot: None,
        tab: None,
        search: None,
        expand: None,
    };
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--dir" => a.dir = it.next().map(PathBuf::from),
            "--set" => {
                if let Some((k, v)) = it.next().as_deref().and_then(|s| s.split_once('=')) {
                    a.sets.push((k.to_string(), v.to_string()));
                }
            }
            "--profile" => a.profile = it.next(),
            "--new-profile" => a.new_profile = it.next(),
            "--from" => a.from = it.next(),
            "--lang" => a.lang = it.next(),
            "--screenshot" => a.screenshot = it.next().map(PathBuf::from),
            "--tab" => a.tab = it.next(),
            "--search" => a.search = it.next(),
            "--expand" => a.expand = it.next(),
            other if !other.starts_with("--") && a.dir.is_none() => a.dir = Some(PathBuf::from(other)),
            _ => {}
        }
    }
    a
}

/// A folder the game reads totk.ini from: the runtime looks next to totk.exe (cfg.c find_ini) and nowhere else.
pub fn is_game_dir(dir: &Path) -> bool {
    dir.join("totk.exe").is_file() || dir.join("totk.ini").is_file()
}

/// The per-user folder the installer keeps its state in (installer/recent.py), where the configurator remembers the
/// last game folder.
fn state_dir() -> Option<PathBuf> {
    if cfg!(windows) {
        std::env::var_os("LOCALAPPDATA").map(|d| PathBuf::from(d).join("TotK Recomp"))
    } else {
        std::env::var_os("XDG_STATE_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/state")))
            .map(|d| d.join("totk-recomp"))
    }
}

pub fn remember_dir(dir: &Path) {
    if let Some(s) = state_dir() {
        let _ = std::fs::create_dir_all(&s);
        let _ = std::fs::write(s.join("configurator-dir.txt"), dir.to_string_lossy().as_bytes());
    }
}

/// The game folder: --dir, else the configurator's own folder when the game is there (it ships next to totk.exe),
/// else the folder used last, else the installer's last install folder (setup.json "dest").
fn find_game_dir(args: &Args) -> Option<PathBuf> {
    if let Some(d) = &args.dir {
        return Some(d.clone());
    }
    if let Some(exe_dir) = std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)) {
        if is_game_dir(&exe_dir) {
            return Some(exe_dir);
        }
    }
    let state = state_dir()?;
    if let Ok(last) = std::fs::read_to_string(state.join("configurator-dir.txt")) {
        let last = PathBuf::from(last.trim());
        if is_game_dir(&last) {
            return Some(last);
        }
    }
    let setup: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(state.join("setup.json")).ok()?).ok()?;
    let dest = PathBuf::from(setup.get("dest")?.as_str()?);
    is_game_dir(&dest).then_some(dest)
}

/// The command line without a window: --set, --new-profile, --profile. Prints to the console it was started from.
fn headless(args: Args, dir: Option<PathBuf>) -> std::process::ExitCode {
    #[cfg(windows)]
    {
        #[link(name = "kernel32")]
        extern "system" {
            fn AttachConsole(pid: u32) -> i32;
        }
        // SAFETY: ATTACH_PARENT_PROCESS; fails harmlessly when there is no console
        unsafe { AttachConsole(u32::MAX) };
    }
    let preset = match args.from.as_deref().unwrap_or("current") {
        "current" => presets::Preset::Current,
        "defaults" => presets::Preset::Defaults,
        "quality" => presets::Preset::Quality,
        "performance" => presets::Preset::Performance,
        "deck" => presets::Preset::SteamDeck,
        other => {
            eprintln!("--from {other}: one of current, defaults, quality, performance, deck");
            return std::process::ExitCode::FAILURE;
        }
    };
    let (sets, profile, new_profile) = (args.sets.clone(), args.profile.clone(), args.new_profile.clone());
    let mut app = app::App::new(args, dir);
    match app.run_headless(&sets, profile.as_deref(), new_profile.as_deref().map(|n| (n, preset))) {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("totk-config: {e}");
            std::process::ExitCode::FAILURE
        }
    }
}

fn main() -> std::process::ExitCode {
    let args = parse_args();
    let dir = find_game_dir(&args);
    if !args.sets.is_empty() || args.profile.is_some() || args.new_profile.is_some() {
        return headless(args, dir);
    }
    match window(args, dir) {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("totk-config: {e}");
            std::process::ExitCode::FAILURE
        }
    }
}

fn window(args: Args, dir: Option<PathBuf>) -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: eframe::egui::ViewportBuilder::default()
            .with_title("TotK Settings")
            .with_inner_size([1240.0, 820.0])
            .with_min_inner_size([920.0, 600.0]),
        ..Default::default()
    };
    eframe::run_native(
        "totk-config",
        options,
        Box::new(move |cc| {
            theme::install(&cc.egui_ctx);
            Ok(Box::new(app::App::new(args, dir)))
        }),
    )
}
