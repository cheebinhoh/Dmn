// Copyright © 2026 Chee Bin HOH. All rights reserved.

// The C++ core is a shared library built by the sys crate's build script; the
// final binary needs its directory in RPATH to start without LD_LIBRARY_PATH.
fn main() {
    if let Ok(dir) = std::env::var("DEP_DMN_BLUETOOTH_CABI_DIR") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
        println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN");
    }
}
