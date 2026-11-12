// Shared by the build scripts of bsp-client and bsp-server (`include!`): sets
// BPS_GIT_DESCRIBE to `git describe --tags --always --dirty`, or "unknown"
// when git cannot say (no git, or no repository). $BPS_GIT_DESCRIBE overrides
// it, for builds that cannot see .git.

fn emit_git_describe() {
    println!("cargo:rerun-if-env-changed=BPS_GIT_DESCRIBE");
    let root = std::path::Path::new(&std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    // Rebuild when the commit changes, or when a source file does (it may
    // make the tree dirty, or clean again).
    for p in [".git/HEAD", ".git/index", "crates", "Cargo.lock"] {
        println!("cargo:rerun-if-changed={}", root.join(p).display());
    }
    // A branch's own ref moves with each commit.
    if let Ok(head) = std::fs::read_to_string(root.join(".git/HEAD"))
        && let Some(r) = head.strip_prefix("ref: ")
    {
        println!(
            "cargo:rerun-if-changed={}",
            root.join(".git").join(r.trim()).display()
        );
    }
    let described = std::env::var("BPS_GIT_DESCRIBE")
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .or_else(|| {
            let out = std::process::Command::new("git")
                .args(["describe", "--tags", "--always", "--dirty"])
                .current_dir(&root)
                .output()
                .ok()?;
            let s = String::from_utf8(out.stdout).ok()?.trim().to_string();
            (out.status.success() && !s.is_empty()).then_some(s)
        })
        .unwrap_or_else(|| "unknown".into());
    println!("cargo:rustc-env=BPS_GIT_DESCRIBE={described}");
}
