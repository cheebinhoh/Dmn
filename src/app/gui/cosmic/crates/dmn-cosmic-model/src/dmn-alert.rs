// Copyright © 2026 Chee Bin HOH. All rights reserved.

//! Privacy-safe desktop alert policy, mirroring the GTK frontend's
//! `desktop-alert-policy`: one generic alert per device, replaced as more
//! notifications arrive. Device names and notification text are never used.

use crate::feed::device_key;
use crate::protocol::EventRecord;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DesktopAlert {
    /// Stable per-device key used to replace or withdraw the alert.
    pub id: String,
    pub title: String,
    pub body: String,
}

/// Returns the device key when the event is a newly added notification.
pub fn alert_device(event: &EventRecord) -> Option<String> {
    let added = event
        .notification
        .as_ref()
        .is_some_and(|n| n.kind == "added");
    (event.kind == "notification" && added).then(|| device_key(&event.adapter, &event.device))
}

/// Builds the generic alert for `count` aggregated notifications.
pub fn make_alert(device: &str, count: usize) -> DesktopAlert {
    let body = if count == 1 {
        "1 new notification arrived. Open Dmn Bluetooth to view it.".to_owned()
    } else {
        format!("{count} new notifications arrived. Open Dmn Bluetooth to view them.")
    };
    DesktopAlert {
        id: format!("device:{device}"),
        title: "New iPhone notification".to_owned(),
        body,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::Notification;

    fn event(kind: &str, nkind: Option<&str>) -> EventRecord {
        EventRecord {
            adapter: "A".into(),
            device: "D".into(),
            kind: kind.into(),
            notification: nkind.map(|k| Notification {
                kind: k.into(),
                title: Some("secret".into()),
                ..Notification::default()
            }),
            ..EventRecord::default()
        }
    }

    #[test]
    fn only_added_notifications_alert() {
        assert_eq!(
            alert_device(&event("notification", Some("added"))),
            Some("A/D".to_owned())
        );
        assert_eq!(alert_device(&event("notification", Some("updated"))), None);
        assert_eq!(alert_device(&event("notification", Some("removed"))), None);
        assert_eq!(alert_device(&event("state", None)), None);
    }

    #[test]
    fn alert_is_generic_and_pluralised() {
        let one = make_alert("A/D", 1);
        assert_eq!(one.id, "device:A/D");
        assert!(one.body.starts_with("1 new notification arrived"));
        assert!(make_alert("A/D", 3).body.starts_with("3 new notifications"));
        assert!(!one.body.contains("secret"));
    }
}
