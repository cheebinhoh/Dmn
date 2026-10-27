// Copyright © 2026 Chee Bin HOH. All rights reserved.

//! Entry point for `dmn-bluetooth-cosmic`. Parses flags, creates the C++
//! engine through `dmn-bluetooth-sys` (real BlueZ, or the synthetic backend
//! with `--demo`), runs the libcosmic application, and shuts the engine down
//! when the window closes.

#[path = "dmn-app.rs"]
mod app;

use std::process::ExitCode;
use std::sync::Arc;

const USAGE: &str = "Usage: dmn-bluetooth-cosmic [--demo] [--version] [--help]

  --demo     Use synthetic phones and notifications instead of BlueZ.
  --version  Print the application and engine versions.
  --help     Show this help.";

fn main() -> ExitCode {
    let mut demo = false;
    for argument in std::env::args().skip(1) {
        match argument.as_str() {
            "--demo" => demo = true,
            "--version" => {
                println!(
                    "dmn-bluetooth-cosmic {} (engine protocol {})",
                    env!("CARGO_PKG_VERSION"),
                    dmn_bluetooth_sys::version()
                );
                return ExitCode::SUCCESS;
            }
            "--help" | "-h" => {
                println!("{USAGE}");
                return ExitCode::SUCCESS;
            }
            other => {
                eprintln!("Unknown option: {other}\n{USAGE}");
                return ExitCode::from(2);
            }
        }
    }

    let engine = if demo {
        dmn_bluetooth_sys::Engine::new_demo()
    } else {
        dmn_bluetooth_sys::Engine::new()
    };
    let engine = match engine {
        Ok(engine) => engine,
        Err(error) => {
            eprintln!("Cannot start the Bluetooth engine: {error}");
            return ExitCode::FAILURE;
        }
    };
    if app::ENGINE.set(Arc::new(engine)).is_err() {
        return ExitCode::FAILURE;
    }

    let settings = cosmic::app::Settings::default().size_limits(
        cosmic::iced::Limits::NONE
            .min_width(360.0)
            .min_height(240.0),
    );
    match cosmic::app::run::<app::App>(settings, app::Flags { demo }) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::FAILURE
        }
    }
}
