// Copyright © 2026 Chee Bin HOH. All rights reserved.

//! Safe wrapper over the Dmn Bluetooth C ABI.
//!
//! The engine is pull-based: send JSON commands with [`Engine::send`] and
//! block on [`Engine::next_message`] from a dedicated thread.

use std::ffi::{c_char, c_int, CStr, CString};
use std::fmt;
use std::ptr::NonNull;
use std::time::Duration;

#[allow(non_camel_case_types)]
mod ffi {
    use super::{c_char, c_int};

    #[repr(C)]
    pub struct dmn_bt_engine {
        _private: [u8; 0],
    }

    extern "C" {
        pub fn dmn_bt_engine_create() -> *mut dmn_bt_engine;
        pub fn dmn_bt_engine_create_demo() -> *mut dmn_bt_engine;
        pub fn dmn_bt_engine_destroy(engine: *mut dmn_bt_engine);
        pub fn dmn_bt_engine_send(engine: *mut dmn_bt_engine, command: *const c_char) -> c_int;
        pub fn dmn_bt_engine_next_message(
            engine: *mut dmn_bt_engine,
            timeout_ms: c_int,
        ) -> *mut c_char;
        pub fn dmn_bt_string_free(text: *mut c_char);
        pub fn dmn_bt_version() -> *const c_char;
    }
}

/// Failure to create the engine.
#[derive(Debug)]
pub struct CreateError;

impl fmt::Display for CreateError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("the Dmn Bluetooth engine could not be created")
    }
}

impl std::error::Error for CreateError {}

/// Owned engine handle. Dropping it stops monitors and joins engine threads.
pub struct Engine {
    handle: NonNull<ffi::dmn_bt_engine>,
}

// The C ABI documents send/next_message as callable from any thread; only
// destruction must not race other calls, which `Drop` guarantees via ownership.
unsafe impl Send for Engine {}
unsafe impl Sync for Engine {}

impl Engine {
    /// Engine over BlueZ and ANCS.
    pub fn new() -> Result<Self, CreateError> {
        Self::from_raw(unsafe { ffi::dmn_bt_engine_create() })
    }

    /// Engine over synthetic devices; needs no Bluetooth hardware.
    pub fn new_demo() -> Result<Self, CreateError> {
        Self::from_raw(unsafe { ffi::dmn_bt_engine_create_demo() })
    }

    fn from_raw(raw: *mut ffi::dmn_bt_engine) -> Result<Self, CreateError> {
        NonNull::new(raw)
            .map(|handle| Self { handle })
            .ok_or(CreateError)
    }

    /// Queues a JSON command; returns `false` when it was rejected.
    pub fn send(&self, command_json: &str) -> bool {
        let Ok(command) = CString::new(command_json) else {
            return false;
        };
        unsafe { ffi::dmn_bt_engine_send(self.handle.as_ptr(), command.as_ptr()) == 0 }
    }

    /// Waits for the next JSON message; `None` on timeout or once closed.
    pub fn next_message(&self, timeout: Duration) -> Option<String> {
        let millis = c_int::try_from(timeout.as_millis()).unwrap_or(c_int::MAX);
        let raw = unsafe { ffi::dmn_bt_engine_next_message(self.handle.as_ptr(), millis) };
        if raw.is_null() {
            return None;
        }
        let text = unsafe { CStr::from_ptr(raw) }
            .to_string_lossy()
            .into_owned();
        unsafe { ffi::dmn_bt_string_free(raw) };
        Some(text)
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        unsafe { ffi::dmn_bt_engine_destroy(self.handle.as_ptr()) };
    }
}

/// Version of the linked C library's protocol implementation.
pub fn version() -> String {
    unsafe { CStr::from_ptr(ffi::dmn_bt_version()) }
        .to_string_lossy()
        .into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    const WAIT: Duration = Duration::from_secs(10);

    fn next_of_type(engine: &Engine, wanted: &str) -> String {
        let pattern = format!("\"type\":\"{wanted}\"");
        for _ in 0..200 {
            if let Some(message) = engine.next_message(Duration::from_millis(50)) {
                if message.contains(&pattern) {
                    return message;
                }
            }
        }
        panic!("no {wanted} message within the deadline");
    }

    #[test]
    fn reports_protocol_version() {
        assert_eq!(version(), "1");
    }

    #[test]
    fn demo_engine_reports_ready_and_inventory_through_the_c_library() {
        let engine = Engine::new_demo().expect("demo engine");
        let ready = engine.next_message(WAIT).expect("ready message");
        assert!(ready.contains("\"type\":\"ready\""), "{ready}");
        assert!(ready.contains("\"backend\":\"demo\""), "{ready}");

        let state = next_of_type(&engine, "state");
        assert!(state.contains("Demo iPhone"), "{state}");
    }

    #[test]
    fn rejects_bad_commands_and_survives_drop_with_work_in_flight() {
        let engine = Engine::new_demo().expect("demo engine");
        assert!(!engine.send("not json"));
        assert!(!engine.send("bad \0 nul"));
        assert!(engine.send(r#"{"type":"scan","enabled":true}"#));
        drop(engine);
    }
}
