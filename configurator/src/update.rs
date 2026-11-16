//! Updates from the project's GitHub releases: the latest release's Windows setup is downloaded, checked against the
//! size and SHA-256 GitHub lists for it, unpacked, and its setup window is started on the game folder, which updates the
//! install as running the new release's TotK-Setup.exe on it would (saves, game files and totk.ini stay; totk.exe and
//! the shaders are rebuilt).
//!
//! - Check: `GET https://api.github.com/repos/valentderah/totk-pc-recomp/releases/latest` (no drafts or pre-releases),
//!   its `TotK-Setup-<version>-windows.zip` asset with `size` and `digest` (`sha256:...`). The asset must come from this
//!   repository's download address.
//! - Download: into `%LOCALAPPDATA%\TotK Recomp\updates\` (where the in-game update of pull request #972 puts them too),
//!   as `<name>.part`, renamed once size and SHA-256 match; a mismatch deletes it.
//! - Unpack: Windows' `tar.exe` (bsdtar reads zip) after a listing that refuses absolute paths and `..`; the release
//!   folder `TotK-Setup` inside becomes `updates\TotK-Setup-<version>`, and its `VERSION` must name the release.
//! - Install: `<release>\python\pythonw.exe -m installer.gui --upgrade <game folder>` when the release's setup knows
//!   `--upgrade` (pull request #972: it waits for the game to exit), else `--rebuild <game folder>`, which every setup
//!   since 0.1 has: it starts the run on that folder at once and starts the game when it succeeds. Never while the
//!   folder's totk.exe runs.
//!
//! The version installed: `logs/totk.log` names the totk.exe that ran last ("version: TotK Recomp 0.4.1+6151ac4f");
//! `totk-install.json` names the release whose setup made the folder. When the two differ, or totk.exe is newer than
//! the setup's record, the folder holds a build of its own, which an update replaces: the window says so first.
//!
//! TOTK_CONFIG_UPDATE_API names another API address for tests, accepted only on http://127.0.0.1 or http://localhost.
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::time::Duration;

pub const REPO: &str = "valentderah/totk-pc-recomp";
const API: &str = "https://api.github.com";
const API_ENV: &str = "TOTK_CONFIG_UPDATE_API";

/// The release offered: its tag, numbers, notes and Windows setup asset.
#[derive(Clone, Debug, PartialEq)]
pub struct Release {
    pub tag: String,
    pub version: (u32, u32, u32),
    pub name: String,
    pub notes: String,
    pub page: String,
    pub published: String,
    pub asset: Asset,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Asset {
    pub name: String,
    pub size: u64,
    pub sha256: String,
    pub url: String,
}

#[derive(Deserialize)]
struct ApiRelease {
    tag_name: String,
    name: Option<String>,
    body: Option<String>,
    html_url: Option<String>,
    published_at: Option<String>,
    #[serde(default)]
    draft: bool,
    #[serde(default)]
    prerelease: bool,
    #[serde(default)]
    assets: Vec<ApiAsset>,
}

#[derive(Deserialize)]
struct ApiAsset {
    name: String,
    size: u64,
    digest: Option<String>,
    browser_download_url: String,
}

/// "0.4.1", "v0.4.1", "0.4.1+6151ac4f" as (0, 4, 1).
pub fn parse_version(s: &str) -> Option<(u32, u32, u32)> {
    let s = s.trim().trim_start_matches(['v', 'V']);
    let core = s.split(['+', '-', ' ']).next()?;
    let mut it = core.split('.');
    let v = (it.next()?.parse().ok()?, it.next()?.parse().ok()?, it.next().unwrap_or("0").parse().ok()?);
    it.next().is_none().then_some(v)
}

/// The API address: GitHub's, or for tests TOTK_CONFIG_UPDATE_API on this machine.
pub fn api_base() -> String {
    match std::env::var(API_ENV) {
        Ok(v) if v.starts_with("http://127.0.0.1:") || v.starts_with("http://localhost:") => {
            v.trim_end_matches('/').to_string()
        }
        _ => API.to_string(),
    }
}

/// Where a release asset may come from: this repository's download address, or the test API's host.
fn allowed_download(url: &str, base: &str) -> bool {
    url.starts_with(&format!("https://github.com/{REPO}/releases/download/"))
        || (base != API && url.starts_with(&format!("{base}/")))
}

/// The release of an API answer (releases/latest), with its Windows setup zip.
pub fn parse_release(json: &str, base: &str) -> Result<Release, String> {
    let r: ApiRelease = serde_json::from_str(json).map_err(|e| format!("unexpected answer from GitHub: {e}"))?;
    if r.draft || r.prerelease {
        return Err(format!("{} is a draft or a pre-release", r.tag_name));
    }
    let version = parse_version(&r.tag_name).ok_or_else(|| format!("no version number in the tag {}", r.tag_name))?;
    let a = r
        .assets
        .into_iter()
        .find(|a| a.name.starts_with("TotK-Setup-") && a.name.ends_with("-windows.zip"))
        .ok_or_else(|| format!("{} has no Windows setup (TotK-Setup-*-windows.zip)", r.tag_name))?;
    let sha256 = a
        .digest
        .as_deref()
        .and_then(|d| d.strip_prefix("sha256:"))
        .filter(|h| h.len() == 64 && h.bytes().all(|b| b.is_ascii_hexdigit()))
        .ok_or_else(|| format!("{} lists no SHA-256 for {}", r.tag_name, a.name))?
        .to_ascii_lowercase();
    if !allowed_download(&a.browser_download_url, base) {
        return Err(format!("{} would download from {}, not from this project", a.name, a.browser_download_url));
    }
    if a.name.contains(['/', '\\']) || a.name.contains("..") {
        return Err(format!("unexpected asset name {}", a.name));
    }
    Ok(Release {
        name: r.name.filter(|n| !n.is_empty()).unwrap_or_else(|| r.tag_name.clone()),
        tag: r.tag_name,
        version,
        notes: r.body.unwrap_or_default().replace("\r\n", "\n"),
        page: r.html_url.unwrap_or_default(),
        published: r.published_at.unwrap_or_default(),
        asset: Asset { name: a.name, size: a.size, sha256, url: a.browser_download_url },
    })
}

/// HTTPS through the system's TLS and certificate store (Schannel on Windows).
fn agent(read_timeout: Duration) -> ureq::Agent {
    let tls = ureq::tls::TlsConfig::builder()
        .provider(ureq::tls::TlsProvider::NativeTls)
        .root_certs(ureq::tls::RootCerts::PlatformVerifier)
        .build();
    ureq::Agent::config_builder()
        .tls_config(tls)
        .user_agent("totk-config")
        .timeout_connect(Some(Duration::from_secs(20)))
        .timeout_per_read(Some(read_timeout))
        .build()
        .into()
}

/// The latest release (one API call).
pub fn check() -> Result<Release, String> {
    let base = api_base();
    let url = format!("{base}/repos/{REPO}/releases/latest");
    let mut resp = agent(Duration::from_secs(30))
        .get(&url)
        .header("Accept", "application/vnd.github+json")
        .call()
        .map_err(|e| format!("could not reach GitHub: {e}"))?;
    let text = resp.body_mut().read_to_string().map_err(|e| format!("could not read GitHub's answer: {e}"))?;
    parse_release(&text, &base)
}

// ---- the version installed

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Installed {
    /// The totk.exe that ran last (logs/totk.log).
    pub run: Option<String>,
    /// The release whose setup made the folder (totk-install.json "release").
    pub setup: Option<String>,
    /// totk.exe is not the one the setup built: a build of the player's own, which an update replaces.
    pub custom: bool,
}

impl Installed {
    /// The version to compare with a release: the one that ran, else the setup's.
    pub fn version(&self) -> Option<(u32, u32, u32)> {
        self.run.as_deref().and_then(parse_version).or_else(|| self.setup.as_deref().and_then(parse_version))
    }
}

/// The version of a "version: TotK Recomp <v>, game ..." line (runtime/main.c).
pub fn version_in_log(text: &str) -> Option<String> {
    let line = text.lines().find(|l| l.contains("version: TotK Recomp "))?;
    let rest = &line[line.find("version: TotK Recomp ")? + "version: TotK Recomp ".len()..];
    Some(rest.split([',', ' ']).next()?.to_string()).filter(|v| !v.is_empty())
}

pub fn installed(dir: &Path) -> Installed {
    let run = std::fs::read_to_string(dir.join("logs").join("totk.log")).ok().and_then(|t| version_in_log(&t));
    let record = dir.join("totk-install.json");
    let setup = std::fs::read_to_string(&record)
        .ok()
        .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .and_then(|v| v.get("release")?.as_str().map(str::to_string));
    let mtime = |p: &Path| std::fs::metadata(p).and_then(|m| m.modified()).ok();
    // the setup writes totk-install.json after it links totk.exe; a totk.exe more than a minute newer came later
    let exe_newer = match (mtime(&dir.join("totk.exe")), mtime(&record)) {
        (Some(e), Some(r)) => e.duration_since(r).is_ok_and(|d| d > Duration::from_secs(60)),
        _ => false,
    };
    let differs = matches!((&run, &setup), (Some(r), Some(s)) if r != s);
    Installed { custom: setup.is_some() && (differs || exe_newer), run, setup }
}

/// Whether the folder's totk.exe runs: Windows refuses to open a running executable for writing.
pub fn game_running(dir: &Path) -> bool {
    let exe = dir.join("totk.exe");
    if !cfg!(windows) || !exe.is_file() {
        return false;
    }
    match std::fs::OpenOptions::new().write(true).open(&exe) {
        Ok(_) => false,
        Err(e) => e.kind() == std::io::ErrorKind::PermissionDenied || e.raw_os_error() == Some(32),
    }
}

// ---- download, check, unpack

/// What the worker thread is doing, for the window.
#[derive(Clone, Debug, Default, PartialEq)]
pub enum Phase {
    #[default]
    Idle,
    Checking,
    Checked(Release),
    Downloading { done: u64, total: u64 },
    Verifying,
    Unpacking,
    /// The release folder, ready to run its setup.
    Ready(PathBuf),
    Failed(String),
}

pub type Shared = Arc<Mutex<Phase>>;

/// `%LOCALAPPDATA%\TotK Recomp\updates` (TOTK_CONFIG_UPDATES_DIR for tests).
pub fn updates_dir() -> Option<PathBuf> {
    if let Some(d) = std::env::var_os("TOTK_CONFIG_UPDATES_DIR") {
        return Some(PathBuf::from(d));
    }
    std::env::var_os("LOCALAPPDATA").map(|d| PathBuf::from(d).join("TotK Recomp").join("updates"))
}

fn sha256_file(path: &Path) -> std::io::Result<String> {
    let mut f = std::fs::File::open(path)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(h.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

/// Size and SHA-256 of a downloaded file against the asset; an error names what differs.
pub fn verify(path: &Path, asset: &Asset) -> Result<(), String> {
    let size = std::fs::metadata(path).map_err(|e| e.to_string())?.len();
    if size != asset.size {
        return Err(format!("{}: {size} bytes, GitHub lists {}", asset.name, asset.size));
    }
    let got = sha256_file(path).map_err(|e| e.to_string())?;
    if got != asset.sha256 {
        return Err(format!("{}: SHA-256 {}, GitHub lists {}", asset.name, &got[..16], &asset.sha256[..16]));
    }
    Ok(())
}

/// Downloads the asset into dir (reusing a finished, matching download) and checks it. Progress goes to `phase`.
pub fn download(rel: &Release, dir: &Path, phase: &Shared) -> Result<PathBuf, String> {
    std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let path = dir.join(&rel.asset.name);
    if path.is_file() {
        *phase.lock().unwrap() = Phase::Verifying;
        if verify(&path, &rel.asset).is_ok() {
            return Ok(path);
        }
        let _ = std::fs::remove_file(&path);
    }
    let part = dir.join(format!("{}.part", rel.asset.name));
    let resp = agent(Duration::from_secs(60))
        .get(&rel.asset.url)
        .header("Accept", "application/octet-stream")
        .call()
        .map_err(|e| format!("download failed: {e}"))?;
    let mut reader = resp.into_body().into_reader();
    let mut out = std::fs::File::create(&part).map_err(|e| format!("{}: {e}", part.display()))?;
    let total = rel.asset.size;
    let (mut done, mut buf) = (0u64, vec![0u8; 1 << 18]);
    let result = (|| -> Result<(), String> {
        loop {
            let n = reader.read(&mut buf).map_err(|e| format!("download broke off after {done} bytes: {e}"))?;
            if n == 0 {
                break;
            }
            done += n as u64;
            if done > total {
                return Err(format!("{} is longer than the {total} bytes GitHub lists", rel.asset.name));
            }
            out.write_all(&buf[..n]).map_err(|e| format!("{}: {e}", part.display()))?;
            *phase.lock().unwrap() = Phase::Downloading { done, total };
        }
        out.flush().map_err(|e| e.to_string())?;
        drop(out);
        *phase.lock().unwrap() = Phase::Verifying;
        verify(&part, &rel.asset)?;
        std::fs::rename(&part, &path).map_err(|e| format!("{}: {e}", path.display()))
    })();
    if let Err(e) = result {
        let _ = std::fs::remove_file(&part);
        return Err(e);
    }
    Ok(path)
}

fn tar() -> PathBuf {
    // Windows' own bsdtar (Windows 10 1803 and later), by full path so a tar.exe elsewhere on PATH never runs
    let sys = std::env::var_os("SystemRoot").map(|r| PathBuf::from(r).join("System32").join("tar.exe"));
    sys.filter(|p| p.is_file()).unwrap_or_else(|| PathBuf::from("tar"))
}

/// An archive entry that stays inside the folder it is unpacked into.
pub fn safe_entry(name: &str) -> bool {
    let n = name.replace('\\', "/");
    !n.is_empty()
        && !n.starts_with('/')
        && !n.as_bytes().get(1).is_some_and(|b| *b == b':')
        && !n.split('/').any(|part| part == "..")
}

/// Unpacks the release zip into dir/TotK-Setup-<version> and checks it is that release's setup. Returns the folder.
pub fn unpack(zip: &Path, rel: &Release, dir: &Path) -> Result<PathBuf, String> {
    let (a, b, c) = rel.version;
    let target = dir.join(format!("TotK-Setup-{a}.{b}.{c}"));
    if release_ok(&target, rel).is_ok() {
        return Ok(target);
    }
    let list = Command::new(tar()).arg("-tf").arg(zip).output().map_err(|e| format!("tar: {e}"))?;
    if !list.status.success() {
        return Err(format!("{} is not a readable zip", zip.display()));
    }
    let names = String::from_utf8_lossy(&list.stdout);
    if let Some(bad) = names.lines().find(|n| !safe_entry(n)) {
        return Err(format!("{} holds an entry outside its folder: {bad}", rel.asset.name));
    }
    let tmp = dir.join(format!("TotK-Setup-{a}.{b}.{c}.unpack"));
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
    let st = Command::new(tar()).arg("-xf").arg(zip).arg("-C").arg(&tmp).status().map_err(|e| format!("tar: {e}"))?;
    if !st.success() {
        let _ = std::fs::remove_dir_all(&tmp);
        return Err(format!("could not unpack {}", rel.asset.name));
    }
    // the zip holds one folder (tools/make_release.py: TotK-Setup/...)
    let inner = std::fs::read_dir(&tmp)
        .map_err(|e| e.to_string())?
        .flatten()
        .map(|e| e.path())
        .find(|p| p.join("VERSION").is_file())
        .unwrap_or_else(|| tmp.clone());
    let _ = std::fs::remove_dir_all(&target);
    std::fs::rename(&inner, &target).map_err(|e| format!("{}: {e}", target.display()))?;
    let _ = std::fs::remove_dir_all(&tmp);
    if let Err(e) = release_ok(&target, rel) {
        let _ = std::fs::remove_dir_all(&target);
        return Err(e);
    }
    let _ = std::fs::remove_file(zip);   // the folder is what the setup runs from
    Ok(target)
}

/// The unpacked release has its setup and names the release in VERSION.
pub fn release_ok(dir: &Path, rel: &Release) -> Result<(), String> {
    let version = std::fs::read_to_string(dir.join("VERSION")).map_err(|_| format!("{} has no VERSION", dir.display()))?;
    if parse_version(&version) != Some(rel.version) {
        return Err(format!("the unpacked release says {}, not {}", version.trim(), rel.tag));
    }
    for need in ["python/pythonw.exe", "installer/gui.py"] {
        if !dir.join(need).is_file() {
            return Err(format!("the unpacked release lacks {need}"));
        }
    }
    Ok(())
}

/// Download, check and unpack on a thread; the phase ends Ready(folder) or Failed(why).
pub fn fetch(rel: Release, phase: Shared) {
    std::thread::spawn(move || {
        let result = (|| {
            let dir = updates_dir().ok_or("no LOCALAPPDATA folder")?;
            *phase.lock().unwrap() = Phase::Downloading { done: 0, total: rel.asset.size };
            let zip = download(&rel, &dir, &phase)?;
            *phase.lock().unwrap() = Phase::Unpacking;
            unpack(&zip, &rel, &dir)
        })();
        *phase.lock().unwrap() = match result {
            Ok(folder) => Phase::Ready(folder),
            Err(e) => Phase::Failed(e.to_string()),
        };
    });
}

/// The check on a thread; the phase ends Checked(release) or Failed(why).
pub fn check_async(phase: Shared) {
    *phase.lock().unwrap() = Phase::Checking;
    std::thread::spawn(move || {
        let r = check();
        *phase.lock().unwrap() = match r {
            Ok(rel) => Phase::Checked(rel),
            Err(e) => Phase::Failed(e),
        };
    });
}

/// The command that runs the release's setup on the game folder: --upgrade when the setup has it, else --rebuild.
pub fn setup_command(release: &Path, game: &Path) -> Command {
    let gui = std::fs::read_to_string(release.join("installer").join("gui.py")).unwrap_or_default();
    let mode = if gui.contains("'--upgrade'") { "--upgrade" } else { "--rebuild" };
    let mut cmd = Command::new(release.join("python").join("pythonw.exe"));
    cmd.args(["-m", "installer.gui", "--root"]).arg(release).arg(mode).arg(game).current_dir(release);
    cmd
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    fn api_json(base: &str, size: u64, sha: &str) -> String {
        serde_json::json!({
            "tag_name": "v9.8.7", "name": "v9.8.7", "body": "Notes\r\nline 2", "html_url": "https://example/r",
            "published_at": "2026-11-16T00:00:00Z", "draft": false, "prerelease": false,
            "assets": [
                {"name": "TotK-Setup-9.8.7-x86_64.AppImage", "size": 1, "digest": "sha256:00", "browser_download_url": format!("{base}/x")},
                {"name": "TotK-Setup-9.8.7-windows.zip", "size": size, "digest": format!("sha256:{sha}"),
                 "browser_download_url": format!("{base}/download/TotK-Setup-9.8.7-windows.zip")}
            ]
        })
        .to_string()
    }

    #[test]
    fn versions() {
        assert_eq!(parse_version("v0.4.1"), Some((0, 4, 1)));
        assert_eq!(parse_version("0.4.1+6151ac4f"), Some((0, 4, 1)));
        assert_eq!(parse_version("0.4"), Some((0, 4, 0)));
        assert_eq!(parse_version("nightly"), None);
        assert!(parse_version("0.4.10").unwrap() > parse_version("0.4.9").unwrap());
        assert_eq!(version_in_log("x\n[host] version: TotK Recomp 0.4.1+6151ac4f, game 1.4.3\n").as_deref(), Some("0.4.1+6151ac4f"));
        assert_eq!(version_in_log("[host] version: TotK Recomp 0.4.0, game 1.4.3").as_deref(), Some("0.4.0"));
    }

    #[test]
    fn release_answer() {
        let sha = "ab".repeat(32);
        let gh = "https://github.com/valentderah/totk-pc-recomp/releases";
        let r = parse_release(&api_json(gh, 10, &sha), API).unwrap();
        assert_eq!((r.version, r.asset.name.as_str(), r.asset.size), ((9, 8, 7), "TotK-Setup-9.8.7-windows.zip", 10));
        assert_eq!(r.notes, "Notes\nline 2");
        // another host, a missing digest, a pre-release: refused
        assert!(parse_release(&api_json("https://evil.example", 10, &sha), API).is_err());
        assert!(parse_release(&api_json(gh, 10, "zz"), API).is_err());
        let pre = api_json(gh, 10, &sha).replace("\"prerelease\":false", "\"prerelease\":true");
        assert!(parse_release(&pre, API).is_err());
        // a test API on this machine may serve its own downloads
        assert!(parse_release(&api_json("http://127.0.0.1:9", 10, &sha), "http://127.0.0.1:9").is_ok());
    }

    #[test]
    fn entries() {
        assert!(safe_entry("TotK-Setup/python/pythonw.exe"));
        for bad in ["/etc/x", "C:/x", "TotK-Setup/../../x", "..\\x", ""] {
            assert!(!safe_entry(bad), "{bad}");
        }
    }

    #[test]
    fn installed_versions() {
        let d = std::env::temp_dir().join(format!("totk-config-inst-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("logs")).unwrap();
        std::fs::write(d.join("totk.exe"), b"").unwrap();
        std::fs::write(d.join("totk-install.json"), r#"{"format": 1, "release": "0.4.1+6151ac4"}"#).unwrap();
        std::fs::write(d.join("logs/totk.log"), "[host] version: TotK Recomp 0.4.1+6151ac4, game 1.4.3\n").unwrap();
        let i = installed(&d);
        assert_eq!((i.version(), i.custom), (Some((0, 4, 1)), false));
        std::fs::write(d.join("logs/totk.log"), "[host] version: TotK Recomp 0.4.0, game 1.4.3\n").unwrap();
        let i = installed(&d);
        assert_eq!((i.version(), i.custom), (Some((0, 4, 0)), true), "a build of its own");
        let _ = std::fs::remove_dir_all(&d);
    }

    /// A small release zip made with Windows' tar: TotK-Setup/{VERSION, python/pythonw.exe, installer/gui.py}.
    #[cfg(windows)]
    fn fake_zip(dir: &Path, version: &str, gui: &str) -> Vec<u8> {
        let root = dir.join("src").join("TotK-Setup");
        std::fs::create_dir_all(root.join("python")).unwrap();
        std::fs::create_dir_all(root.join("installer")).unwrap();
        std::fs::write(root.join("VERSION"), version).unwrap();
        std::fs::write(root.join("python/pythonw.exe"), b"MZ").unwrap();
        std::fs::write(root.join("installer/gui.py"), gui).unwrap();
        let zip = dir.join("fake.zip");
        let st = Command::new(tar()).arg("-a").arg("-cf").arg(&zip).arg("-C").arg(dir.join("src")).arg("TotK-Setup").status().unwrap();
        assert!(st.success());
        std::fs::read(&zip).unwrap()
    }

    fn sha_hex(bytes: &[u8]) -> String {
        Sha256::digest(bytes).iter().map(|b| format!("{b:02x}")).collect()
    }

    /// The whole flow against a fake GitHub on this machine: check, download, SHA-256, unpack, setup command; then a
    /// download whose bytes do not match is refused and deleted.
    #[cfg(windows)]
    #[test]
    fn check_download_unpack_with_a_fake_github() {
        let work = std::env::temp_dir().join(format!("totk-config-upd-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&work);
        std::fs::create_dir_all(&work).unwrap();
        let zip = fake_zip(&work, "9.8.7+abc1234\n", "ap.add_argument('--upgrade', type=Path)\n");
        let sha = sha_hex(&zip);
        // the API answer names the download address, so the server's port is known before the answer is made
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let base = format!("http://127.0.0.1:{}", listener.local_addr().unwrap().port());
        let json = api_json(&base, zip.len() as u64, &sha);
        let routes = vec![
            (format!("/repos/{REPO}/releases/latest"), json.into_bytes()),
            ("/download/TotK-Setup-9.8.7-windows.zip".to_string(), zip.clone()),
        ];
        serve_on(listener, routes.clone(), 2);
        std::env::set_var(API_ENV, &base);
        let rel = check().unwrap();
        assert_eq!(rel.version, (9, 8, 7));
        let phase: Shared = Default::default();
        let dl = work.join("updates");
        let path = download(&rel, &dl, &phase).unwrap();
        assert_eq!(std::fs::read(&path).unwrap(), zip);
        let folder = unpack(&path, &rel, &dl).unwrap();
        assert_eq!(folder, dl.join("TotK-Setup-9.8.7"));
        assert!(!path.exists(), "the zip goes once unpacked");
        let cmd = setup_command(&folder, Path::new("C:/Games/TotK"));
        let args: Vec<String> = cmd.get_args().map(|a| a.to_string_lossy().into_owned()).collect();
        assert_eq!(args[..3], ["-m", "installer.gui", "--root"]);
        assert_eq!(args[4..], ["--upgrade", "C:/Games/TotK"]);
        assert!(cmd.get_program().to_string_lossy().ends_with("pythonw.exe"));

        // a download whose SHA-256 differs from the listed one: refused, nothing left behind
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let base2 = serve_on(l, routes, 1);
        let mut bad = rel.clone();
        bad.asset.sha256 = "0".repeat(64);
        bad.asset.url = format!("{base2}/download/TotK-Setup-9.8.7-windows.zip");
        let dl2 = work.join("updates2");
        let err = download(&bad, &dl2, &phase).unwrap_err();
        assert!(err.contains("SHA-256"), "{err}");
        assert_eq!(std::fs::read_dir(&dl2).unwrap().count(), 0, "the bad download is deleted");
        std::env::remove_var(API_ENV);
        let _ = std::fs::remove_dir_all(&work);
    }

    /// A release whose VERSION names another version, or that lacks the setup, is refused.
    #[cfg(windows)]
    #[test]
    fn unpacked_release_must_match() {
        let work = std::env::temp_dir().join(format!("totk-config-ver-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&work);
        std::fs::create_dir_all(&work).unwrap();
        let bytes = fake_zip(&work, "1.0.0\n", "ap.add_argument('--rebuild')\n");
        let zip = work.join("TotK-Setup-9.8.7-windows.zip");
        std::fs::write(&zip, &bytes).unwrap();
        let rel = parse_release(&api_json("http://127.0.0.1:9", bytes.len() as u64, &sha_hex(&bytes)), "http://127.0.0.1:9").unwrap();
        let err = unpack(&zip, &rel, &work.join("u")).unwrap_err();
        assert!(err.contains("1.0.0"), "{err}");
        assert!(!work.join("u").join("TotK-Setup-9.8.7").exists());
        // an older setup without --upgrade gets --rebuild
        let rel_dir = work.join("src").join("TotK-Setup");
        let args: Vec<String> = setup_command(&rel_dir, Path::new("D:/T")).get_args().map(|a| a.to_string_lossy().into_owned()).collect();
        assert_eq!(args[4], "--rebuild");
        let _ = std::fs::remove_dir_all(&work);
    }

    /// An HTTP server on this machine for `requests` requests: routes[path] with status 200, else 404. Returns its address.
    fn serve_on(l: TcpListener, routes: Vec<(String, Vec<u8>)>, requests: usize) -> String {
        let base = format!("http://127.0.0.1:{}", l.local_addr().unwrap().port());
        std::thread::spawn(move || {
            for _ in 0..requests {
                let Ok((mut s, _)) = l.accept() else { return };
                let mut req = Vec::new();
                let mut b = [0u8; 1024];
                while !req.windows(4).any(|w| w == b"\r\n\r\n") {
                    let n = s.read(&mut b).unwrap_or(0);
                    if n == 0 {
                        break;
                    }
                    req.extend_from_slice(&b[..n]);
                }
                let line = String::from_utf8_lossy(&req).lines().next().unwrap_or("").to_string();
                let path = line.split(' ').nth(1).unwrap_or("").to_string();
                match routes.iter().find(|(p, _)| *p == path) {
                    Some((_, body)) => {
                        let _ = write!(s, "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len());
                        let _ = s.write_all(body);
                    }
                    None => {
                        let _ = s.write_all(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    }
                }
            }
        });
        base
    }
}
