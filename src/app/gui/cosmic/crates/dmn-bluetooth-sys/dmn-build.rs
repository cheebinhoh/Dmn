// Copyright © 2026 Chee Bin HOH. All rights reserved.
//
// Cargo is the build entry point: this script configures and builds the C++
// core with CMake, then tells Cargo how to link the resulting C ABI library.
// CMake never invokes Cargo.

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

const LIBRARY_FILE: &str = "libdmn-bluetooth-cabi.so";

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let source_dir = env::var_os("DMN_SOURCE_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            manifest_dir
                .ancestors()
                .nth(6)
                .expect("crate is not inside the Dmn source tree")
                .to_path_buf()
        });
    assert!(
        source_dir.join("CMakeLists.txt").is_file(),
        "Dmn source tree not found at {}; set DMN_SOURCE_DIR",
        source_dir.display()
    );

    for path in [
        "CMakeLists.txt",
        "CMakeMacro.cmake",
        "src",
        "include",
        "VERSION.txt",
    ] {
        println!("cargo:rerun-if-changed={}", source_dir.join(path).display());
    }
    println!("cargo:rerun-if-env-changed=DMN_SOURCE_DIR");
    println!("cargo:rerun-if-env-changed=DMN_CMAKE_ARGS");
    println!("cargo:rerun-if-env-changed=DMN_CMAKE_GENERATOR");

    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let build_dir = out_dir.join("cmake");
    let build_type = if env::var("PROFILE").as_deref() == Ok("release") {
        "Release"
    } else {
        "Debug"
    };

    let mut configure = Command::new("cmake");
    configure
        .arg("-S")
        .arg(&source_dir)
        .arg("-B")
        .arg(&build_dir)
        .arg(format!("-DCMAKE_BUILD_TYPE={build_type}"))
        .arg("-DENABLE_APP_BLUETOOTH_COSMIC=ON")
        .arg("-DENABLE_DMN_DMESGNET=OFF")
        .arg("-DDMN_BUILD_TESTS=OFF");
    if let Ok(generator) = env::var("DMN_CMAKE_GENERATOR") {
        configure.arg("-G").arg(generator);
    }
    if let Ok(extra) = env::var("DMN_CMAKE_ARGS") {
        configure.args(extra.split_whitespace());
    }
    run(configure, "configure the Dmn C++ core");

    let mut build = Command::new("cmake");
    build
        .arg("--build")
        .arg(&build_dir)
        .arg("--target")
        .arg("dmn-bluetooth-cabi")
        .arg("--parallel");
    run(build, "build the Dmn C ABI library");

    let library_dir = find_library_dir(&build_dir).unwrap_or_else(|| {
        panic!(
            "{LIBRARY_FILE} was not produced under {}",
            build_dir.display()
        )
    });

    println!("cargo:rustc-link-search=native={}", library_dir.display());
    println!("cargo:rustc-link-lib=dylib=dmn-bluetooth-cabi");
    // Consumed by dependents (the application) to set the executable's RPATH.
    println!("cargo:dir={}", library_dir.display());
    // Lets this crate's own test binaries find the library at run time.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", library_dir.display());
}

fn run(mut command: Command, what: &str) {
    let status = command.status().unwrap_or_else(|error| {
        panic!(
            "failed to {what}: could not run `{}` ({error}); install CMake and a C++23 toolchain",
            command.get_program().to_string_lossy()
        )
    });
    assert!(status.success(), "failed to {what}: {status}");
}

fn find_library_dir(root: &Path) -> Option<PathBuf> {
    for entry in std::fs::read_dir(root).ok()?.flatten() {
        let path = entry.path();
        if path.is_dir() {
            if let Some(found) = find_library_dir(&path) {
                return Some(found);
            }
        } else if path.file_name().is_some_and(|name| name == LIBRARY_FILE) {
            return path.parent().map(Path::to_path_buf);
        }
    }
    None
}
