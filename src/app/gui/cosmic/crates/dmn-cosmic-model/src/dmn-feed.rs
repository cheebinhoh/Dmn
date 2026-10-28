// Copyright © 2026 Chee Bin HOH. All rights reserved.

//! Bounded notification feed with filtering. Pure; no UI types.

use std::collections::VecDeque;

use crate::protocol::EventRecord;

/// Events kept in memory; the oldest are dropped first.
pub const FEED_LIMIT: usize = 1000;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum KindFilter {
    #[default]
    All,
    Notifications,
    Problems,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FeedFilter {
    /// `adapter/device` key; `None` shows every device.
    pub device: Option<String>,
    pub kind: KindFilter,
    pub search: String,
}

pub fn device_key(adapter: &str, device: &str) -> String {
    format!("{adapter}/{device}")
}

impl EventRecord {
    pub fn is_problem(&self) -> bool {
        matches!(self.kind.as_str(), "error" | "monitor_error")
    }

    /// One-line title for list rows.
    pub fn headline(&self) -> String {
        match (&self.notification, self.kind.as_str()) {
            (Some(n), _) => n
                .title
                .clone()
                .filter(|t| !t.is_empty())
                .or_else(|| n.app_identifier.clone())
                .unwrap_or_else(|| "Notification".to_owned()),
            (None, "state") => format!("Monitor {}", self.state.as_deref().unwrap_or("changed")),
            (None, "monitor_error") => "Monitor failed to start".to_owned(),
            (None, _) => "Monitor error".to_owned(),
        }
    }

    /// Secondary text for list rows and the detail view.
    pub fn summary(&self) -> String {
        if let Some(n) = &self.notification {
            let text = [&n.subtitle, &n.message]
                .into_iter()
                .flatten()
                .filter(|t| !t.is_empty())
                .cloned()
                .collect::<Vec<_>>()
                .join(" — ");
            if !text.is_empty() {
                return text;
            }
            return match n.kind.as_str() {
                "removed" => "Dismissed on the phone".to_owned(),
                "attributes_unavailable" => "Details were not available".to_owned(),
                _ => String::new(),
            };
        }
        self.error.clone().unwrap_or_default()
    }

    /// Multi-line text for the detail dialog: every field the phone provided.
    /// `received` is the already formatted local receipt time.
    pub fn details(&self, received: &str) -> String {
        let value = |text: &Option<String>| match text.as_deref() {
            Some(text) if !text.is_empty() => text.to_owned(),
            _ => "Unavailable".to_owned(),
        };
        let event = match (&self.notification, self.kind.as_str()) {
            (Some(n), _) => match n.kind.as_str() {
                "added" => "Added",
                "updated" => "Updated",
                "removed" => "Removed",
                "attributes_unavailable" => "Attributes unavailable",
                _ => "Notification",
            },
            (None, "state") => "Monitor state",
            (None, _) => "Monitor error",
        };
        let mut lines = vec![
            format!("Device: {}", self.name),
            format!("Adapter: {}", self.adapter),
            format!("Device address: {}", self.device),
            format!("Received: {received}"),
            format!("Event: {event}"),
        ];
        if let Some(n) = &self.notification {
            lines.push(format!("Notification UID: {}", n.uid));
            lines.push(format!(
                "Category: {} ({})",
                category_name(n.category),
                n.category
            ));
            if n.kind == "removed" {
                lines.push(
                    "Notification was removed; ANCS provides no current attributes for this event."
                        .to_owned(),
                );
            } else {
                lines.push(format!("App identifier: {}", value(&n.app_identifier)));
                lines.push(format!("Title: {}", value(&n.title)));
                lines.push(format!("Subtitle: {}", value(&n.subtitle)));
                lines.push(format!("Message: {}", value(&n.message)));
                lines.push(format!("Phone-provided date: {}", value(&n.date)));
                lines.push(format!("Category count: {}", n.category_count));
                lines.push(format!("Flags: {}", n.flags));
            }
        }
        if let Some(state) = &self.state {
            lines.push(format!("Monitor state: {state}"));
        }
        if let Some(error) = &self.error {
            lines.push(format!("Error: {error}"));
        }
        lines.join("\n")
    }

    fn matches(&self, filter: &FeedFilter) -> bool {
        if let Some(key) = &filter.device {
            if device_key(&self.adapter, &self.device) != *key {
                return false;
            }
        }
        match filter.kind {
            KindFilter::All => {}
            KindFilter::Notifications if self.notification.is_none() => return false,
            KindFilter::Problems if !self.is_problem() => return false,
            _ => {}
        }
        let needle = filter.search.trim().to_lowercase();
        if needle.is_empty() {
            return true;
        }
        let mut haystack = format!("{} {} {}", self.name, self.headline(), self.summary());
        if let Some(app) = self
            .notification
            .as_ref()
            .and_then(|n| n.app_identifier.as_ref())
        {
            haystack.push(' ');
            haystack.push_str(app);
        }
        haystack.to_lowercase().contains(&needle)
    }
}

/// ANCS category names, as shown by the GTK frontend.
fn category_name(category: u32) -> &'static str {
    match category {
        0 => "Other",
        1 => "Incoming call",
        2 => "Missed call",
        3 => "Voicemail",
        4 => "Social",
        5 => "Schedule",
        6 => "Email",
        7 => "News",
        8 => "Health and fitness",
        9 => "Business and finance",
        10 => "Location",
        11 => "Entertainment",
        _ => "Unknown category",
    }
}

#[derive(Debug, Default)]
pub struct Feed {
    events: VecDeque<EventRecord>,
}

impl Feed {
    pub fn push(&mut self, event: EventRecord) {
        if self.events.len() == FEED_LIMIT {
            self.events.pop_front();
        }
        self.events.push_back(event);
    }

    pub fn len(&self) -> usize {
        self.events.len()
    }

    pub fn is_empty(&self) -> bool {
        self.events.is_empty()
    }

    pub fn clear(&mut self) {
        self.events.clear();
    }

    pub fn get(&self, sequence: u64) -> Option<&EventRecord> {
        self.events.iter().find(|e| e.sequence == sequence)
    }

    /// Matching events, newest first.
    pub fn visible<'a>(&'a self, filter: &'a FeedFilter) -> impl Iterator<Item = &'a EventRecord> {
        self.events.iter().rev().filter(move |e| e.matches(filter))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::Notification;

    fn note(sequence: u64, device: &str, title: &str, message: &str) -> EventRecord {
        EventRecord {
            sequence,
            adapter: "A".into(),
            device: device.into(),
            name: format!("Phone {device}"),
            kind: "notification".into(),
            notification: Some(Notification {
                kind: "added".into(),
                title: Some(title.into()),
                message: Some(message.into()),
                ..Notification::default()
            }),
            ..EventRecord::default()
        }
    }

    #[test]
    fn keeps_only_the_most_recent_events() {
        let mut feed = Feed::default();
        for i in 0..(FEED_LIMIT as u64 + 5) {
            feed.push(note(i, "D", "t", "m"));
        }
        assert_eq!(feed.len(), FEED_LIMIT);
        assert!(feed.get(4).is_none());
        assert!(feed.get(5).is_some());
        let all = FeedFilter::default();
        let newest = feed.visible(&all).next().unwrap();
        assert_eq!(newest.sequence, FEED_LIMIT as u64 + 4);
    }

    #[test]
    fn filters_by_device_kind_and_text() {
        let mut feed = Feed::default();
        feed.push(note(1, "D1", "Lunch", "Are we on?"));
        feed.push(note(2, "D2", "Build", "Pipeline finished"));
        feed.push(EventRecord {
            sequence: 3,
            adapter: "A".into(),
            device: "D1".into(),
            kind: "monitor_error".into(),
            error: Some("ANCS service not found".into()),
            ..EventRecord::default()
        });

        let by_device = FeedFilter {
            device: Some(device_key("A", "D2")),
            ..FeedFilter::default()
        };
        assert_eq!(feed.visible(&by_device).count(), 1);

        let problems = FeedFilter {
            kind: KindFilter::Problems,
            ..FeedFilter::default()
        };
        assert_eq!(feed.visible(&problems).next().unwrap().sequence, 3);

        let search = FeedFilter {
            search: "  PIPELINE ".into(),
            ..FeedFilter::default()
        };
        assert_eq!(feed.visible(&search).next().unwrap().sequence, 2);

        let notifications = FeedFilter {
            kind: KindFilter::Notifications,
            ..FeedFilter::default()
        };
        assert_eq!(feed.visible(&notifications).count(), 2);
    }

    #[test]
    fn headline_and_summary_degrade_gracefully() {
        let removed = EventRecord {
            notification: Some(Notification {
                kind: "removed".into(),
                ..Notification::default()
            }),
            ..EventRecord::default()
        };
        assert_eq!(removed.headline(), "Notification");
        assert_eq!(removed.summary(), "Dismissed on the phone");
    }

    #[test]
    fn details_list_every_notification_field() {
        let mut event = note(1, "D", "Title", "Body");
        let n = event.notification.as_mut().unwrap();
        n.uid = 42;
        n.category = 4;
        n.category_count = 3;
        let text = event.details("now");
        for expected in [
            "Device: Phone D",
            "Device address: D",
            "Received: now",
            "Event: Added",
            "Notification UID: 42",
            "Category: Social (4)",
            "Title: Title",
            "Subtitle: Unavailable",
            "Message: Body",
            "Category count: 3",
        ] {
            assert!(text.contains(expected), "missing {expected:?} in {text}");
        }
    }
}
