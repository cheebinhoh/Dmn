// Copyright © 2026 Chee Bin HOH. All rights reserved.

//! UI-independent model for `dmn-bluetooth-cosmic`.
//!
//! Everything here is plain data and pure functions so it is tested with
//! `cargo test -p dmn-cosmic-model` without a shell, a display or the C++ core.

#[path = "dmn-alert.rs"]
pub mod alert;
#[path = "dmn-feed.rs"]
pub mod feed;
#[path = "dmn-protocol.rs"]
pub mod protocol;
#[path = "dmn-settings.rs"]
pub mod settings;

pub use alert::{alert_device, make_alert, DesktopAlert};
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
    /// checkbox is merely toggled. `selected_only` hides unticked devices and
    /// `search` (case-insensitive, any displayed part) hides non-matches.
    pub fn device_groups(&self, selected_only: bool, search: &str) -> (Vec<&Device>, Vec<&Device>) {
        let needle = search.trim().to_lowercase();
        self.snapshot
            .devices
            .iter()
            .filter(|d| !selected_only || d.selected)
            .filter(|d| d.matches_search(&needle))
            .partition(|d| d.saved)
    }

    /// The app scans for the whole session. True when an adapter is known
    /// but scanning is off and no scan/connect operation is in flight, so the
    /// frontend should (re)request it; the caller throttles retries.
    pub fn should_start_scan(&self) -> bool {
        self.ready
            && !self.snapshot.scanning
            && !self.snapshot.busy
            && !self.snapshot.adapters.is_empty()
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
        let (previous, nearby) = model.device_groups(false, "");
        assert_eq!(ids(previous), ["1", "2"]);
        assert_eq!(ids(nearby), ["3", "4"]);

        let (previous, nearby) = model.device_groups(true, "");
        assert_eq!(ids(previous), ["1"]);
        assert_eq!(ids(nearby), ["3"]);
    }

    #[test]
    fn device_search_matches_any_part_in_both_groups() {
        let mut model = Model::default();
        model.apply(
            parse_incoming(
                r#"{"type":"state","devices":[
                {"adapter":"A","device":"AA:01","name":"Alice iPhone","saved":true},
                {"adapter":"A","device":"BB:02","name":"Bob Phone","status":"Paired","saved":false},
                {"adapter":"A","device":"CC:03","name":"Carol","saved":false}]}"#,
            )
            .unwrap(),
        );
        let count = |s: &str| {
            let (a, b) = model.device_groups(false, s);
            (a.len(), b.len())
        };
        assert_eq!(count(""), (1, 2));
        assert_eq!(count("  ALICE "), (1, 0));
        assert_eq!(count("bb:0"), (0, 1));
        assert_eq!(count("paired"), (0, 1));
        assert_eq!(count("phone"), (1, 1));
        assert_eq!(count("zzz"), (0, 0));
    }

    #[test]
    fn scan_starts_only_when_ready_idle_and_an_adapter_exists() {
        let mut model = Model::default();
        assert!(!model.should_start_scan());
        model.apply(parse_incoming(r#"{"type":"ready","protocol":1}"#).unwrap());
        model.apply(parse_incoming(r#"{"type":"state","adapters":[]}"#).unwrap());
        assert!(!model.should_start_scan());
        let state = |extra: &str| {
            parse_incoming(&format!(
                r#"{{"type":"state","adapters":[{{"address":"A"}}]{extra}}}"#
            ))
            .unwrap()
        };
        model.apply(state(""));
        assert!(model.should_start_scan());
        model.apply(state(r#","busy":true"#));
        assert!(!model.should_start_scan());
        model.apply(state(r#","scanning":true"#));
        assert!(!model.should_start_scan());
    }

    #[test]
    fn protocol_mismatch_is_reported_and_blocks_readiness() {
        let mut model = Model::default();
        model.apply(parse_incoming(r#"{"type":"ready","protocol":2}"#).unwrap());
        assert!(!model.ready);
        assert!(model.incompatible.unwrap().contains("protocol 2"));
    }
}
