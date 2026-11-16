//! The window: profiles and tabs on the left, the settings of the tab (or of a search) in the middle, save and launch
//! at the bottom. The working copy of the settings is `work`; `disk` is what totk.ini held when it was last read or
//! written. Saving applies only the difference between the two to the file as it is on disk then, so a change the
//! in-game menu wrote meanwhile to another key survives.
use crate::data::{tr, Catalog, Kind, Setting};
use crate::i18n::t;
use crate::ini::{self, Values};
use crate::presets::{self, Preset};
use crate::profiles::{self, Store};
use crate::theme::{self, BG, CARD, CARD_HOVER, GOLD, RED, WEAK};
use crate::Args;
use eframe::egui::{self, Align, Color32, Layout, RichText, Ui};
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::SystemTime;

const EXPERT: &str = "expert";
const OTHER: &str = "other";
const UPDATES: &str = "updates";
const LANGS: [(&str, &str); 4] = [("en", "English"), ("ru", "Русский"), ("pt-BR", "Português (Brasil)"), ("zh-Hans", "简体中文")];

enum Dialog {
    None,
    New { name: String, preset: Preset, switch: bool },
    Rename { from: String, name: String },
    Delete(String),
    /// Confirm downloading and installing the latest release.
    Update,
}

pub struct App {
    cat: Rc<Catalog>,
    lang: String,
    dir: Option<PathBuf>,
    ini_text: String,
    disk: Values,
    work: Values,
    ini_mtime: Option<SystemTime>,
    changed_on_disk: bool,
    store: Option<Store>,
    profiles: Vec<String>,
    active: Option<String>,
    tab: String,
    search: String,
    only_changed: bool,
    status: Option<(String, bool, f64)>,
    dialog: Dialog,
    bind_edit: Option<String>,
    capture: Option<String>,
    other_key: String,
    other_value: String,
    last_check: f64,
    screenshot: Option<PathBuf>,
    frames: u32,
    /// Where the run-time settings and descriptions came from (docs.rs).
    sources: crate::docs::Sources,
    /// The configurator's own settings (prefs.rs).
    prefs: crate::prefs::Prefs,
    /// The update worker's state (update.rs), the latest release seen, the version in the game folder.
    upd: crate::update::Shared,
    latest: Option<crate::update::Release>,
    installed: crate::update::Installed,
    /// Run the setup as soon as the download is unpacked (the player confirmed).
    install_when_ready: bool,
    /// --show update: open the confirmation once the check has an answer (screenshots).
    show_update: bool,
    /// --screenshot: when the open modal appeared, and whether the screenshot was asked for.
    shot_modal_at: Option<f64>,
    shot_sent: bool,
}

/// The Windows UI language as a menu language (GetUserDefaultUILanguage's primary language id).
fn system_lang() -> String {
    #[cfg(windows)]
    {
        #[link(name = "kernel32")]
        extern "system" {
            fn GetUserDefaultUILanguage() -> u16;
        }
        // SAFETY: no arguments, returns a LANGID.
        let id = unsafe { GetUserDefaultUILanguage() };
        return match id & 0x3ff {
            0x19 => "ru",
            0x16 if id == 0x0416 => "pt-BR",
            0x04 if id != 0x0404 && id != 0x0c04 && id != 0x1404 => "zh-Hans",
            _ => "en",
        }
        .to_string();
    }
    #[allow(unreachable_code)]
    std::env::var("LANG").ok().filter(|l| l.starts_with("ru")).map(|_| "ru".to_string()).unwrap_or_else(|| "en".into())
}

fn mtime(path: &Path) -> Option<SystemTime> {
    std::fs::metadata(path).and_then(|m| m.modified()).ok()
}

/// Same keys and values, in any order.
fn same(a: &Values, b: &Values) -> bool {
    a.len() == b.len() && a.iter().all(|(k, v)| b.get(k) == Some(v))
}

/// "1920x1080" as "1920 × 1080", "0.500" as "0.5".
fn pretty_number(x: f64, step: f64) -> String {
    if step >= 1.0 && x.fract() == 0.0 {
        return format!("{}", x as i64);
    }
    let s = format!("{x:.3}");
    s.trim_end_matches('0').trim_end_matches('.').to_string()
}

impl App {
    pub fn new(args: Args, dir: Option<PathBuf>) -> App {
        let cat = Rc::new(Catalog::load());
        let mut app = App {
            lang: String::new(),
            cat,
            dir: None,
            ini_text: String::new(),
            disk: Values::new(),
            work: Values::new(),
            ini_mtime: None,
            changed_on_disk: false,
            store: None,
            profiles: Vec::new(),
            active: None,
            tab: args.tab.clone().unwrap_or_else(|| "screen".into()),
            search: args.search.clone().unwrap_or_default(),
            only_changed: false,
            status: None,
            dialog: Dialog::None,
            bind_edit: args.expand.clone(),
            capture: None,
            other_key: String::new(),
            other_value: String::new(),
            last_check: 0.0,
            screenshot: args.screenshot.clone(),
            frames: 0,
            sources: Default::default(),
            prefs: crate::prefs::Prefs::load(),
            upd: Default::default(),
            latest: None,
            installed: Default::default(),
            install_when_ready: false,
            show_update: args.show.as_deref() == Some("update"),
            shot_modal_at: None,
            shot_sent: false,
        };
        if let Some(d) = dir {
            app.open_dir(d);
        }
        if let Some(name) = args.show.as_deref().and_then(|s| s.strip_prefix("new:")) {
            let preset = presets::ALL.into_iter().find(|p| format!("{p:?}").eq_ignore_ascii_case(name)).unwrap_or(Preset::Current);
            app.dialog = Dialog::New { name: "Potato".into(), preset, switch: true };
        }
        // the Updates tab asked for on the command line, or --show update, checks at once
        let wants_check = app.tab == UPDATES || app.show_update;
        if (app.prefs.check_updates && app.screenshot.is_none() || wants_check) && cfg!(windows) {
            crate::update::check_async(app.upd.clone());
        }
        // the language: --lang, else the menu language totk.ini sets, else the system's
        app.lang = args
            .lang
            .clone()
            .or_else(|| app.work.get("MENU_LANG").map(str::to_string))
            .filter(|l| LANGS.iter().any(|(c, _)| c == l))
            .unwrap_or_else(system_lang);
        app
    }

    fn ini_path(&self) -> Option<PathBuf> {
        self.dir.as_ref().map(|d| d.join("totk.ini"))
    }

    fn open_dir(&mut self, dir: PathBuf) {
        // the settings and descriptions of the documents next to this game, read now (docs.rs)
        let (cat, sources) = crate::docs::load(Some(&dir));
        self.cat = Rc::new(cat);
        self.sources = sources;
        self.installed = crate::update::installed(&dir);
        self.store = Some(Store::new(&dir));
        self.dir = Some(dir);
        self.reload();
        self.refresh_profiles();
    }

    fn reload(&mut self) {
        let Some(path) = self.ini_path() else { return };
        self.ini_text = std::fs::read_to_string(&path).unwrap_or_default();
        self.disk = ini::parse(&self.ini_text);
        self.work = self.disk.clone();
        self.ini_mtime = mtime(&path);
        self.changed_on_disk = false;
    }

    fn refresh_profiles(&mut self) {
        if let Some(s) = &self.store {
            self.profiles = s.list();
            self.active = s.active();
        }
    }

    fn dirty(&self) -> bool {
        !same(&self.work, &self.disk)
    }

    /// The keys whose value differs from totk.ini's, for the unsaved-changes tooltip.
    fn changed_keys(&self) -> Vec<String> {
        let mut keys: Vec<String> = self.work.iter().filter(|(k, v)| self.disk.get(k) != Some(*v)).map(|(k, _)| k.to_ascii_lowercase()).collect();
        keys.extend(self.disk.iter().filter(|(k, _)| self.work.get(k).is_none()).map(|(k, _)| format!("{} ({})", k.to_ascii_lowercase(), t(&self.lang, "unset"))));
        keys
    }

    fn say(&mut self, ui: &Ui, text: impl Into<String>, error: bool) {
        self.status = Some((text.into(), error, ui.input(|i| i.time)));
    }


    /// Writes `target` as totk.ini's settings: the keys that differ from what the file holds now change, the rest of
    /// the file stays. Returns false (and says why) when the file cannot be written.
    fn write_ini(&mut self, target: &Values) -> std::io::Result<()> {
        let path = self.ini_path().ok_or_else(|| std::io::Error::other("no game folder"))?;
        let fresh = std::fs::read_to_string(&path).or_else(|e| match e.kind() {
            std::io::ErrorKind::NotFound => Ok(String::new()),
            _ => Err(e),
        })?;
        let cat = self.cat.clone();
        let text = ini::save_text(&fresh, &ini::parse_raw(&fresh), target, &|k| cat.section_of(k));
        if text != fresh {
            ini::write_atomic(&path, &text)?;
        }
        self.ini_text = text;
        self.disk = ini::parse(&self.ini_text);
        self.work = self.disk.clone();
        self.ini_mtime = mtime(&path);
        self.changed_on_disk = false;
        Ok(())
    }

    /// Saves the edits: they go onto the file as it is now (keys the game changed meanwhile and these edits leave
    /// alone keep the game's value), and into the active profile.
    pub fn save_core(&mut self) -> std::io::Result<()> {
        let fresh = self.ini_path().and_then(|p| std::fs::read_to_string(p).ok()).unwrap_or_default();
        let mut target = ini::parse(&fresh);
        for (k, v) in self.work.iter() {
            if self.disk.get(k) != Some(v) {
                target.set(k, v);
            }
        }
        for (k, _) in self.disk.iter() {
            if self.work.get(k).is_none() {
                target.remove(k);
            }
        }
        self.write_ini(&target)?;
        self.store_active()
    }

    fn save(&mut self, ui: &Ui) -> bool {
        match self.save_core() {
            Ok(()) => {
                self.say(ui, t(&self.lang, "Saved"), false);
                true
            }
            Err(e) => {
                self.say(ui, format!("{}: {e}", t(&self.lang, "Could not save")), true);
                false
            }
        }
    }

    /// totk.ini's settings into the active profile's file.
    fn store_active(&self) -> std::io::Result<()> {
        if let (Some(store), Some(name)) = (&self.store, &self.active) {
            let cat = self.cat.clone();
            store.write(name, &self.disk, &|k| cat.section_of(k))?;
        }
        Ok(())
    }

    /// Makes `name` the active profile: unsaved edits are saved first, totk.ini's settings go into the profile that
    /// was active (so changes the in-game menu made stay with it), then the chosen profile's settings become totk.ini's.
    pub fn switch_core(&mut self, name: &str) -> std::io::Result<()> {
        if self.dirty() {
            self.save_core()?;
        } else {
            self.store_active()?;
        }
        let store = self.store.as_ref().ok_or_else(|| std::io::Error::other("no game folder"))?;
        let values = ini::parse(&store.read(name)?);
        self.write_ini(&values)?;
        self.store.as_ref().unwrap().set_active(Some(name))?;
        self.refresh_profiles();
        Ok(())
    }

    fn switch_to(&mut self, ui: &Ui, name: &str) {
        match self.switch_core(name) {
            Ok(()) => self.say(ui, format!("{} \"{name}\"", t(&self.lang, "Switched to profile")), false),
            Err(e) => self.say(ui, format!("{}: {e}", t(&self.lang, "Could not save")), true),
        }
        self.refresh_profiles();
    }

    /// The command line's edits (--set key=value, an empty value resets; --profile name), without a window.
    pub fn run_headless(&mut self, sets: &[(String, String)], profile: Option<&str>, new_profile: Option<(&str, Preset)>) -> Result<(), String> {
        if self.dir.is_none() {
            return Err("no game folder (pass --dir)".into());
        }
        if let Some((name, preset)) = new_profile {
            if !profiles::valid_name(name) {
                return Err(format!("{name:?} is not a usable profile name"));
            }
            let values = presets::values(preset, &self.cat, &self.work);
            let cat = self.cat.clone();
            self.store.as_ref().unwrap().create(name, &values, &|k| cat.section_of(k)).map_err(|e| e.to_string())?;
            self.refresh_profiles();
        }
        if let Some(p) = profile {
            self.switch_core(p).map_err(|e| format!("profile {p}: {e}"))?;
        }
        for (k, v) in sets {
            let key = ini::norm_key(k.trim());
            if let Some(s) = self.cat.find(&key) {
                if !v.is_empty() && !s.accepts(v) {
                    return Err(format!("{}: {v} is not one of {}", k, s.values.as_deref().unwrap_or("?")));
                }
            }
            self.work.set(&key, v.trim());
        }
        if !sets.is_empty() {
            self.save_core().map_err(|e| e.to_string())?;
        }
        Ok(())
    }
    fn launch(&mut self, ui: &Ui) {
        if self.dirty() && !self.save(ui) {
            return;
        }
        let Some(dir) = self.dir.clone() else { return };
        let exe = dir.join(if cfg!(windows) { "totk.exe" } else { "totk" });
        match std::process::Command::new(&exe).current_dir(&dir).spawn() {
            Ok(_) => self.say(ui, t(&self.lang, "Launched totk.exe"), false),
            Err(e) => self.say(ui, format!("{}: {e}", t(&self.lang, "Could not launch totk.exe")), true),
        }
    }

    // ---- values
    fn value<'a>(&'a self, s: &'a Setting) -> Option<&'a str> {
        self.work.get(&s.key)
    }

    /// The value in effect: the set one, else the fallback key's (per-stick dead zones), else the default.
    fn effective(&self, s: &Setting) -> String {
        if let Some(v) = self.work.get(&s.key) {
            return v.to_string();
        }
        if let Some(fb) = &s.fallback {
            if let Some(v) = self.work.get(fb) {
                return v.to_string();
            }
            if let Some(f) = self.cat.find(fb) {
                return f.default.clone();
            }
        }
        s.default.clone()
    }

    fn choice_label(&self, s: &Setting, value: &str) -> String {
        if let Some(c) = s.choices.iter().flatten().find(|c| c.value == value) {
            if let Some(l) = tr(&c.label, &self.lang) {
                return l.to_string();
            }
        }
        if value.is_empty() {
            return tr(&self.cat.common["auto"], &self.lang).unwrap_or("Automatic").to_string();
        }
        if let Some((w, h)) = value.split_once('x') {
            if w.parse::<u32>().is_ok() && h.parse::<u32>().is_ok() {
                return format!("{w} × {h}");
            }
        }
        match s.key.as_str() {
            "RENDER_SCALE" => return format!("{value} %"),
            "ANISO" => return format!("{value}x"),
            "WALK_KEY" => return self.key_label(value),
            _ => {}
        }
        match value {
            "0" if s.values.as_deref() == Some("0|1") => tr(&self.cat.common["off"], &self.lang).unwrap_or("Off").into(),
            "1" if s.values.as_deref() == Some("0|1") => tr(&self.cat.common["on"], &self.lang).unwrap_or("On").into(),
            _ => value.to_string(),
        }
    }

    fn key_label(&self, name: &str) -> String {
        if let Some(l) = self.cat.input.key_labels.get(name) {
            if let Some(v) = l.get(&self.lang).or_else(|| l.get("en")) {
                return v.clone();
            }
        }
        if let Some(n) = name.strip_prefix("num") {
            if n.len() == 1 {
                return format!("Num {n}");
            }
        }
        if name.len() <= 3 {
            name.to_uppercase()
        } else {
            let mut c = name.chars();
            c.next().map(|f| f.to_uppercase().collect::<String>() + c.as_str()).unwrap_or_default().replace('_', " ")
        }
    }

    fn pad_label(&self, name: &str) -> String {
        self.cat
            .input
            .pad_labels
            .get(name)
            .and_then(|l| l.get(&self.lang).or_else(|| l.get("en")))
            .cloned()
            .unwrap_or_else(|| name.replace('_', " "))
    }

    fn label_of(&self, s: &Setting) -> String {
        tr(&s.label, &self.lang).map(str::to_string).unwrap_or_else(|| s.key.to_ascii_lowercase())
    }

    fn matches(&self, s: &Setting, needle: &str) -> bool {
        let n = needle.to_lowercase();
        let hit = |t: &Option<crate::data::Text>| {
            t.as_ref().is_some_and(|m| m.values().any(|v| v.to_lowercase().contains(&n)))
        };
        s.key.to_lowercase().contains(&n)
            || hit(&s.label)
            || hit(&s.desc)
            || s.doc.as_deref().is_some_and(|d| d.to_lowercase().contains(&n))
    }

    // ---- the frame
    fn top_bar(&mut self, ui: &mut Ui) {
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            ui.label(RichText::new("◆").color(GOLD).size(22.0));
            ui.heading(RichText::new(t(&self.lang, "TotK Settings")).strong());
            ui.add_space(16.0);
            ui.label(RichText::new(t(&self.lang, "Game folder")).color(WEAK));
            let shown = self.dir.as_ref().map(|d| d.display().to_string()).unwrap_or_else(|| "—".into());
            let room = (ui.available_width() - 420.0).max(120.0);
            ui.scope(|ui| {
                ui.set_max_width(room);
                ui.add(egui::Label::new(RichText::new(&shown).monospace()).truncate()).on_hover_text(&shown);
            });
            if ui.button(t(&self.lang, "Change…")).clicked() {
                if let Some(d) = rfd::FileDialog::new().set_title(t(&self.lang, "Choose the folder of totk.exe")).pick_folder() {
                    self.open_dir(d);
                    crate::remember_dir(self.dir.as_ref().unwrap());
                }
            }
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let current = LANGS.iter().find(|(c, _)| *c == self.lang).map(|(_, n)| *n).unwrap_or("English");
                egui::ComboBox::from_id_salt("ui_lang").width(170.0).selected_text(current).show_ui(ui, |ui| {
                    for (code, name) in LANGS {
                        ui.selectable_value(&mut self.lang, code.to_string(), name);
                    }
                });
                ui.label(RichText::new(t(&self.lang, "Language")).color(WEAK));
            });
        });
        if let Some(d) = &self.dir {
            if !d.join("totk.exe").is_file() {
                ui.label(RichText::new(t(&self.lang, "totk.exe is not in this folder; totk.ini is written here anyway.")).small().color(WEAK));
            }
        }
        ui.add_space(4.0);
    }

    fn side_panel(&mut self, ui: &mut Ui) {
        let cat = self.cat.clone();
        ui.add_space(8.0);
        ui.label(RichText::new(t(&self.lang, "Profiles")).strong().color(GOLD));
        ui.add_space(2.0);
        if self.profiles.is_empty() {
            ui.label(RichText::new(t(&self.lang, "No profile: totk.ini is edited directly.")).small().color(WEAK));
        }
        let mut switch = None;
        for name in self.profiles.clone() {
            let active = self.active.as_deref() == Some(&name);
            let text = if active {
                RichText::new(format!("●  {name}")).color(GOLD).strong()
            } else {
                RichText::new(format!("○  {name}"))
            };
            let r = ui.add_sized([ui.available_width(), 28.0], egui::Button::selectable(active, text));
            if r.clicked() && !active {
                switch = Some(name.clone());
            }
            r.context_menu(|ui| {
                if ui.button(t(&self.lang, "Rename")).clicked() {
                    self.dialog = Dialog::Rename { from: name.clone(), name: name.clone() };
                }
                if ui.button(t(&self.lang, "Duplicate")).clicked() {
                    if let Some(store) = &self.store {
                        let to = profiles::free_name(store, &name);
                        let _ = store.duplicate(&name, &to);
                    }
                    self.refresh_profiles();
                }
                if ui.button(t(&self.lang, "Delete")).clicked() {
                    self.dialog = Dialog::Delete(name.clone());
                }
            });
        }
        if let Some(name) = switch {
            self.switch_to(ui, &name);
        }
        ui.add_space(4.0);
        ui.add_enabled_ui(self.dir.is_some(), |ui| {
            ui.horizontal_wrapped(|ui| {
                if ui.button(format!("+ {}", t(&self.lang, "New"))).clicked() {
                    let base = t(&self.lang, "Quality");
                    let name = self.store.as_ref().map(|s| profiles::free_name(s, base)).unwrap_or_default();
                    self.dialog = Dialog::New { name, preset: Preset::Current, switch: true };
                }
                if let Some(a) = self.active.clone() {
                    if ui.button(t(&self.lang, "Rename")).clicked() {
                        self.dialog = Dialog::Rename { from: a.clone(), name: a.clone() };
                    }
                    if ui.button(t(&self.lang, "Duplicate")).clicked() {
                        if let Some(store) = &self.store {
                            let to = profiles::free_name(store, &a);
                            let _ = store.duplicate(&a, &to);
                        }
                        self.refresh_profiles();
                    }
                    if ui.button(RichText::new(t(&self.lang, "Delete")).color(RED)).clicked() {
                        self.dialog = Dialog::Delete(a);
                    }
                }
            });
        });
        ui.add_space(10.0);
        ui.separator();
        ui.add_space(6.0);
        let mut tabs: Vec<(String, String)> =
            cat.tabs.iter().map(|tb| (tb.id.clone(), tr(&tb.label, &self.lang).unwrap_or(&tb.id).to_string())).collect();
        tabs.push((EXPERT.into(), t(&self.lang, "Expert").into()));
        tabs.push((OTHER.into(), t(&self.lang, "Other keys").into()));
        if cfg!(windows) {
            tabs.push((UPDATES.into(), t(&self.lang, "Updates").into()));
        }
        for (id, label) in tabs {
            let newer = id == UPDATES && self.update_available();
            let changed = if id == UPDATES {
                0
            } else if id == OTHER {
                self.work.iter().filter(|(k, _)| cat.find(k).is_none()).count()
            } else {
                cat.settings.iter().filter(|s| s.tab == id && self.work.get(&s.key).is_some()).count()
            };
            let selected = self.tab == id && self.search.is_empty();
            let r = ui.add_sized(
                [ui.available_width(), 32.0],
                egui::Button::selectable(selected, RichText::new(&label).size(16.0)).right_text(if newer {
                    RichText::new(t(&self.lang, "new")).color(GOLD).small()
                } else if changed > 0 {
                    RichText::new(format!("{changed}")).color(GOLD).small()
                } else {
                    RichText::new("")
                }),
            );
            if r.clicked() {
                self.tab = id;
                self.search.clear();
            }
        }
    }

    /// Which documents next to the game gave settings and descriptions (docs.rs), one line in the bottom bar; the
    /// paths and the added keys show on hover.
    fn sources_note(&self, ui: &mut Ui) {
        let s = &self.sources;
        let found: Vec<(&str, &PathBuf)> =
            [("settings.json", &s.json), ("totk.ini.example", &s.example), ("settings.md", &s.settings_md), ("totk.ini", &s.ini)]
                .into_iter()
                .filter_map(|(n, p)| Some((n, p.as_ref()?)))
                .collect();
        if found.is_empty() {
            return;
        }
        let names: Vec<&str> = found.iter().map(|(n, _)| *n).collect();
        let paths: Vec<String> = found.iter().map(|(_, p)| p.display().to_string()).collect();
        ui.label(RichText::new(format!("{}: {}", t(&self.lang, "Descriptions read from"), names.join(", "))).small().color(WEAK))
            .on_hover_text(paths.join("\n"));
        if !s.added.is_empty() {
            ui.label(RichText::new(format!("+{} {}", s.added.len(), t(&self.lang, "new settings from these files"))).small().color(GOLD))
                .on_hover_text(s.added.iter().map(|k| k.to_ascii_lowercase()).collect::<Vec<_>>().join("\n"));
        }
    }
    fn bottom_bar(&mut self, ui: &mut Ui) {
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            let now = ui.input(|i| i.time);
            if let Some((text, error, at)) = &self.status {
                if now - at < 6.0 {
                    ui.label(RichText::new(text).color(if *error { RED } else { GOLD }));
                }
            }
            if self.dirty() {
                let keys = self.changed_keys();
                ui.label(RichText::new(format!("● {}", t(&self.lang, "Unsaved changes"))).color(GOLD)).on_hover_text(keys.join("\n"));
            }
            self.sources_note(ui);
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let can = self.dir.is_some();
                let exe = self.dir.as_ref().is_some_and(|d| d.join("totk.exe").is_file());
                if ui
                    .add_enabled(can && exe, egui::Button::new(RichText::new(format!("▶  {}", t(&self.lang, "Save and launch"))).strong()).fill(theme::GREEN_BRIGHT))
                    .clicked()
                {
                    self.launch(ui);
                }
                let save = egui::Button::new(RichText::new(t(&self.lang, "Save")).strong().color(BG)).fill(GOLD);
                if ui.add_enabled(can && self.dirty(), save).clicked() {
                    self.save(ui);
                }
                if ui.add_enabled(self.dirty(), egui::Button::new(t(&self.lang, "Revert"))).clicked() {
                    self.work = self.disk.clone();
                }
                ui.label(RichText::new(format!("{} {}", self.work.len(), t(&self.lang, "settings"))).small().color(WEAK));
            });
        });
        ui.add_space(4.0);
    }

    fn central(&mut self, ui: &mut Ui) {
        let cat = self.cat.clone();
        if self.dir.is_none() {
            ui.add_space(40.0);
            ui.vertical_centered(|ui| {
                ui.label(RichText::new(t(&self.lang, "No game folder yet: choose the folder that holds totk.exe.")).size(18.0));
                ui.add_space(10.0);
                if ui.button(RichText::new(t(&self.lang, "Change…")).size(17.0)).clicked() {
                    if let Some(d) = rfd::FileDialog::new().pick_folder() {
                        self.open_dir(d);
                        crate::remember_dir(self.dir.as_ref().unwrap());
                    }
                }
            });
            return;
        }
        if self.changed_on_disk {
            egui::Frame::new().fill(Color32::from_rgb(70, 52, 20)).corner_radius(8).inner_margin(8).show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.label(t(&self.lang, "totk.ini changed on disk."));
                    if ui.button(t(&self.lang, "Reload")).clicked() {
                        self.reload();
                    }
                    if ui.button(t(&self.lang, "Keep mine")).clicked() {
                        self.changed_on_disk = false;
                    }
                });
            });
        }
        ui.horizontal(|ui| {
            ui.add(
                egui::TextEdit::singleline(&mut self.search)
                    .hint_text(format!("🔍  {}", t(&self.lang, "Search settings")))
                    .desired_width(360.0),
            );
            if !self.search.is_empty() && ui.small_button("✕").clicked() {
                self.search.clear();
            }
            ui.checkbox(&mut self.only_changed, t(&self.lang, "Only changed"));
        });
        ui.add_space(6.0);

        let searching = !self.search.trim().is_empty();
        egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            ui.set_max_width(ui.available_width() - 8.0);
            if !searching && self.tab == UPDATES {
                self.updates_page(ui);
                return;
            }
            if !searching && self.tab == OTHER {
                self.other_keys(ui);
                return;
            }
            if !searching && self.tab == EXPERT {
                warn_box(ui, t(&self.lang, "These keys are for testing and troubleshooting. Many only act together with another setting; some can crash the game or damage saves. Change one only when you know why."));
            }
            let needle = self.search.trim().to_string();
            let mut rows: Vec<&Setting> = cat
                .settings
                .iter()
                .filter(|s| if searching { self.matches(s, &needle) } else { s.tab == self.tab })
                .filter(|s| !self.only_changed || self.work.get(&s.key).is_some())
                .collect();
            let tab_index = |id: &str| cat.tabs.iter().position(|tb| tb.id == id).unwrap_or(99);
            // the in-game menu's rows in its order; the rest grouped by their docs heading or totk.ini section
            rows.sort_by_key(|s| (tab_index(&s.tab), !s.in_menu, if s.in_menu { None } else { s.doc_group.clone().or(s.section.clone()) }, s.order));
            if rows.is_empty() {
                ui.add_space(20.0);
                ui.label(RichText::new(t(&self.lang, "Nothing matches.")).color(WEAK));
            }
            let mut last_heading: Option<String> = None;
            for s in rows {
                let heading = self.heading_of(s, searching);
                if heading != last_heading {
                    if let Some(h) = &heading {
                        ui.add_space(10.0);
                        ui.label(RichText::new(h).size(17.0).strong().color(GOLD));
                        ui.add_space(2.0);
                    }
                    last_heading = heading;
                }
                self.row(ui, s);
            }
            ui.add_space(20.0);
        });
    }

    fn heading_of(&self, s: &Setting, searching: bool) -> Option<String> {
        let tab = || {
            if s.tab == EXPERT {
                t(&self.lang, "Expert").to_string()
            } else {
                self.cat.tabs.iter().find(|tb| tb.id == s.tab).and_then(|tb| tr(&tb.label, &self.lang)).unwrap_or(&s.tab).to_string()
            }
        };
        let inner = if s.kind == Kind::Bind {
            tr(&s.group, &self.lang).map(|g| format!("{} · {g}", t(&self.lang, "Bindings")))
        } else if s.in_menu {
            tr(&s.group, &self.lang).map(str::to_string)
        } else if s.tab == EXPERT {
            Some(s.doc_group.clone().or(s.section.clone()).unwrap_or_else(|| t(&self.lang, "Expert").to_string()))
        } else {
            Some(t(&self.lang, "Not in the in-game menu").to_string())
        };
        match (searching, inner) {
            (true, Some(i)) => Some(format!("{} · {i}", tab())),
            (true, None) => Some(tab()),
            (false, i) => i,
        }
    }

    /// One setting: its name, key and marks on the left, the control and the reset button on the right, the
    /// description below.
    fn row(&mut self, ui: &mut Ui, s: &Setting) {
        let set = self.value(s).map(str::to_string);
        let env = std::env::var_os(format!("TOTK_{}", s.key)).is_some();
        let fill = if ui.rect_contains_pointer(ui.max_rect()) { CARD } else { CARD };
        let resp = egui::Frame::new()
            .fill(fill)
            .corner_radius(10)
            .inner_margin(egui::Margin { left: 14, right: 12, top: 9, bottom: 9 })
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                ui.horizontal(|ui| {
                    let control_w = (ui.available_width() * 0.42).clamp(280.0, 420.0);
                    let left_w = ui.available_width() - control_w - 12.0;
                    ui.allocate_ui_with_layout(egui::vec2(left_w, 0.0), Layout::top_down(Align::Min), |ui| {
                        ui.horizontal_wrapped(|ui| {
                            if s.label.is_some() {
                                ui.label(RichText::new(self.label_of(s)).size(15.5).strong());
                            } else {
                                ui.label(RichText::new(self.label_of(s)).monospace().size(15.0).strong());
                            }
                            if set.is_some() {
                                theme::badge(ui, t(&self.lang, "changed"), BG, GOLD);
                            }
                            if env {
                                theme::badge(ui, t(&self.lang, "set by environment variable"), Color32::WHITE, RED)
                                    .on_hover_text(t(&self.lang, "the environment variable wins over totk.ini"));
                            }
                            if let Some(src) = &s.source {
                                theme::badge(ui, &format!("{} {src}", t(&self.lang, "from")), Color32::WHITE, Color32::from_rgb(60, 80, 120))
                                    .on_hover_text(t(&self.lang, "Read from this file at start; not in the configurator's own list yet."));
                            }
                            if let Some(safety) = &s.safety {
                                let color = match safety.as_str() {
                                    "Safe" => Color32::from_rgb(60, 110, 80),
                                    "Caution" => Color32::from_rgb(120, 96, 40),
                                    _ => Color32::from_rgb(130, 50, 40),
                                };
                                theme::badge(ui, safety, Color32::WHITE, color);
                            }
                        });
                        ui.horizontal_wrapped(|ui| {
                            if s.label.is_some() {
                                ui.label(RichText::new(s.key.to_ascii_lowercase()).monospace().color(WEAK));
                            }
                            let def = if s.kind == Kind::Bind {
                                self.default_binds(s)
                            } else if s.default.is_empty() {
                                t(&self.lang, "unset").to_string()
                            } else {
                                self.choice_label(s, &s.default)
                            };
                            ui.label(RichText::new(format!("{} {def}", t(&self.lang, "Default:"))).small().color(WEAK));
                            if let Some(d) = &s.deck {
                                ui.label(RichText::new(format!("· {} {}", t(&self.lang, "Steam Deck profile:"), self.choice_label(s, d))).small().color(WEAK));
                            }
                            // while the game runs: whether its own menu applies the value at once (cfg.c LIVE)
                            let when = if s.live { "applies at once in the game" } else { "after a restart of the game" };
                            ui.label(RichText::new(if s.live { "⚡" } else { "⟳" }).small().color(WEAK)).on_hover_text(t(&self.lang, when));
                            if let Some(v) = &set {
                                if !s.accepts(v) {
                                    theme::badge(ui, t(&self.lang, "not a value the game takes"), Color32::WHITE, RED)
                                        .on_hover_text(s.values.clone().unwrap_or_default());
                                }
                            }
                        });
                    });
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        let reset = ui.add_enabled(set.is_some(), egui::Button::new(RichText::new("↺").size(17.0)).min_size(egui::vec2(30.0, 28.0)));
                        if reset.on_hover_text(t(&self.lang, "Reset to default")).clicked() {
                            self.work.remove(&s.key);
                            if self.bind_edit.as_deref() == Some(&s.key) {
                                self.bind_edit = None;
                            }
                        }
                        ui.allocate_ui_with_layout(egui::vec2(control_w - 40.0, 30.0), Layout::right_to_left(Align::Center), |ui| {
                            self.control(ui, s);
                        });
                    });
                });
                let desc = tr(&s.desc, &self.lang).map(str::to_string).or_else(|| s.doc.clone());
                if let Some(d) = desc.filter(|d| !d.is_empty()) {
                    ui.add_space(2.0);
                    ui.label(RichText::new(d).small().color(Color32::from_rgb(190, 192, 178)));
                }
                if s.kind == Kind::Bind && self.bind_edit.as_deref() == Some(&s.key) {
                    ui.add_space(6.0);
                    self.bind_editor(ui, s);
                }
            });
        if set.is_some() {
            // a gold bar on the left edge of a changed row
            let r = resp.response.rect;
            ui.painter().rect_filled(egui::Rect::from_min_size(r.min, egui::vec2(4.0, r.height())), 2.0, GOLD);
        }
        let _ = CARD_HOVER;
        ui.add_space(6.0);
    }

    fn set(&mut self, key: &str, value: &str) {
        self.work.set(key, value);
    }

    fn control(&mut self, ui: &mut Ui, s: &Setting) {
        let key = s.key.clone();
        let cur = self.effective(s);
        // free-text values offered as a list (row_set_or_not): On writes the menu's value
        if let Some(on_value) = &s.set_or_not {
            let mut on = self.value(s).is_some();
            let mut text = self.value(s).unwrap_or("").to_string();
            if on {
                if ui.add(egui::TextEdit::singleline(&mut text).desired_width(150.0)).changed() {
                    self.set(&key, &text);
                }
            }
            if theme::toggle(ui, &mut on).changed() {
                if on {
                    let v = if on_value.is_empty() { "input-record.txt" } else { on_value };
                    self.set(&key, v);
                } else {
                    self.work.remove(&key);
                }
            }
            return;
        }
        match s.kind {
            Kind::Toggle => {
                let mut on = cur != "0" && !cur.is_empty();
                if theme::toggle(ui, &mut on).changed() {
                    self.set(&key, if on { "1" } else { "0" });
                }
                let word = if on { &self.cat.common["on"] } else { &self.cat.common["off"] };
                ui.label(RichText::new(tr(word, &self.lang).unwrap_or("")).color(WEAK));
            }
            Kind::Slider | Kind::Number => {
                let (lo, hi) = match (s.min, s.max) {
                    (Some(a), Some(b)) => (a, b),
                    _ => s.range().unwrap_or((0.0, 100.0)),
                };
                let step = s.step.unwrap_or_else(|| {
                    let ints = lo.fract() == 0.0 && hi.fract() == 0.0 && s.default.parse::<f64>().map_or(true, |d| d.fract() == 0.0);
                    if ints { 1.0 } else { 0.01 }
                });
                let mut x: f64 = cur.parse().unwrap_or(lo);
                let fmt = s.format.clone().unwrap_or_default();
                let decimals = fmt.split_once("%.").and_then(|(_, r)| r.chars().next()?.to_digit(10)).map(|d| d as usize);
                let suffix = fmt.rsplit_once('f').map(|(_, r)| r.replace("%%", "%")).unwrap_or_default();
                let changed = if s.kind == Kind::Slider && hi - lo <= 20000.0 {
                    ui.add(
                        // no step_by and clamping only of edits: showing a value (203 nits with a step of 5, a value
                        // outside the range) must not change it; a moved slider is rounded to the step below
                        egui::Slider::new(&mut x, lo..=hi)
                            .custom_formatter(move |v, _| match decimals {
                                Some(d) => format!("{v:.d$}{suffix}"),
                                None => pretty_number(v, step),
                            })
                            .clamping(egui::SliderClamping::Edits),
                    )
                    .changed()
                } else {
                    ui.add(egui::DragValue::new(&mut x).range(lo..=hi).clamp_existing_to_range(false).speed(step.max((hi - lo) / 500.0)).max_decimals(if step >= 1.0 { 0 } else { 3 })).changed()
                };
                if changed {
                    let x = (((x - lo) / step).round() * step + lo).clamp(lo, hi);
                    self.set(&key, &pretty_number(x, step));
                }
                if self.value(s).is_none() {
                    if let Some(fb) = &s.fallback {
                        ui.label(RichText::new(format!("{} {}", t(&self.lang, "uses"), fb.to_ascii_lowercase())).small().color(WEAK));
                    }
                }
            }
            Kind::Choice | Kind::Key if s.choices.as_ref().is_some_and(|c| !c.is_empty()) => {
                let choices = s.choices.clone().unwrap();
                // a free-form key (no fixed list in cfg.c) also takes a value typed by hand
                if s.values.is_none() {
                    let mut text = self.value(s).unwrap_or("").to_string();
                    if ui.add(egui::TextEdit::singleline(&mut text).hint_text(&s.default).desired_width(100.0)).changed() {
                        self.set(&key, text.trim());
                    }
                }
                let shown = self.choice_label(s, &cur);
                egui::ComboBox::from_id_salt(&key).width(210.0).selected_text(shown).height(400.0).show_ui(ui, |ui| {
                    for c in &choices {
                        let label = self.choice_label(s, &c.value);
                        if ui.selectable_label(cur == c.value, label).clicked() {
                            if c.value.is_empty() {
                                self.work.remove(&key);
                            } else {
                                self.set(&key, &c.value);
                            }
                        }
                    }
                });
            }
            Kind::Bind => {
                let summary = self.bind_summary(s);
                let editing = self.bind_edit.as_deref() == Some(&key);
                if ui.button(t(&self.lang, if editing { "Done" } else { "Edit" })).clicked() {
                    self.bind_edit = if editing { None } else { Some(key.clone()) };
                }
                ui.label(RichText::new(summary).color(if self.value(s).is_some() { GOLD } else { WEAK }));
            }
            Kind::Folder => {
                if ui.button(t(&self.lang, "Browse…")).clicked() {
                    if let Some(d) = rfd::FileDialog::new().pick_folder() {
                        self.set(&key, &d.display().to_string());
                    }
                }
                let mut text = self.value(s).unwrap_or("").to_string();
                if ui.add(egui::TextEdit::singleline(&mut text).hint_text(&s.default).desired_width(220.0)).changed() {
                    self.set(&key, &text);
                }
            }
            _ => {
                let mut text = self.value(s).unwrap_or("").to_string();
                let hint = if s.default.is_empty() { t(&self.lang, "unset").to_string() } else { s.default.clone() };
                if ui.add(egui::TextEdit::singleline(&mut text).hint_text(hint).desired_width(260.0)).changed() {
                    self.set(&key, &text);
                }
            }
        }
    }

    // ---- bindings (input_map.c input_map_bind: "key:<name>, pad:<name>, key:default, pad:default" or "none")
    fn bind_entries(&self, s: &Setting) -> Vec<String> {
        self.value(s).map(|v| v.split(',').map(|e| e.trim().to_string()).filter(|e| !e.is_empty()).collect()).unwrap_or_default()
    }

    fn entry_label(&self, e: &str) -> String {
        if let Some(k) = e.strip_prefix("key:") {
            if k == "default" {
                return format!("⌨ {}", t(&self.lang, "default"));
            }
            return format!("⌨ {}", self.key_label(k));
        }
        if let Some(p) = e.strip_prefix("pad:") {
            if p == "default" {
                return format!("🎮 {}", t(&self.lang, "default"));
            }
            return format!("🎮 {}", self.pad_label(p));
        }
        if e.eq_ignore_ascii_case("none") {
            return t(&self.lang, "No input").to_string();
        }
        e.to_string()
    }

    fn bind_summary(&self, s: &Setting) -> String {
        if self.value(s).is_none() {
            return self.default_binds(s);
        }
        self.bind_entries(s).iter().map(|e| self.entry_label(e)).collect::<Vec<_>>().join("   ")
    }

    /// The keys and the gamepad button (Nintendo layout) an action has by default (input_map.c DEFAULT_KEYS, DEFAULT_PADS).
    fn default_binds(&self, s: &Setting) -> String {
        let action = &s.key[5..];
        let list: Vec<String> = {
            let keys = self.cat.input.default_keys.get(action).cloned().unwrap_or_default();
            let pads = self.cat.input.default_pads.get("nintendo").and_then(|p| p.get(action)).cloned().unwrap_or_default();
            keys.iter().map(|k| format!("⌨ {}", self.key_label(k))).chain(pads.iter().map(|p| format!("🎮 {}", self.pad_label(p)))).collect()
        };
        list.join("   ")
    }

    fn bind_editor(&mut self, ui: &mut Ui, s: &Setting) {
        let key = s.key.clone();
        let mut entries = self.bind_entries(s);
        let before = entries.clone();
        egui::Frame::new().fill(Color32::from_rgb(16, 26, 21)).corner_radius(8).inner_margin(10).show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.horizontal_wrapped(|ui| {
                let mut remove = None;
                for (i, e) in entries.iter().enumerate() {
                    egui::Frame::new().fill(theme::GREEN).corner_radius(12).inner_margin(egui::Margin::symmetric(8, 3)).show(ui, |ui| {
                        ui.horizontal(|ui| {
                            ui.label(self.entry_label(e));
                            if ui.small_button("✕").clicked() {
                                remove = Some(i);
                            }
                        });
                    });
                }
                if let Some(i) = remove {
                    entries.remove(i);
                }
                if entries.is_empty() {
                    ui.label(RichText::new(format!("{}: {}", t(&self.lang, "Default bindings"), self.bind_summary(s))).color(WEAK));
                }
            });
            ui.add_space(4.0);
            ui.horizontal_wrapped(|ui| {
                egui::ComboBox::from_id_salt(format!("{key}_addkey")).selected_text(t(&self.lang, "Add key")).width(170.0).height(360.0).show_ui(ui, |ui| {
                    for k in &self.cat.input.keys {
                        if ui.selectable_label(false, self.key_label(k)).clicked() {
                            entries.push(format!("key:{k}"));
                        }
                    }
                });
                let capturing = self.capture.as_deref() == Some(&key);
                let cap_text = if capturing { t(&self.lang, "Press a key…") } else { t(&self.lang, "Capture") };
                let cap = ui.add(egui::Button::new(cap_text).selected(capturing));
                if cap.clicked() && !capturing {
                    self.capture = Some(key.clone());
                } else if capturing {
                    if let Some(name) = captured_key(ui) {
                        if name != "escape" {
                            entries.push(format!("key:{name}"));
                        }
                        self.capture = None;
                    }
                }
                if !s.keys_only {
                    egui::ComboBox::from_id_salt(format!("{key}_addpad")).selected_text(t(&self.lang, "Add button")).width(170.0).show_ui(ui, |ui| {
                        for p in &self.cat.input.pads {
                            if ui.selectable_label(false, self.pad_label(p)).clicked() {
                                entries.push(format!("pad:{p}"));
                            }
                        }
                    });
                }
            });
            ui.horizontal_wrapped(|ui| {
                let mut kd = entries.iter().any(|e| e == "key:default");
                if ui.checkbox(&mut kd, t(&self.lang, "keep the default keys")).changed() {
                    entries.retain(|e| e != "key:default");
                    if kd {
                        entries.insert(0, "key:default".into());
                    }
                }
                if !s.keys_only {
                    let mut pd = entries.iter().any(|e| e == "pad:default");
                    if ui.checkbox(&mut pd, t(&self.lang, "keep the default button")).changed() {
                        entries.retain(|e| e != "pad:default");
                        if pd {
                            entries.push("pad:default".into());
                        }
                    }
                }
                if ui.button(t(&self.lang, "No input")).clicked() {
                    entries = vec!["none".into()];
                }
            });
        });
        if entries != before {
            entries.dedup();
            if entries.len() > 1 {
                entries.retain(|e| !e.eq_ignore_ascii_case("none"));
            }
            self.set(&key, &entries.join(", "));
        }
    }

    fn other_keys(&mut self, ui: &mut Ui) {
        ui.label(RichText::new(t(&self.lang, "Keys in totk.ini that this version of the game does not know, and the cheat switches. They are kept as they are.")).color(WEAK));
        ui.add_space(8.0);
        let others: Vec<(String, String)> =
            self.work.iter().filter(|(k, _)| !self.cat.is_known(k) || k.starts_with("CHEAT_")).map(|(k, v)| (k.to_string(), v.to_string())).collect();
        let disk_keys: Vec<String> = self.disk.iter().map(|(k, _)| k.to_string()).collect();
        for (k, v) in others {
            egui::Frame::new().fill(CARD).corner_radius(10).inner_margin(10).show(ui, |ui| {
                ui.set_width(ui.available_width());
                ui.horizontal(|ui| {
                    ui.label(RichText::new(k.to_ascii_lowercase()).monospace().strong());
                    if k.starts_with("CHEAT_") {
                        theme::badge(ui, "cheat", BG, GOLD);
                    } else if disk_keys.contains(&k) {
                        theme::badge(ui, "?", Color32::WHITE, Color32::from_rgb(90, 90, 90));
                    }
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        if ui.button("✕").on_hover_text(t(&self.lang, "Delete")).clicked() {
                            self.work.remove(&k);
                        }
                        let mut text = v.clone();
                        if ui.add(egui::TextEdit::singleline(&mut text).desired_width(260.0)).changed() {
                            self.work.set(&k, &text);
                        }
                    });
                });
            });
            ui.add_space(6.0);
        }
        ui.add_space(8.0);
        ui.horizontal(|ui| {
            ui.add(egui::TextEdit::singleline(&mut self.other_key).hint_text(t(&self.lang, "key")).desired_width(220.0));
            ui.label("=");
            ui.add(egui::TextEdit::singleline(&mut self.other_value).hint_text(t(&self.lang, "value")).desired_width(220.0));
            let k = ini::norm_key(self.other_key.trim());
            let ok = !k.is_empty() && k.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_') && !self.other_value.trim().is_empty();
            if ui.add_enabled(ok, egui::Button::new(t(&self.lang, "Add"))).clicked() {
                self.work.set(&k, self.other_value.trim());
                self.other_key.clear();
                self.other_value.clear();
            }
        });
    }

    fn dialogs(&mut self, ui: &mut Ui) {
        let ctx = ui.ctx().clone();
        let lang = self.lang.clone();
        let mut close = false;
        let mut action: Option<Box<dyn FnOnce(&mut App, &Ui)>> = None;
        let update_text = if matches!(self.dialog, Dialog::Update) { self.update_confirm_text() } else { Vec::new() };
        let update_tag = self.latest.as_ref().map(|r| r.tag.clone()).unwrap_or_default();
        match &mut self.dialog {
            Dialog::None => return,
            Dialog::New { name, preset, switch } => {
                let taken = self.profiles.iter().any(|p| p.eq_ignore_ascii_case(name.trim()));
                egui::Modal::new(egui::Id::new("new_profile")).show(&ctx, |ui| {
                    ui.set_width(620.0);
                    ui.heading(t(&lang, "New"));
                    ui.add_space(6.0);
                    ui.label(t(&lang, "Name"));
                    ui.add(egui::TextEdit::singleline(name).desired_width(340.0));
                    ui.add_space(4.0);
                    ui.label(t(&lang, "Start from"));
                    ui.horizontal_wrapped(|ui| {
                        for p in presets::ALL {
                            ui.radio_value(preset, p, t(&lang, p.label()));
                        }
                    });
                    // what the chosen preset does, and why each of its values is there
                    egui::Frame::new().fill(CARD).corner_radius(8).inner_margin(10).show(ui, |ui| {
                        ui.set_width(ui.available_width());
                        ui.label(RichText::new(t(&lang, preset.description())).strong());
                        let changes = preset.changes();
                        if !changes.is_empty() {
                            ui.label(RichText::new(t(&lang, "On top of the current settings:")).small().color(WEAK));
                            egui::ScrollArea::vertical().max_height(300.0).show(ui, |ui| {
                                egui::Grid::new("preset_changes").num_columns(2).spacing([12.0, 4.0]).striped(true).show(ui, |ui| {
                                    for c in changes {
                                        let value = c.value.map(str::to_string).unwrap_or_else(|| format!("({})", t(&lang, "default")));
                                        ui.label(RichText::new(format!("{} = {value}", c.key.to_ascii_lowercase())).monospace().color(GOLD));
                                        ui.add(egui::Label::new(RichText::new(t(&lang, c.why)).small()).wrap());
                                        ui.end_row();
                                    }
                                });
                            });
                        }
                    });
                    ui.checkbox(switch, t(&lang, "Switch to it"));
                    let valid = profiles::valid_name(name) && !taken;
                    if !valid {
                        ui.label(RichText::new(t(&lang, "That name is taken or not allowed.")).small().color(RED));
                    }
                    ui.add_space(6.0);
                    ui.horizontal(|ui| {
                        if ui.add_enabled(valid, egui::Button::new(RichText::new(t(&lang, "Create")).color(BG)).fill(GOLD)).clicked() {
                            let (n, p, sw) = (name.clone(), *preset, *switch);
                            action = Some(Box::new(move |app: &mut App, ui: &Ui| {
                                let values = presets::values(p, &app.cat, &app.work);
                                let cat = app.cat.clone();
                                let r = app.store.as_ref().unwrap().create(&n, &values, &|k| cat.section_of(k));
                                app.refresh_profiles();
                                match r {
                                    Ok(()) if sw => app.switch_to(ui, &n),
                                    Ok(()) => {}
                                    Err(e) => app.say(ui, e.to_string(), true),
                                }
                            }));
                            close = true;
                        }
                        if ui.button(t(&lang, "Cancel")).clicked() {
                            close = true;
                        }
                    });
                });
            }
            Dialog::Rename { from, name } => {
                let taken = self.profiles.iter().any(|p| p.eq_ignore_ascii_case(name.trim()) && !p.eq_ignore_ascii_case(from));
                egui::Modal::new(egui::Id::new("rename_profile")).show(&ctx, |ui| {
                    ui.set_width(360.0);
                    ui.heading(t(&lang, "Rename"));
                    ui.add(egui::TextEdit::singleline(name).desired_width(320.0));
                    let valid = profiles::valid_name(name) && !taken;
                    if !valid {
                        ui.label(RichText::new(t(&lang, "That name is taken or not allowed.")).small().color(RED));
                    }
                    ui.horizontal(|ui| {
                        if ui.add_enabled(valid, egui::Button::new(t(&lang, "OK"))).clicked() {
                            let (f, n) = (from.clone(), name.clone());
                            action = Some(Box::new(move |app: &mut App, ui: &Ui| {
                                if let Err(e) = app.store.as_ref().unwrap().rename(&f, &n) {
                                    app.say(ui, e.to_string(), true);
                                }
                                app.refresh_profiles();
                            }));
                            close = true;
                        }
                        if ui.button(t(&lang, "Cancel")).clicked() {
                            close = true;
                        }
                    });
                });
            }
            Dialog::Update => {
                let text = &update_text;
                egui::Modal::new(egui::Id::new("update_confirm")).show(&ctx, |ui| {
                    ui.set_width(460.0);
                    ui.heading(format!("{} {}?", t(&lang, "Update to"), update_tag));
                    ui.add_space(4.0);
                    for (line, tone) in text {
                        tone.show(ui, line);
                    }
                    ui.add_space(8.0);
                    ui.horizontal(|ui| {
                        let go = RichText::new(format!("{} {}", t(&lang, "Update to"), update_tag)).strong().color(BG);
                        if ui.add(egui::Button::new(go).fill(GOLD)).clicked() {
                            action = Some(Box::new(|app: &mut App, ui: &Ui| app.start_update(ui)));
                            close = true;
                        }
                        if ui.button(t(&lang, "Not now")).clicked() {
                            close = true;
                        }
                    });
                });
            }
            Dialog::Delete(name) => {
                egui::Modal::new(egui::Id::new("delete_profile")).show(&ctx, |ui| {
                    ui.set_width(360.0);
                    ui.heading(format!("{} \"{name}\"", t(&lang, "Delete profile")));
                    ui.label(t(&lang, "The file is removed; totk.ini keeps its settings."));
                    ui.horizontal(|ui| {
                        if ui.add(egui::Button::new(RichText::new(t(&lang, "Delete")).color(Color32::WHITE)).fill(RED)).clicked() {
                            let n = name.clone();
                            action = Some(Box::new(move |app: &mut App, ui: &Ui| {
                                if let Err(e) = app.store.as_ref().unwrap().delete(&n) {
                                    app.say(ui, e.to_string(), true);
                                }
                                app.refresh_profiles();
                            }));
                            close = true;
                        }
                        if ui.button(t(&lang, "Cancel")).clicked() {
                            close = true;
                        }
                    });
                });
            }
        }
        if close {
            self.dialog = Dialog::None;
        }
        if let Some(a) = action {
            a(self, ui);
        }
    }

    // ---- updates (update.rs)
    fn update_available(&self) -> bool {
        match (&self.latest, self.installed.version()) {
            (Some(rel), Some(have)) => rel.version > have,
            (Some(_), None) => true,
            _ => false,
        }
    }

    fn update_busy(&self) -> bool {
        use crate::update::Phase::*;
        matches!(*self.upd.lock().unwrap(), Checking | Downloading { .. } | Verifying | Unpacking)
    }

    /// Takes the worker's results: a checked release, and a release ready to install once the player confirmed.
    fn poll_update(&mut self, ui: &Ui) {
        use crate::update::Phase;
        let phase = self.upd.lock().unwrap().clone();
        match phase {
            Phase::Checked(rel) => {
                self.latest = Some(rel);
                if std::mem::take(&mut self.show_update) {
                    self.dialog = Dialog::Update;
                }
                *self.upd.lock().unwrap() = Phase::Idle;
            }
            Phase::Ready(folder) if self.install_when_ready => {
                self.install_when_ready = false;
                self.launch_setup(ui, &folder);
            }
            Phase::Checking | Phase::Downloading { .. } | Phase::Verifying | Phase::Unpacking => {
                ui.ctx().request_repaint_after(std::time::Duration::from_millis(100));
            }
            _ => {}
        }
    }

    /// The confirmation: what stays, what the setup does, and (as a calm note) what is worth knowing about this folder.
    /// Red is kept for real errors (a failed download, a running game), not for an update that works as meant.
    fn update_confirm_text(&self) -> Vec<(String, Tone)> {
        let l = &self.lang;
        let mut out = Vec::new();
        let Some(rel) = &self.latest else { return out };
        out.push((t(l, "Your saves, game files and settings stay. The setup rebuilds totk.exe and the shaders (a few minutes) and starts the game when done.").to_string(), Tone::Body));
        if self.installed.custom {
            out.push((
                format!(
                    "{} ({}). {} {} {}",
                    t(l, "This folder runs a custom build"),
                    self.installed.run.as_deref().unwrap_or("?"),
                    t(l, "The update installs the official"),
                    rel.tag,
                    t(l, "build in its place.")
                ),
                Tone::Note,
            ));
        } else if !self.update_available() {
            out.push((t(l, "This folder already has this version; the setup installs it again.").to_string(), Tone::Note));
        }
        out.push((t(l, "The configurator saves your settings and closes while the setup runs.").to_string(), Tone::Small));
        out
    }
    /// After the confirmation: save the settings, then download, check and unpack on a thread; poll_update starts the
    /// setup when the release is ready.
    fn start_update(&mut self, ui: &Ui) {
        let Some(dir) = self.dir.clone() else { return };
        if crate::update::game_running(&dir) {
            self.say(ui, t(&self.lang, "Close the game first: totk.exe from this folder is running."), true);
            return;
        }
        if self.dirty() && !self.save(ui) {
            return;
        }
        let Some(rel) = self.latest.clone() else { return };
        self.install_when_ready = true;
        crate::update::fetch(rel, self.upd.clone());
    }

    fn launch_setup(&mut self, ui: &Ui, release: &Path) {
        let Some(dir) = self.dir.clone() else { return };
        if crate::update::game_running(&dir) {
            self.say(ui, t(&self.lang, "Close the game first: totk.exe from this folder is running."), true);
            return;
        }
        match crate::update::setup_command(release, &dir).spawn() {
            Ok(_) => ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close),
            Err(e) => {
                *self.upd.lock().unwrap() = crate::update::Phase::Failed(format!("{}: {e}", t(&self.lang, "Could not start the setup")));
            }
        }
    }

    fn updates_page(&mut self, ui: &mut Ui) {
        use crate::update::Phase;
        let l = self.lang.clone();
        let card = |ui: &mut Ui, add: &mut dyn FnMut(&mut Ui)| {
            egui::Frame::new().fill(CARD).corner_radius(10).inner_margin(14).show(ui, |ui| {
                ui.set_width(ui.available_width());
                add(ui);
            });
            ui.add_space(8.0);
        };
        // the folder
        let installed = self.installed.clone();
        card(ui, &mut |ui| {
            ui.label(RichText::new(t(&l, "Installed")).size(17.0).strong().color(GOLD));
            // the version that ran (logs/totk.log), else the setup's (totk-install.json); a custom build says so in words
            let version = installed.run.clone().or_else(|| installed.setup.clone()).unwrap_or_else(|| t(&l, "unknown").to_string());
            ui.horizontal(|ui| {
                ui.label(RichText::new(&version).size(16.0).strong());
                if installed.custom {
                    theme::badge(ui, t(&l, "custom build"), BG, NOTE);
                }
            })
            .response
            .on_hover_text(t(&l, "The version of the totk.exe that ran last, from logs/totk.log."));
            if let Some(setup) = &installed.setup {
                ui.label(RichText::new(format!("{} {setup}", t(&l, "Set up with release"))).small().color(WEAK))
                    .on_hover_text("totk-install.json");
            }
            if installed.custom {
                Tone::Note.show(ui, t(&l, "An update installs the official build of the release in place of this one."));
            }
        });
        // the latest release
        let phase = self.upd.lock().unwrap().clone();
        let busy = self.update_busy();
        let latest = self.latest.clone();
        let newer = self.update_available();
        let mut check = false;
        let mut install = false;
        let mut prefs_changed = false;
        let mut check_at_start = self.prefs.check_updates;
        card(ui, &mut |ui| {
            ui.horizontal(|ui| {
                ui.label(RichText::new(t(&l, "Latest release")).size(17.0).strong().color(GOLD));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    check = ui.add_enabled(!busy, egui::Button::new(t(&l, "Check now"))).clicked();
                });
            });
            match &latest {
                Some(rel) => {
                    ui.horizontal_wrapped(|ui| {
                        ui.label(RichText::new(&rel.name).size(16.0).strong());
                        if newer {
                            theme::badge(ui, t(&l, "new"), BG, GOLD);
                        } else {
                            theme::badge(ui, t(&l, "up to date"), Color32::WHITE, theme::GREEN);
                        }
                        ui.label(RichText::new(format!("{} · {:.0} MB", rel.published.get(..10).unwrap_or(""), rel.asset.size as f64 / 1e6)).color(WEAK));
                        if !rel.page.is_empty() {
                            ui.hyperlink_to(t(&l, "Release page"), &rel.page);
                        }
                    });
                    ui.add_space(4.0);
                    egui::ScrollArea::vertical().id_salt("notes").max_height(260.0).show(ui, |ui| {
                        ui.label(RichText::new(clean_notes(&rel.notes)).small());
                    });
                    ui.add_space(6.0);
                    let label = format!("⬇  {} {}", t(&l, "Update to"), rel.tag);
                    install = ui.add_enabled(!busy && self.dir.is_some(), egui::Button::new(RichText::new(label).strong().color(BG)).fill(GOLD)).clicked();
                }
                None => {
                    ui.label(RichText::new(t(&l, "Not checked yet.")).color(WEAK));
                }
            }
            match &phase {
                Phase::Checking => {
                    ui.horizontal(|ui| {
                        ui.spinner();
                        ui.label(t(&l, "Asking GitHub…"));
                    });
                }
                Phase::Downloading { done, total } => {
                    let f = if *total > 0 { *done as f32 / *total as f32 } else { 0.0 };
                    ui.add(egui::ProgressBar::new(f).text(format!("{} {:.0} / {:.0} MB", t(&l, "Downloading"), *done as f64 / 1e6, *total as f64 / 1e6)));
                }
                Phase::Verifying => {
                    ui.horizontal(|ui| {
                        ui.spinner();
                        ui.label(t(&l, "Checking the SHA-256…"));
                    });
                }
                Phase::Unpacking => {
                    ui.horizontal(|ui| {
                        ui.spinner();
                        ui.label(t(&l, "Unpacking…"));
                    });
                }
                Phase::Ready(folder) => {
                    ui.label(RichText::new(format!("{} {}", t(&l, "Ready:"), folder.display())).color(GOLD));
                }
                Phase::Failed(why) => {
                    ui.label(RichText::new(why).color(RED));
                }
                _ => {}
            }
            ui.add_space(6.0);
            if ui.checkbox(&mut check_at_start, t(&l, "Check for updates when the configurator opens")).changed() {
                prefs_changed = true;
            }
            ui.label(RichText::new(t(&l, "Saved in the configurator's own settings, not in totk.ini.")).small().color(WEAK));
        });
        if check {
            crate::update::check_async(self.upd.clone());
        }
        if prefs_changed {
            self.prefs.check_updates = check_at_start;
            if let Err(e) = self.prefs.save() {
                self.say(ui, format!("{}: {e}", t(&self.lang, "Could not save")), true);
            }
        }
        if install {
            if self.dir.as_deref().is_some_and(crate::update::game_running) {
                self.say(ui, t(&self.lang, "Close the game first: totk.exe from this folder is running."), true);
            } else {
                self.dialog = Dialog::Update;
            }
        }
    }

    /// totk.ini written by someone else (the game's menu): reload when nothing is edited here, else ask.
    fn watch_file(&mut self, ui: &Ui) {
        let now = ui.input(|i| i.time);
        if now - self.last_check < 1.0 {
            return;
        }
        self.last_check = now;
        let Some(path) = self.ini_path() else { return };
        let m = mtime(&path);
        if m != self.ini_mtime {
            if self.dirty() {
                self.changed_on_disk = true;
                self.ini_mtime = m;
            } else {
                self.reload();
                self.refresh_profiles();
            }
        }
    }

    fn take_screenshot(&mut self, ui: &Ui) {
        let Some(path) = self.screenshot.clone() else { return };
        if self.update_busy() {
            ui.ctx().request_repaint();
            return;   // the picture waits for the check's answer
        }
        self.frames += 1;
        // a window that opened (a modal) fades in: the picture waits a second after it appeared
        let now = ui.input(|i| i.time);
        let modal = !matches!(self.dialog, Dialog::None);
        let since = *self.shot_modal_at.get_or_insert(now);
        if !modal {
            self.shot_modal_at = None;
        }
        if self.frames >= 8 && (!modal || now - since > 1.0) && !self.shot_sent {
            self.shot_sent = true;
            ui.ctx().send_viewport_cmd(egui::ViewportCommand::Screenshot(Default::default()));
        }
        let image = ui.input(|i| {
            i.raw.events.iter().find_map(|e| match e {
                egui::Event::Screenshot { image, .. } => Some(image.clone()),
                _ => None,
            })
        });
        if let Some(image) = image {
            let _ = write_bmp(&path, &image);
            ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close);
        }
        ui.ctx().request_repaint();
    }
}

/// How a line of the update texts reads: the body, a calm note (amber with an info mark, for what is worth knowing
/// but expected), or small grey print.
#[derive(Clone, Copy)]
enum Tone {
    Body,
    Note,
    Small,
}

/// A muted amber for notes, between the text and the gold accent.
const NOTE: Color32 = Color32::from_rgb(226, 196, 128);

impl Tone {
    fn show(self, ui: &mut Ui, text: &str) {
        match self {
            Tone::Body => ui.label(text),
            Tone::Note => ui.label(RichText::new(format!("ℹ  {text}")).color(NOTE)),
            Tone::Small => ui.label(RichText::new(text).small().color(WEAK)),
        };
    }
}

/// Release notes as plain text: HTML tags (<details>, <b>), GitHub's alert marks ("> [!NOTE]") and quote marks go, the
/// words stay.
fn clean_notes(md: &str) -> String {
    let mut text = String::with_capacity(md.len());
    let mut in_tag = false;
    for c in md.chars() {
        match c {
            '<' => in_tag = true,
            '>' if in_tag => in_tag = false,
            _ if !in_tag => text.push(c),
            _ => {}
        }
    }
    text.lines()
        .map(|l| l.trim_start_matches('>').trim_start())
        .filter(|l| !(l.starts_with("[!") && l.ends_with(']')))
        .map(|l| l.replace("**", "").replace('`', ""))
        .collect::<Vec<_>>()
        .join("\n")
}

fn warn_box(ui: &mut Ui, text: &str) {
    egui::Frame::new().fill(Color32::from_rgb(64, 40, 24)).corner_radius(8).inner_margin(10).show(ui, |ui| {
        ui.set_width(ui.available_width());
        ui.label(RichText::new(format!("⚠  {text}")).color(Color32::from_rgb(250, 214, 160)));
    });
    ui.add_space(6.0);
}

/// The key or mouse button pressed this frame, as an input_map.c key name ("escape" to cancel).
fn captured_key(ui: &Ui) -> Option<String> {
    use egui::Key;
    ui.input(|i| {
        for e in &i.raw.events {
            match e {
                egui::Event::Key { key, pressed: true, .. } => {
                    let name = match key {
                        Key::Escape => "escape".to_string(),
                        Key::Space => "space".into(),
                        Key::Tab => "tab".into(),
                        Key::Enter => "enter".into(),
                        Key::Backspace => "backspace".into(),
                        Key::Insert => "insert".into(),
                        Key::Delete => "delete".into(),
                        Key::Home => "home".into(),
                        Key::End => "end".into(),
                        Key::PageUp => "pageup".into(),
                        Key::PageDown => "pagedown".into(),
                        Key::ArrowUp => "up".into(),
                        Key::ArrowDown => "down".into(),
                        Key::ArrowLeft => "left".into(),
                        Key::ArrowRight => "right".into(),
                        Key::Minus => "minus".into(),
                        Key::Equals => "equals".into(),
                        Key::OpenBracket => "lbracket".into(),
                        Key::CloseBracket => "rbracket".into(),
                        Key::Semicolon => "semicolon".into(),
                        Key::Quote => "apostrophe".into(),
                        Key::Comma => "comma".into(),
                        Key::Period => "period".into(),
                        Key::Slash => "slash".into(),
                        Key::Backslash => "backslash".into(),
                        Key::Backtick => "grave".into(),
                        Key::F2 => "f2".into(),
                        Key::F3 => "f3".into(),
                        Key::F4 => "f4".into(),
                        Key::F5 => "f5".into(),
                        Key::F6 => "f6".into(),
                        Key::F7 => "f7".into(),
                        Key::F9 => "f9".into(),
                        Key::F12 => "f12".into(),
                        k => {
                            let n = k.name();
                            if n.len() == 1 && n.chars().all(|c| c.is_ascii_alphanumeric()) {
                                n.to_ascii_lowercase()
                            } else {
                                continue;
                            }
                        }
                    };
                    return Some(name);
                }
                egui::Event::PointerButton { button, pressed: true, .. } => {
                    let name = match button {
                        egui::PointerButton::Secondary => "mouse_r",
                        egui::PointerButton::Middle => "mouse_m",
                        egui::PointerButton::Extra1 => "mouse_x1",
                        egui::PointerButton::Extra2 => "mouse_x2",
                        egui::PointerButton::Primary => continue,   // the click that started the capture
                    };
                    return Some(name.to_string());
                }
                _ => {}
            }
        }
        // the modifier keys arrive as state, not as key events
        let m = i.modifiers;
        if m.shift {
            Some("lshift".into())
        } else if m.ctrl {
            Some("lctrl".into())
        } else if m.alt {
            Some("lalt".into())
        } else {
            None
        }
    })
}

/// A 24-bit BMP of the image (no compression library needed); ffmpeg or any viewer converts it.
fn write_bmp(path: &Path, img: &egui::ColorImage) -> std::io::Result<()> {
    let (w, h) = (img.size[0], img.size[1]);
    let row = (w * 3 + 3) & !3;
    let size = 54 + row * h;
    let mut out = Vec::with_capacity(size);
    out.extend_from_slice(b"BM");
    out.extend_from_slice(&(size as u32).to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(&54u32.to_le_bytes());
    out.extend_from_slice(&40u32.to_le_bytes());
    out.extend_from_slice(&(w as i32).to_le_bytes());
    out.extend_from_slice(&(h as i32).to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&24u16.to_le_bytes());
    out.extend_from_slice(&[0u8; 24]);
    for y in (0..h).rev() {
        for x in 0..w {
            let p = img.pixels[y * w + x];
            out.extend_from_slice(&[p.b(), p.g(), p.r()]);
        }
        out.resize(out.len() + row - w * 3, 0);
    }
    std::fs::write(path, out)
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut Ui, _frame: &mut eframe::Frame) {
        self.watch_file(ui);
        if ui.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, egui::Key::S)) && self.dirty() {
            self.save(ui);
        }
        let bar = egui::Frame::new().fill(BG).inner_margin(egui::Margin::symmetric(16, 4));
        egui::Panel::top("top").frame(bar).show(ui, |ui| self.top_bar(ui));
        egui::Panel::bottom("bottom").frame(bar).show(ui, |ui| self.bottom_bar(ui));
        egui::Panel::left("side")
            .frame(egui::Frame::new().fill(BG).inner_margin(egui::Margin::symmetric(12, 6)))
            .resizable(false)
            .exact_size(230.0)
            .show(ui, |ui| {
                egui::ScrollArea::vertical().show(ui, |ui| self.side_panel(ui));
            });
        egui::CentralPanel::default_margins()
            .frame(egui::Frame::new().fill(theme::PANEL).inner_margin(egui::Margin { left: 18, right: 10, top: 12, bottom: 0 }))
            .show(ui, |ui| self.central(ui));
        self.dialogs(ui);
        self.poll_update(ui);
        if self.capture.is_some() {
            ui.ctx().request_repaint();
        }
        self.take_screenshot(ui);
    }
}
