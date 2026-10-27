// Copyright © 2026 Chee Bin HOH. All rights reserved.

//! Drives the real C++ engine (demo backend) through the Rust model, the same
//! path the COSMIC view uses, without needing a display.

use std::time::{Duration, Instant};

use dmn_bluetooth_sys::Engine;
use dmn_cosmic_model::{parse_incoming, Command, Identity, Model};

fn pump_until(engine: &Engine, model: &mut Model, done: impl Fn(&Model) -> bool) {
    let deadline = Instant::now() + Duration::from_secs(10);
    while !done(model) {
        assert!(
            Instant::now() < deadline,
            "timed out; status: {}",
            model.snapshot.status
        );
        if let Some(text) = engine.next_message(Duration::from_millis(100)) {
            Model::apply(model, parse_incoming(&text).expect("valid engine JSON"));
        }
    }
}

#[test]
fn demo_phone_can_be_connected_monitored_and_produces_notifications() {
    let engine = Engine::new_demo().unwrap();
    let mut model = Model::default();
    assert!(engine.send(&Command::Hello.to_json()));
    assert!(engine.send(&Command::Refresh.to_json()));
    pump_until(&engine, &mut model, |m| {
        m.ready && m.snapshot.devices.len() >= 3
    });

    let identity = Identity {
        adapter: "00:11:22:33:44:55".into(),
        device: "AA:BB:CC:00:00:01".into(),
    };
    assert!(engine.send(
        &Command::Select {
            identity: identity.clone(),
            selected: true
        }
        .to_json()
    ));
    assert!(engine.send(&Command::SaveSelection.to_json()));
    pump_until(&engine, &mut model, |m| {
        m.device(&identity).is_some_and(|d| d.saved)
    });
    assert!(engine.send(&Command::Connect(identity.clone()).to_json()));
    pump_until(&engine, &mut model, |m| {
        m.device(&identity)
            .is_some_and(|d| d.connected == Some(true))
    });

    let monitor = Command::Monitor {
        identity: identity.clone(),
        enabled: true,
    };
    assert!(engine.send(&monitor.to_json()));
    pump_until(&engine, &mut model, |m| {
        m.device(&identity)
            .is_some_and(|d| d.monitor_state == "active")
            && !m.feed.is_empty()
    });
    assert_eq!(model.monitoring_count(), 1);
}
