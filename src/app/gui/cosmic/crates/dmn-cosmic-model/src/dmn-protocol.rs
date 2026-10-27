// Copyright © 2026 Chee Bin HOH. All rights reserved.

//! Wire types for protocol v1 between the frontend and the C++ engine.
//!
//! Unknown fields and unknown message kinds are ignored so the protocol can
//! grow without breaking an older frontend.

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

/// Protocol major version this frontend understands.
pub const PROTOCOL_VERSION: u32 = 1;

/// Stable device identity: Bluetooth addresses, never object paths.
#[derive(Clone, Debug, Default, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct Identity {
    pub adapter: String,
    pub device: String,
}

#[derive(Clone, Debug, Default, PartialEq, Deserialize)]
#[serde(default)]
pub struct Adapter {
    pub address: String,
    pub name: String,
    pub powered: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Deserialize)]
#[serde(default)]
pub struct Device {
    pub adapter: String,
    pub device: String,
    pub name: String,
    pub status: String,
    pub paired: Option<bool>,
    pub connected: Option<bool>,
    pub present: bool,
    pub saved: bool,
    pub selected: bool,
    pub connecting: bool,
    pub disconnecting: bool,
    pub monitoring_requested: bool,
    pub monitor_state: String,
    pub monitor_detail: String,
    pub can_connect: bool,
    pub can_disconnect: bool,
    pub can_start_monitoring: bool,
    pub can_stop_monitoring: bool,
}

impl Device {
    pub fn identity(&self) -> Identity {
        Identity {
            adapter: self.adapter.clone(),
            device: self.device.clone(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SortColumn {
    #[default]
    Name,
    Status,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct Sort {
    pub column: SortColumn,
    pub ascending: bool,
}

impl Default for Sort {
    fn default() -> Self {
        Self {
            column: SortColumn::Name,
            ascending: true,
        }
    }
}

/// Full engine snapshot; replaces the previous one.
#[derive(Clone, Debug, Default, PartialEq, Deserialize)]
#[serde(default)]
pub struct Snapshot {
    pub adapters: Vec<Adapter>,
    pub scanning: bool,
    pub busy: bool,
    pub status: String,
    pub unsaved_changes: bool,
    pub sort: Sort,
    pub devices: Vec<Device>,
}

#[derive(Clone, Debug, Default, PartialEq, Deserialize)]
#[serde(default)]
pub struct Notification {
    pub kind: String,
    pub uid: u32,
    pub category: u32,
    pub flags: u32,
    pub app_identifier: Option<String>,
    pub title: Option<String>,
    pub subtitle: Option<String>,
    pub message: Option<String>,
    pub date: Option<String>,
}

#[derive(Clone, Debug, Default, PartialEq, Deserialize)]
#[serde(default)]
pub struct EventRecord {
    pub sequence: u64,
    pub received_ms: i64,
    pub adapter: String,
    pub device: String,
    pub name: String,
    /// `notification`, `state`, `error` or `monitor_error`.
    pub kind: String,
    pub state: Option<String>,
    pub error: Option<String>,
    pub notification: Option<Notification>,
}

/// Saved selection and monitoring intents the frontend must store.
#[derive(Clone, Debug, Default, PartialEq, Deserialize)]
#[serde(default)]
pub struct Persist {
    pub saved: Vec<Identity>,
    pub monitoring: Vec<Identity>,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Incoming {
    Ready {
        protocol: u32,
        engine: String,
        backend: String,
    },
    State(Box<Snapshot>),
    Event(Box<EventRecord>),
    Persist(Persist),
    Error(String),
    Bye,
    /// A well-formed message of a kind this frontend does not know.
    Unknown(String),
}

/// Parses one engine message.
pub fn parse_incoming(text: &str) -> Result<Incoming, serde_json::Error> {
    let value: Value = serde_json::from_str(text)?;
    let kind = value
        .get("type")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned();
    Ok(match kind.as_str() {
        "ready" => Incoming::Ready {
            protocol: value
                .get("protocol")
                .and_then(Value::as_u64)
                .and_then(|v| u32::try_from(v).ok())
                .unwrap_or(0),
            engine: string_field(&value, "engine"),
            backend: string_field(&value, "backend"),
        },
        "state" => Incoming::State(Box::new(serde_json::from_value(value)?)),
        "event" => Incoming::Event(Box::new(serde_json::from_value(value)?)),
        "persist" => Incoming::Persist(serde_json::from_value(value)?),
        "error" => Incoming::Error(string_field(&value, "message")),
        "bye" => Incoming::Bye,
        _ => Incoming::Unknown(kind),
    })
}

fn string_field(value: &Value, key: &str) -> String {
    value
        .get(key)
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned()
}

/// Commands the frontend sends to the engine.
#[derive(Clone, Debug, PartialEq)]
pub enum Command {
    Hello,
    Configure {
        saved: Vec<Identity>,
        monitoring: Vec<Identity>,
        sort: Sort,
    },
    Refresh,
    Scan {
        enabled: bool,
    },
    Select {
        identity: Identity,
        selected: bool,
    },
    SaveSelection,
    DiscardSelection,
    SortBy(Sort),
    Connect(Identity),
    Disconnect(Identity),
    Monitor {
        identity: Identity,
        enabled: bool,
    },
    Shutdown,
}

impl Command {
    pub fn to_json(&self) -> String {
        let value = match self {
            Self::Hello => json!({"type": "hello"}),
            Self::Configure {
                saved,
                monitoring,
                sort,
            } => json!({
                "type": "configure",
                "saved": saved,
                "monitoring": monitoring,
                "sort": sort,
            }),
            Self::Refresh => json!({"type": "refresh"}),
            Self::Scan { enabled } => json!({"type": "scan", "enabled": enabled}),
            Self::Select { identity, selected } => json!({
                "type": "select",
                "adapter": identity.adapter,
                "device": identity.device,
                "selected": selected,
            }),
            Self::SaveSelection => json!({"type": "save_selection"}),
            Self::DiscardSelection => json!({"type": "discard_selection"}),
            Self::SortBy(sort) => json!({
                "type": "sort",
                "column": sort.column,
                "ascending": sort.ascending,
            }),
            Self::Connect(identity) => device_command("connect", identity),
            Self::Disconnect(identity) => device_command("disconnect", identity),
            Self::Monitor { identity, enabled } => {
                let mut value = device_command("monitor", identity);
                value["enabled"] = json!(enabled);
                value
            }
            Self::Shutdown => json!({"type": "shutdown"}),
        };
        value.to_string()
    }
}

fn device_command(kind: &str, identity: &Identity) -> Value {
    json!({"type": kind, "adapter": identity.adapter, "device": identity.device})
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_state_tolerating_unknown_fields_and_nulls() {
        let text = r#"{"type":"state","future":1,"scanning":true,"status":"Hi",
            "devices":[{"adapter":"A","device":"D","name":"Phone","paired":null,
            "connected":true,"can_connect":false,"extra":{}}]}"#;
        let Incoming::State(state) = parse_incoming(text).unwrap() else {
            panic!("expected state");
        };
        assert!(state.scanning);
        assert_eq!(state.devices[0].paired, None);
        assert_eq!(state.devices[0].connected, Some(true));
        assert_eq!(state.sort, Sort::default());
    }

    #[test]
    fn unknown_message_kinds_are_not_errors() {
        assert_eq!(
            parse_incoming(r#"{"type":"from_the_future"}"#).unwrap(),
            Incoming::Unknown("from_the_future".into())
        );
        assert!(parse_incoming("{broken").is_err());
    }

    #[test]
    fn parses_ready_event_and_persist() {
        assert_eq!(
            parse_incoming(r#"{"type":"ready","protocol":1,"engine":"1","backend":"demo"}"#)
                .unwrap(),
            Incoming::Ready {
                protocol: 1,
                engine: "1".into(),
                backend: "demo".into()
            }
        );
        let Incoming::Event(event) = parse_incoming(
            r#"{"type":"event","sequence":7,"received_ms":5,"adapter":"A","device":"D",
            "kind":"notification","notification":{"title":"Hi","uid":3}}"#,
        )
        .unwrap() else {
            panic!("expected event");
        };
        assert_eq!(event.sequence, 7);
        assert_eq!(event.notification.unwrap().title.as_deref(), Some("Hi"));
        let Incoming::Persist(persist) = parse_incoming(
            r#"{"type":"persist","saved":[{"adapter":"A","device":"D"}],"monitoring":[]}"#,
        )
        .unwrap() else {
            panic!("expected persist");
        };
        assert_eq!(persist.saved.len(), 1);
    }

    #[test]
    fn commands_serialize_to_the_documented_shape() {
        let id = Identity {
            adapter: "A".into(),
            device: "D".into(),
        };
        let monitor: Value = serde_json::from_str(
            &Command::Monitor {
                identity: id.clone(),
                enabled: true,
            }
            .to_json(),
        )
        .unwrap();
        assert_eq!(monitor["type"], "monitor");
        assert_eq!(monitor["device"], "D");
        assert_eq!(monitor["enabled"], true);

        let sort: Value = serde_json::from_str(
            &Command::SortBy(Sort {
                column: SortColumn::Status,
                ascending: false,
            })
            .to_json(),
        )
        .unwrap();
        assert_eq!(sort["column"], "status");
        assert_eq!(sort["ascending"], false);
    }
}
