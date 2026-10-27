// Copyright © 2026 Chee Bin HOH. All rights reserved.

//! COSMIC view and update logic. All Bluetooth behavior lives in the C++
//! engine; this module only renders [`Model`] and forwards user intent.
//!
//! Architecture: the UI sends JSON commands to the engine (`App::send`) and
//! receives JSON messages through a subscription that polls
//! `Engine::next_message` on a helper thread and forwards each text as
//! `Message::Engine`. `Message::Engine` is parsed into [`Incoming`] and applied
//! to [`Model`] (the pure reducer in `dmn-cosmic-model`). Selection and
//! monitoring intent are persisted to `settings.json` by this module only; the
//! engine never writes to disk.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};

use crate::notifier::Notifier;
use cosmic::app::context_drawer::{self, ContextDrawer};
use cosmic::iced::futures::{self, SinkExt};
use cosmic::iced::{Alignment, Length, Subscription};
use cosmic::prelude::*;
use cosmic::widget;
use cosmic::widget::about::About;
use cosmic::widget::menu::action::MenuAction;
use cosmic::widget::menu::{self, ItemHeight, ItemWidth};
use cosmic::widget::RcElementWrapper;
use dmn_bluetooth_sys::Engine;
use dmn_cosmic_model::{
    alert_device, device_key, parse_incoming, Command, Device, EventRecord, Incoming, KindFilter,
    Model, Settings, Sort, SortColumn,
};

/// Set once by `main`; the subscription stream is a plain `fn` so it reaches
/// the engine through this cell rather than a capture.
pub static ENGINE: OnceLock<Arc<Engine>> = OnceLock::new();

/// Vertical gap between device cards and between notification rows, kept
/// identical so both panes have the same rhythm (half of the `space_xs` step).
fn card_gap() -> u16 {
    cosmic::theme::spacing().space_xs / 2
}

/// Heading-to-list gap is 56% of `space_xs`; the gap between the two device
/// sections is 130% of it.
const HEADING_GAP_SCALE: f32 = 0.56;
const SECTION_GAP_SCALE: f32 = 1.3;

fn scaled_gap(scale: f32) -> u16 {
    (f32::from(cosmic::theme::spacing().space_xs) * scale).round() as u16
}

/// Minimum time between automatic scan requests.
const SCAN_RETRY_INTERVAL: Duration = Duration::from_secs(10);

/// Wrap width of the notification tooltip.
const TOOLTIP_MAX_WIDTH: f32 = 480.0;

/// Fixed so the width-aware toolbar does not claim the page height.
const TOOLBAR_HEIGHT: f32 = 40.0;

/// Engine poll period; also bounds how quickly the poll thread notices shutdown.
const POLL_INTERVAL: Duration = Duration::from_millis(200);

/// Start-up flags from the command line.
pub struct Flags {
    pub demo: bool,
}

/// Application state: the engine-fed model plus UI-only state.
pub struct App {
    core: cosmic::Core,
    model: Model,
    settings: Settings,
    settings_path: Option<PathBuf>,
    show_devices: bool,
    /// Filters both device groups; UI-only, not persisted.
    device_search: String,
    last_scan_request: Option<Instant>,
    /// Posts and withdraws desktop alerts.
    notifier: Notifier,
    about: About,
    /// Whether the About context drawer is open.
    show_about: bool,
    /// Sequence of the event whose detail dialog is open.
    detail_sequence: Option<u64>,
}

/// Entries of the header menu.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum MenuEntry {
    DesktopAlerts,
    About,
}

impl MenuAction for MenuEntry {
    type Message = Message;

    fn message(&self) -> Message {
        match self {
            MenuEntry::DesktopAlerts => Message::ToggleDesktopAlerts,
            MenuEntry::About => Message::ToggleAbout,
        }
    }
}

/// Formats a receipt time (Unix milliseconds) in the local time zone.
fn format_received(received_ms: i64) -> String {
    let seconds = (received_ms / 1000) as libc::time_t;
    let mut buffer = [0u8; 64];
    // SAFETY: `local` is zeroed storage `localtime_r` fills in, and `strftime`
    // writes at most `buffer.len()` bytes and returns the length it wrote.
    unsafe {
        let mut local: libc::tm = std::mem::zeroed();
        if libc::localtime_r(&seconds, &mut local).is_null() {
            return String::new();
        }
        let written = libc::strftime(
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            c"%Y-%m-%d %H:%M:%S %Z".as_ptr(),
            &local,
        );
        String::from_utf8_lossy(&buffer[..written]).into_owned()
    }
}

/// Opens a link from the About page in the default browser.
fn open_url(url: &str) {
    if let Err(error) = std::process::Command::new("xdg-open").arg(url).spawn() {
        eprintln!("could not open {url}: {error}");
    }
}

#[derive(Clone, Debug)]
/// UI messages: user actions and engine traffic.
pub enum Message {
    Engine(String),
    Pump(Incoming),
    Select(Device, bool),
    Save,
    Discard,
    SortBy(SortColumn),
    Connect(Device),
    Disconnect(Device),
    Monitor(Device, bool),
    FilterDevice(Option<String>),
    FilterKind(KindFilter),
    Search(String),
    DeviceSearch(String),
    SelectedOnly(bool),
    DesktopAlerts(bool),
    ToggleDesktopAlerts,
    ToggleAbout,
    OpenUrl(String),
    ShowDetails(u64),
    CloseDetails,
    ClearFeed,
    ToggleDevices,
}

impl App {
    /// The app scans for its whole lifetime: request it whenever the engine
    /// reports it idle. Retries are throttled so a failing adapter is not
    /// hammered.
    fn ensure_scanning(&mut self) {
        let due = self
            .last_scan_request
            .map_or(true, |at| at.elapsed() >= SCAN_RETRY_INTERVAL);
        if due && self.model.should_start_scan() {
            self.last_scan_request = Some(Instant::now());
            self.send(Command::Scan { enabled: true });
        }
    }

    /// Sends a command to the engine; a rejected command is only logged.
    fn send(&self, command: Command) {
        if let Some(engine) = ENGINE.get() {
            if !engine.send(&command.to_json()) {
                eprintln!("engine rejected command {command:?}");
            }
        }
    }
}

impl cosmic::Application for App {
    type Executor = cosmic::executor::Default;
    type Flags = Flags;
    type Message = Message;

    const APP_ID: &'static str = "io.github.cheebinhoh.DmnBluetoothCosmic";

    fn core(&self) -> &cosmic::Core {
        &self.core
    }

    fn core_mut(&mut self) -> &mut cosmic::Core {
        &mut self.core
    }

    fn init(core: cosmic::Core, flags: Flags) -> (Self, Task<cosmic::Action<Message>>) {
        // Demo data must never overwrite the user's real saved devices.
        let settings_path = if flags.demo {
            None
        } else {
            Settings::default_path()
        };
        let (settings, warning) = settings_path
            .as_deref()
            .map(Settings::load)
            .unwrap_or_default();

        let mut model = Model::default();
        model.notice = warning;
        let app = App {
            core,
            model,
            settings,
            settings_path,
            show_devices: true,
            device_search: String::new(),
            last_scan_request: None,
            notifier: Notifier::new(),
            about: About::default()
                .name("Dmn Bluetooth")
                .icon(widget::icon::from_name(Self::APP_ID))
                .version(env!("CARGO_PKG_VERSION"))
                .author("Chee Bin Hoh")
                .comments("Bluetooth device and ANCS notification monitor.")
                .copyright("© 2026 Chee Bin Hoh")
                .license("GPL-3.0-only")
                .license_url("https://github.com/cheebinhoh/Dmn?tab=GPL-3.0-1-ov-file")
                .developers([("Chee Bin Hoh", "https://github.com/cheebinhoh")])
                .links([
                    ("Repository", "https://github.com/cheebinhoh/Dmn"),
                    ("Issues", "https://github.com/cheebinhoh/Dmn/issues"),
                    ("Email", "mailto:CheeBin.Hoh@gmail.com"),
                ]),
            show_about: false,
            detail_sequence: None,
        };
        app.send(Command::Hello);
        app.send(app.settings.configure_command());
        app.send(Command::Refresh);
        (app, Task::none())
    }

    fn header_start(&self) -> Vec<Element<'_, Message>> {
        vec![
            widget::nav_bar_toggle()
                .active(self.show_devices)
                .on_toggle(Message::ToggleDevices)
                .into(),
            widget::text::heading("Dmn Bluetooth").into(),
        ]
    }

    /// Header-bar menu: desktop alerts toggle and About.
    fn header_end(&self) -> Vec<Element<'_, Message>> {
        let key_binds = HashMap::new();
        let menu = menu::bar(vec![menu::Tree::with_children(
            RcElementWrapper::new(Element::from(
                widget::button::icon(widget::icon::from_name("open-menu-symbolic"))
                    .padding(8)
                    .class(cosmic::theme::Button::MenuRoot),
            )),
            menu::items(
                &key_binds,
                vec![
                    menu::Item::CheckBox(
                        "Desktop alerts",
                        None,
                        self.settings.desktop_alerts,
                        MenuEntry::DesktopAlerts,
                    ),
                    menu::Item::Divider,
                    menu::Item::Button("About Dmn Bluetooth", None, MenuEntry::About),
                ],
            ),
        )])
        .item_height(ItemHeight::Dynamic(40))
        .item_width(ItemWidth::Uniform(240))
        .spacing(4.0);
        vec![menu.into()]
    }

    /// Detail dialog for one feed event.
    fn dialog(&self) -> Option<Element<'_, Message>> {
        let event = self.model.feed.get(self.detail_sequence?)?;
        let details = event.details(&format_received(event.received_ms));
        Some(
            widget::dialog()
                .title("Notification details")
                .control(widget::selectable_text::body(details))
                .primary_action(widget::button::standard("Close").on_press(Message::CloseDetails))
                .into(),
        )
    }

    fn context_drawer(&self) -> Option<ContextDrawer<'_, Message>> {
        self.show_about.then(|| {
            context_drawer::about(
                &self.about,
                |url| Message::OpenUrl(url.to_owned()),
                Message::ToggleAbout,
            )
        })
    }

    fn subscription(&self) -> Subscription<Message> {
        Subscription::run(|| {
            cosmic::iced::stream::channel(
                64,
                |mut emitter: futures::channel::mpsc::Sender<Message>| async move {
                    let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<String>();
                    if let Some(engine) = ENGINE.get().cloned() {
                        std::thread::spawn(move || loop {
                            if let Some(text) = engine.next_message(POLL_INTERVAL) {
                                let bye = text.contains("\"type\":\"bye\"");
                                if tx.send(text).is_err() || bye {
                                    break;
                                }
                            } else if tx.is_closed() {
                                break;
                            }
                        });
                    }
                    while let Some(text) = rx.recv().await {
                        if emitter.send(Message::Engine(text)).await.is_err() {
                            break;
                        }
                    }
                    futures::future::pending::<()>().await;
                },
            )
        })
    }

    fn update(&mut self, message: Message) -> Task<cosmic::Action<Message>> {
        match message {
            Message::Engine(text) => match parse_incoming(&text) {
                Ok(incoming) => return self.update(Message::Pump(incoming)),
                Err(error) => self.model.notice = Some(format!("Bad engine message: {error}")),
            },
            Message::Pump(incoming) => {
                if let Incoming::Event(event) = &incoming {
                    if self.settings.desktop_alerts {
                        if let Some(device) = alert_device(event) {
                            self.notifier.notify(&device);
                        }
                    }
                }
                if let Some(persist) = Model::apply(&mut self.model, incoming) {
                    self.settings.apply_persist(persist);
                    if let Some(path) = &self.settings_path {
                        if let Err(error) = self.settings.save(path) {
                            self.model.notice = Some(format!("Could not save settings: {error}"));
                        }
                    }
                }
                self.ensure_scanning();
            }
            Message::Select(device, selected) => self.send(Command::Select {
                identity: device.identity(),
                selected,
            }),
            Message::Save => self.send(Command::SaveSelection),
            Message::Discard => self.send(Command::DiscardSelection),
            Message::SortBy(column) => {
                let current = &self.model.snapshot.sort;
                let ascending = if current.column == column {
                    !current.ascending
                } else {
                    true
                };
                self.settings.sort = Sort { column, ascending };
                self.send(Command::SortBy(self.settings.sort.clone()));
            }
            Message::Connect(device) => self.send(Command::Connect(device.identity())),
            Message::Disconnect(device) => {
                self.notifier
                    .withdraw(&device_key(&device.adapter, &device.device));
                self.send(Command::Disconnect(device.identity()))
            }
            Message::Monitor(device, enabled) => {
                if !enabled {
                    self.notifier
                        .withdraw(&device_key(&device.adapter, &device.device));
                }
                self.send(Command::Monitor {
                    identity: device.identity(),
                    enabled,
                })
            }
            Message::FilterDevice(key) => self.model.filter.device = key,
            Message::FilterKind(kind) => self.model.filter.kind = kind,
            Message::Search(text) => self.model.filter.search = text,
            Message::DeviceSearch(text) => self.device_search = text,
            Message::SelectedOnly(enabled) => {
                self.settings.selected_only = enabled;
                if let Some(path) = &self.settings_path {
                    if let Err(error) = self.settings.save(path) {
                        self.model.notice = Some(format!("Could not save settings: {error}"));
                    }
                }
            }
            Message::DesktopAlerts(enabled) => {
                self.settings.desktop_alerts = enabled;
                if !enabled {
                    self.notifier.withdraw_all();
                }
                if let Some(path) = &self.settings_path {
                    if let Err(error) = self.settings.save(path) {
                        self.model.notice = Some(format!("Could not save settings: {error}"));
                    }
                }
            }
            Message::ToggleDesktopAlerts => {
                return self.update(Message::DesktopAlerts(!self.settings.desktop_alerts));
            }
            Message::ToggleAbout => {
                self.show_about = !self.show_about;
                self.set_show_context(self.show_about);
            }
            Message::OpenUrl(url) => open_url(&url),
            Message::ShowDetails(sequence) => self.detail_sequence = Some(sequence),
            Message::CloseDetails => self.detail_sequence = None,
            Message::ClearFeed => {
                self.notifier.withdraw_all();
                self.model.feed.clear();
                self.detail_sequence = None;
            }
            Message::ToggleDevices => self.show_devices = !self.show_devices,
        }
        Task::none()
    }

    fn view(&self) -> Element<'_, Message> {
        let spacing = cosmic::theme::spacing();
        let mut page = widget::column::with_capacity(4).spacing(spacing.space_s);

        if let Some(problem) = &self.model.incompatible {
            page = page.push(widget::text::body(problem.clone()));
        }
        page = page.push(self.toolbar());

        // Each pane scrolls independently so a long device list never moves
        // the notification feed, and vice versa.
        let devices = widget::scrollable(self.device_list()).height(Length::Fill);
        let feed = widget::scrollable(self.feed_view()).height(Length::Fill);
        let body: Element<_> = if !self.show_devices {
            feed.into()
        } else if self.core.is_condensed() {
            widget::column::with_children(vec![devices.into(), feed.into()])
                .spacing(spacing.space_m)
                .height(Length::Fill)
                .into()
        } else {
            widget::row::with_children(vec![
                widget::container(devices)
                    .width(Length::FillPortion(1))
                    .height(Length::Fill)
                    .into(),
                widget::container(feed)
                    .width(Length::FillPortion(1))
                    .height(Length::Fill)
                    .into(),
            ])
            .spacing(spacing.space_m)
            .height(Length::Fill)
            .into()
        };
        page = page.push(body);

        let status = self
            .model
            .notice
            .clone()
            .unwrap_or_else(|| self.model.snapshot.status.clone());
        page = page.push(widget::text::caption(status));

        widget::container(page)
            .padding(spacing.space_s)
            .width(Length::Fill)
            .height(Length::Fill)
            .into()
    }
}

impl App {
    fn toolbar(&self) -> Element<'_, Message> {
        let unsaved = self.model.snapshot.unsaved_changes;
        let selected_only = self.settings.selected_only;
        let search_text = self.device_search.clone();
        // The device search is half as wide as the device panel: the panel is
        // half the window when side by side, the whole window when stacked.
        // It stays visible, at the same width, while the panel is hidden.
        let share = if !self.core.is_condensed() { 0.25 } else { 0.5 };
        widget::responsive(move |size| {
            let spacing = cosmic::theme::spacing();
            let mut save = widget::button::suggested("Save");
            let mut discard = widget::button::standard("Discard");
            if unsaved {
                save = save.on_press(Message::Save);
                discard = discard.on_press(Message::Discard);
            }
            widget::row::with_capacity(4)
                .spacing(spacing.space_xs)
                .align_y(Alignment::Center)
                .push(
                    widget::search_input("Search devices", search_text.clone())
                        .on_input(Message::DeviceSearch)
                        .on_clear(Message::DeviceSearch(String::new()))
                        .width(Length::Fixed(size.width * share)),
                )
                .push(
                    widget::checkbox(selected_only)
                        .label("Selected only")
                        .on_toggle(Message::SelectedOnly),
                )
                .push(save)
                .push(discard)
                .into()
        })
        .width(Length::Fill)
        .height(Length::Fixed(TOOLBAR_HEIGHT))
        .into()
    }

    fn device_list<'a>(&'a self) -> Element<'a, Message> {
        let spacing = cosmic::theme::spacing();
        let heading_gap = scaled_gap(HEADING_GAP_SCALE);
        let section_gap = scaled_gap(SECTION_GAP_SCALE);
        let snapshot = &self.model.snapshot;
        let sort_label = |name: &str, column: SortColumn| {
            let mark = if snapshot.sort.column == column {
                if snapshot.sort.ascending {
                    " ▲"
                } else {
                    " ▼"
                }
            } else {
                ""
            };
            widget::button::text(format!("{name}{mark}")).on_press(Message::SortBy(column))
        };
        let top_heading = widget::row::with_children(vec![
            widget::text::title4("Previous selected devices").into(),
            widget::space::horizontal().into(),
            widget::text::caption("Sort by").into(),
            sort_label("Name", SortColumn::Name).into(),
            sort_label("Status", SortColumn::Status).into(),
        ])
        .spacing(spacing.space_xxs)
        .align_y(Alignment::Center);
        // Grouping rules live in `Model::device_groups` (unit tested).
        let selected_only = self.settings.selected_only;
        let (previous, nearby) = self.model.device_groups(selected_only, &self.device_search);
        let card_gap = card_gap();
        let group = |devices: Vec<&'a Device>, empty: &'static str| {
            let mut cards = widget::column::with_capacity(devices.len().max(1)).spacing(card_gap);
            if devices.is_empty() {
                cards = cards.push(widget::text::body(empty));
            }
            for device in devices {
                cards = cards.push(self.device_card(device));
            }
            cards
        };
        // Each heading sits closer to its own list than the two sections sit
        // to each other.
        let top = widget::column::with_capacity(2)
            .spacing(heading_gap)
            .push(top_heading)
            .push(group(
                previous,
                "No saved devices yet. Select a device and save to keep it here.",
            ));
        let bottom = widget::column::with_capacity(2)
            .spacing(heading_gap)
            .push(widget::text::title4("Nearby devices"))
            .push(group(
                nearby,
                if selected_only || !self.device_search.trim().is_empty() {
                    "No devices match. Clear the search or Selected only to see all."
                } else {
                    "Scanning for devices…"
                },
            ));
        widget::column::with_capacity(2)
            .spacing(section_gap)
            .push(top)
            .push(bottom)
            .into()
    }

    fn device_card<'a>(&'a self, device: &'a Device) -> Element<'a, Message> {
        let spacing = cosmic::theme::spacing();
        let name = if device.name.is_empty() {
            device.device.clone()
        } else {
            device.name.clone()
        };
        let mut lines = vec![
            widget::checkbox(device.selected)
                .label(name)
                .on_toggle({
                    let device = device.clone();
                    move |selected| Message::Select(device.clone(), selected)
                })
                .into(),
            widget::text::caption(format!("{} · {}", device.device, device.status)).into(),
        ];
        if device.monitor_state != "idle" {
            let detail = if device.monitor_detail.is_empty() {
                device.monitor_state.clone()
            } else {
                format!("{}: {}", device.monitor_state, device.monitor_detail)
            };
            lines.push(widget::text::caption(format!("Monitoring {detail}")).into());
        }

        let mut connect = widget::button::standard("Connect");
        if device.can_connect {
            connect = connect.on_press(Message::Connect(device.clone()));
        }
        let mut disconnect = widget::button::standard("Disconnect");
        if device.can_disconnect {
            disconnect = disconnect.on_press(Message::Disconnect(device.clone()));
        }
        let (monitor_label, monitor_enabled, monitor_available) = if device.can_stop_monitoring {
            ("Stop monitoring", false, true)
        } else {
            ("Start monitoring", true, device.can_start_monitoring)
        };
        let mut monitor = widget::button::suggested(monitor_label);
        if monitor_available {
            monitor = monitor.on_press(Message::Monitor(device.clone(), monitor_enabled));
        }
        let filter = widget::button::link("Show notifications").on_press(Message::FilterDevice(
            Some(device_key(&device.adapter, &device.device)),
        ));
        lines.push(
            widget::row::with_children(vec![
                connect.into(),
                disconnect.into(),
                monitor.into(),
                filter.into(),
            ])
            .spacing(spacing.space_xs)
            .into(),
        );
        widget::container(widget::column::with_children(lines).spacing(spacing.space_xxs))
            .padding(spacing.space_xs)
            .class(cosmic::theme::Container::Card)
            .width(Length::Fill)
            .into()
    }

    fn feed_view(&self) -> Element<'_, Message> {
        let spacing = cosmic::theme::spacing();
        let filter = &self.model.filter;
        let kind_button = |label: &'static str, kind: KindFilter| {
            let button = widget::button::text(label);
            if filter.kind == kind {
                button
            } else {
                button.on_press(Message::FilterKind(kind))
            }
        };
        let mut device_button = widget::button::text("All devices");
        if filter.device.is_some() {
            device_button = device_button.on_press(Message::FilterDevice(None));
        }

        let mut column = widget::column::with_capacity(8).spacing(spacing.space_xs);
        column = column.push(widget::text::title4("Notifications"));
        column = column.push(
            widget::row::with_children(vec![
                kind_button("All", KindFilter::All).into(),
                kind_button("Notifications", KindFilter::Notifications).into(),
                kind_button("Problems", KindFilter::Problems).into(),
                device_button.into(),
                widget::button::text("Clear")
                    .on_press(Message::ClearFeed)
                    .into(),
            ])
            .spacing(spacing.space_xxs),
        );
        column =
            column.push(widget::search_input("Search", &filter.search).on_input(Message::Search));

        let mut shown = 0;
        let mut rows = widget::column::with_capacity(16).spacing(card_gap());
        for event in self.model.feed.visible(filter).take(200) {
            shown += 1;
            rows = rows.push(self.event_row(event));
        }
        if shown == 0 {
            rows = rows.push(widget::text::body("Nothing to show."));
        }
        column = column.push(rows);
        column.into()
    }

    fn event_row<'a>(&'a self, event: &'a EventRecord) -> Element<'a, Message> {
        let mut lines: Vec<Element<'a, Message>> = vec![
            widget::selectable_text::body(event.headline()).into(),
            widget::text::caption(event.name.clone()).into(),
        ];
        if event.notification.is_some() {
            lines.push(
                widget::button::text("Details")
                    .on_press(Message::ShowDetails(event.sequence))
                    .into(),
            );
        }
        // Title and message are selectable (drag to highlight, right-click for
        // Copy / Select All); the tooltip shows the whole text.
        let summary = event.summary();
        if !summary.is_empty() {
            lines.insert(1, widget::selectable_text::body(summary.clone()).into());
        }
        let row = widget::container(widget::column::with_children(lines).width(Length::Fill))
            .padding(cosmic::theme::spacing().space_xs)
            .class(cosmic::theme::Container::Card)
            .width(Length::Fill);
        let full = if summary.is_empty() {
            event.headline()
        } else {
            format!("{}\n{}", event.headline(), summary)
        };
        widget::tooltip(
            row,
            widget::container(widget::text::body(full)).max_width(TOOLTIP_MAX_WIDTH),
            widget::tooltip::Position::Bottom,
        )
        .into()
    }
}
