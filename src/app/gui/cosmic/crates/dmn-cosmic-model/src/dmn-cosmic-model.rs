// Copyright © 2026 Chee Bin HOH. All rights reserved.

//! UI-independent model for `dmn-bluetooth-cosmic`.
//!
//! Everything here is plain data and pure functions so it is tested with
//! `cargo test -p dmn-cosmic-model` without a shell, a display or the C++ core.

#[path = "dmn-feed.rs"]
pub mod feed;
#[path = "dmn-protocol.rs"]
pub mod protocol;
#[path = "dmn-settings.rs"]
pub mod settings;

pub use feed::{device_key, Feed, FeedFilter, KindFilter, FEED_LIMIT};
pub use protocol::{
    parse_incoming, Command, Device, EventRecord, Identity, Incoming, Persist, Snapshot, Sort,
    SortColumn, PROTOCOL_VERSION,
};
pub use settings::Settings;

/// Everything the view needs, updated only through [`Model::apply`].
#[derive(Debug, Default)]
pub struct Model {
    pub snapshot: Snapshot,
    pub feed: Feed,
    pub filter: FeedFilter,
    pub backend: String,
    pub ready: bool,
    pub closed: bool,
    /// Set when the engine speaks a protocol this frontend cannot use.
    pub incompatible: Option<String>,
    /// Latest problem not tied to a device (engine error, bad message).
    pub notice: Option<String>,
}

impl Model {
    /// Applies one engine message. Returns settings to store, if the engine
    /// asked for persistence.
    pub fn apply(&mut self, message: Incoming) -> Option<Persist> {
        match message {
            Incoming::Ready {
                protocol, backend, ..
            } => {
                self.backend = backend;
                self.ready = protocol == PROTOCOL_VERSION;
                self.incompatible = (!self.ready).then(|| {
                    format!(
                        "The Bluetooth engine uses protocol {protocol}; \
                         this app needs protocol {PROTOCOL_VERSION}."
                    )
                });
            }
            Incoming::State(snapshot) => self.snapshot = *snapshot,
            Incoming::Event(event) => self.feed.push(*event),
            Incoming::Persist(persist) => return Some(persist),
            Incoming::Error(message) => self.notice = Some(message),
            Incoming::Bye => self.closed = true,
            Incoming::Unknown(_) => {}
        }
        None
    }

    pub fn device(&self, identity: &Identity) -> Option<&Device> {
        self.snapshot
            .devices
            .iter()
            .find(|d| d.adapter == identity.adapter && d.device == identity.device)
    }

    /// Splits devices into (previous selected, nearby) groups, keeping the
    /// engine's order. Grouping follows the *saved* state, so a device changes
    /// group only when its selection is saved or discarded, never while the
    /// checkbox is merely toggled. `selected_only` hides unticked devices.
    pub fn device_groups(&self, selected_only: bool) -> (Vec<&Device>, Vec<&Device>) {
        self.snapshot
            .devices
            .iter()
            .filter(|d| !selected_only || d.selected)
            .partition(|d| d.saved)
    }

    pub fn monitoring_count(&self) -> usize {
        self.snapshot
            .devices
            .iter()
            .filter(|d| d.monitor_state == "active")
            .count()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reducer_tracks_state_events_and_persistence() {
        let mut model = Model::default();
        assert!(model
            .apply(parse_incoming(r#"{"type":"ready","protocol":1,"backend":"demo"}"#).unwrap())
            .is_none());
        assert!(model.ready);
        assert_eq!(model.backend, "demo");

        model.apply(
            parse_incoming(
                r#"{"type":"state","devices":[{"adapter":"A","device":"D","monitor_state":"active"}]}"#,
            )
            .unwrap(),
        );
        assert_eq!(model.monitoring_count(), 1);
        assert!(model
            .device(&Identity {
                adapter: "A".into(),
                device: "D".into()
            })
            .is_some());

        model.apply(
            parse_incoming(r#"{"type":"event","sequence":1,"kind":"notification"}"#).unwrap(),
        );
        assert_eq!(model.feed.len(), 1);

        let persist = model
            .apply(parse_incoming(r#"{"type":"persist","saved":[],"monitoring":[]}"#).unwrap());
        assert!(persist.is_some());
    }

    #[test]
    fn device_groups_follow_saved_state_and_selected_only() {
        let mut model = Model::default();
        model.apply(
            parse_incoming(
                r#"{"type":"state","devices":[
                {"adapter":"A","device":"1","saved":true,"selected":true},
                {"adapter":"A","device":"2","saved":true,"selected":false},
                {"adapter":"A","device":"3","saved":false,"selected":true},
                {"adapter":"A","device":"4","saved":false,"selected":false}]}"#,
            )
            .unwrap(),
        );
        let ids = |devices: Vec<&Device>| -> Vec<String> {
            devices.into_iter().map(|d| d.device.clone()).collect()
        };

        // A ticked-but-unsaved device (3) stays in the nearby group, and an
        // unticked-but-still-saved device (2) stays in the top group.
        let (previous, nearby) = model.device_groups(false);
        assert_eq!(ids(previous), ["1", "2"]);
        assert_eq!(ids(nearby), ["3", "4"]);

        let (previous, nearby) = model.device_groups(true);
        assert_eq!(ids(previous), ["1"]);
        assert_eq!(ids(nearby), ["3"]);
    }

    #[test]
    fn protocol_mismatch_is_reported_and_blocks_readiness() {
        let mut model = Model::default();
        model.apply(parse_incoming(r#"{"type":"ready","protocol":2}"#).unwrap());
        assert!(!model.ready);
        assert!(model.incompatible.unwrap().contains("protocol 2"));
    }
}
