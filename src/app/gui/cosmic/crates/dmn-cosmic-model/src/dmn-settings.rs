// Copyright © 2026 Chee Bin HOH. All rights reserved.

//! Persistent frontend settings. Stores only stable identities and UI
//! preferences, never notification content.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::protocol::{Command, Identity, Persist, Sort};

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Settings {
    pub saved: Vec<Identity>,
    pub monitoring: Vec<Identity>,
    pub sort: Sort,
    /// Show only devices in the current draft selection.
    pub selected_only: bool,
    /// Show generic desktop alerts for new notifications.
    pub desktop_alerts: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            saved: Vec::new(),
            monitoring: Vec::new(),
            sort: Sort::default(),
            selected_only: false,
            desktop_alerts: true,
        }
    }
}

impl Settings {
    /// `$XDG_CONFIG_HOME/dmn-bluetooth-cosmic/settings.json` (or `~/.config`).
    pub fn default_path() -> Option<PathBuf> {
        let base = std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .filter(|p| p.is_absolute())
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))?;
        Some(base.join("dmn-bluetooth-cosmic").join("settings.json"))
    }

    /// Missing files yield defaults; unreadable or corrupt files are reported
    /// so the caller can warn, and also yield defaults.
    pub fn load(path: &Path) -> (Self, Option<String>) {
        match fs::read_to_string(path) {
            Ok(text) => match serde_json::from_str(&text) {
                Ok(settings) => (settings, None),
                Err(error) => (
                    Self::default(),
                    Some(format!("Ignoring unreadable settings: {error}")),
                ),
            },
            Err(error) if error.kind() == io::ErrorKind::NotFound => (Self::default(), None),
            Err(error) => (
                Self::default(),
                Some(format!("Could not read settings: {error}")),
            ),
        }
    }

    /// Writes atomically so a crash never leaves a truncated file.
    pub fn save(&self, path: &Path) -> io::Result<()> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        let temporary = path.with_extension("json.tmp");
        fs::write(&temporary, serde_json::to_vec_pretty(self)?)?;
        fs::rename(temporary, path)
    }

    pub fn apply_persist(&mut self, persist: Persist) {
        self.saved = persist.saved;
        self.monitoring = persist.monitoring;
    }

    pub fn configure_command(&self) -> Command {
        Command::Configure {
            saved: self.saved.clone(),
            monitoring: self.monitoring.clone(),
            sort: self.sort.clone(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::SortColumn;

    fn temp_path(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("dmn-cosmic-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        dir.join("settings.json")
    }

    #[test]
    fn round_trips_and_creates_parent_directories() {
        let path = temp_path("roundtrip");
        let settings = Settings {
            saved: vec![Identity {
                adapter: "A".into(),
                device: "D".into(),
            }],
            monitoring: vec![],
            sort: Sort {
                column: SortColumn::Status,
                ascending: false,
            },
            selected_only: true,
            desktop_alerts: false,
        };
        settings.save(&path).unwrap();
        let (loaded, warning) = Settings::load(&path);
        assert_eq!(loaded, settings);
        assert!(warning.is_none());
        let _ = fs::remove_dir_all(path.parent().unwrap());
    }

    #[test]
    fn missing_file_is_default_and_corrupt_file_warns() {
        let path = temp_path("corrupt");
        assert_eq!(Settings::load(&path), (Settings::default(), None));
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, "{not json").unwrap();
        let (loaded, warning) = Settings::load(&path);
        assert_eq!(loaded, Settings::default());
        assert!(warning.unwrap().contains("unreadable"));
        let _ = fs::remove_dir_all(path.parent().unwrap());
    }

    #[test]
    fn persist_replaces_saved_and_monitoring() {
        let mut settings = Settings::default();
        settings.apply_persist(Persist {
            saved: vec![Identity::default()],
            monitoring: vec![Identity::default()],
        });
        assert_eq!(settings.saved.len(), 1);
        assert_eq!(settings.monitoring.len(), 1);
    }
}
