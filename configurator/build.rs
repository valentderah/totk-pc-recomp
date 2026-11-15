//! Embeds the icon (totk-config.rc, totk-config.ico) into totk-config.exe on Windows. With the GNU toolchain this runs
//! windres from MinGW-w64, the same PATH entry the windows crates need for dlltool.
fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        println!("cargo:rerun-if-changed=totk-config.ico");
        embed_resource::compile("totk-config.rc", embed_resource::NONE).manifest_optional().unwrap();
    }
}