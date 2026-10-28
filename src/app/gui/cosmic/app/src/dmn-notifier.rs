// Copyright © 2026 Chee Bin HOH. All rights reserved.

//! Desktop alerts through the freedesktop `org.freedesktop.Notifications`
//! service, which the COSMIC notification applet serves. A worker thread owns
//! the D-Bus connection so the UI never blocks; failures are logged only.

use std::collections::HashMap;
use std::sync::mpsc::{channel, Receiver, Sender};

use dmn_cosmic_model::{make_alert, DesktopAlert};
use zbus::blocking::Connection;
use zbus::zvariant::Value;

const APP_NAME: &str = "Dmn Bluetooth";
const APP_ICON: &str = "io.github.cheebinhoh.DmnBluetoothCosmic";

enum Request {
    /// Show or replace the alert for a device key with its aggregated count.
    Show(String, usize),
    Withdraw(String),
    WithdrawAll,
}

/// Cheap handle to the notification worker.
pub struct Notifier {
    tx: Sender<Request>,
    counts: HashMap<String, usize>,
}

impl Notifier {
    pub fn new() -> Self {
        let (tx, rx) = channel();
        std::thread::spawn(move || worker(rx));
        Self {
            tx,
            counts: HashMap::new(),
        }
    }

    /// Records one more notification for `device` and updates its alert.
    pub fn notify(&mut self, device: &str) {
        let count = self.counts.entry(device.to_owned()).or_insert(0);
        *count += 1;
        let _ = self.tx.send(Request::Show(device.to_owned(), *count));
    }

    pub fn withdraw(&mut self, device: &str) {
        if self.counts.remove(device).is_some() {
            let _ = self.tx.send(Request::Withdraw(device.to_owned()));
        }
    }

    pub fn withdraw_all(&mut self) {
        if !self.counts.is_empty() {
            self.counts.clear();
            let _ = self.tx.send(Request::WithdrawAll);
        }
    }
}

fn worker(rx: Receiver<Request>) {
    let connection = match Connection::session() {
        Ok(connection) => connection,
        Err(error) => {
            eprintln!("desktop alerts unavailable: {error}");
            return;
        }
    };
    // Server-assigned ids, keyed by alert id, so a later alert replaces the
    // earlier one for the same device.
    let mut ids: HashMap<String, u32> = HashMap::new();
    while let Ok(request) = rx.recv() {
        match request {
            Request::Show(device, count) => {
                let alert = make_alert(&device, count);
                let replaces = ids.get(&alert.id).copied().unwrap_or(0);
                match show(&connection, &alert, replaces) {
                    Ok(id) => {
                        ids.insert(alert.id, id);
                    }
                    Err(error) => eprintln!("could not show desktop alert: {error}"),
                }
            }
            Request::Withdraw(device) => {
                if let Some(id) = ids.remove(&make_alert(&device, 1).id) {
                    close(&connection, id);
                }
            }
            Request::WithdrawAll => {
                for (_, id) in ids.drain() {
                    close(&connection, id);
                }
            }
        }
    }
}

fn show(connection: &Connection, alert: &DesktopAlert, replaces: u32) -> zbus::Result<u32> {
    let hints: HashMap<&str, Value<'_>> = HashMap::from([("desktop-entry", Value::from(APP_ICON))]);
    let actions: Vec<&str> = Vec::new();
    connection
        .call_method(
            Some("org.freedesktop.Notifications"),
            "/org/freedesktop/Notifications",
            Some("org.freedesktop.Notifications"),
            "Notify",
            &(
                APP_NAME,
                replaces,
                APP_ICON,
                alert.title.as_str(),
                alert.body.as_str(),
                actions,
                hints,
                -1i32,
            ),
        )?
        .body()
        .deserialize()
}

fn close(connection: &Connection, id: u32) {
    let _ = connection.call_method(
        Some("org.freedesktop.Notifications"),
        "/org/freedesktop/Notifications",
        Some("org.freedesktop.Notifications"),
        "CloseNotification",
        &(id,),
    );
}
