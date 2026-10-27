/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluetooth-cabi.h
 * @brief C ABI over the GUI-independent Bluetooth monitor engine.
 *
 * The interface is pull-based: a frontend sends JSON commands and blocks on
 * dmn_bt_engine_next_message() in a thread of its own, so the library never
 * calls back into the frontend. Every function catches all C++ exceptions.
 * Message formats are described in docs/specs/dmn-bluetooth-cosmic-spec.md.
 */

#ifndef DMN_BLUETOOTH_CABI_H_
#define DMN_BLUETOOTH_CABI_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dmn_bt_engine dmn_bt_engine;

/** Creates an engine over BlueZ and ANCS; NULL on failure. */
dmn_bt_engine *dmn_bt_engine_create(void);

/** Creates an engine over synthetic devices; needs no Bluetooth hardware. */
dmn_bt_engine *dmn_bt_engine_create_demo(void);

/**
 * Stops monitors, joins all worker threads and frees the engine. Must not be
 * called concurrently with other calls on the same handle. NULL is ignored.
 */
void dmn_bt_engine_destroy(dmn_bt_engine *engine);

/**
 * Queues one UTF-8 JSON command object.
 * Returns 0 when accepted, non-zero for bad input or a closed engine.
 */
int dmn_bt_engine_send(dmn_bt_engine *engine, const char *command_json);

/**
 * Waits up to timeout_ms for the next JSON message. Returns NULL on timeout or
 * once the engine has closed and drained. Free the result with
 * dmn_bt_string_free().
 */
char *dmn_bt_engine_next_message(dmn_bt_engine *engine, int timeout_ms);

/** Frees a string returned by this library. NULL is ignored. */
void dmn_bt_string_free(char *text);

/** Static NUL-terminated version string. */
const char *dmn_bt_version(void);

#ifdef __cplusplus
}
#endif

#endif /* DMN_BLUETOOTH_CABI_H_ */
