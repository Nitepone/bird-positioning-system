//! Compiles the firmware's portable C core (the parts with Rust originals)
//! plus a small shim with plain-argument entry points for the tests.

use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let core = manifest.join("../../firmware/lib/bps_core/src");
    for f in [
        "timesync.c",
        "gate.c",
        "timesync.h",
        "gate.h",
        "bps_common.h",
    ] {
        println!("cargo:rerun-if-changed={}", core.join(f).display());
    }
    println!("cargo:rerun-if-changed=csrc/shim.c");
    cc::Build::new()
        .include(&core)
        .file(core.join("timesync.c"))
        .file(core.join("gate.c"))
        .file("csrc/shim.c")
        // Same cap as the Rust estimator, so long streams behave identically.
        .define("BPS_EST_MAX_SAMPLES", "4096")
        // Rust never fuses multiply-adds; neither may C, for identical results.
        .flag_if_supported("-ffp-contract=off")
        .compile("bps_core_check");
}
