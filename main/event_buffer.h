#pragma once

#include <stdint.h>
#include <stdbool.h>

// ── Event types ──────────────────────────────────────────────────

#define EVT_MAX_TEMP_OFF    0x01   // temp >= max → auto-OFF
#define EVT_MIN_TEMP_ON     0x02   // temp <= min, auto-reheat → auto-ON
#define EVT_MIN_TEMP_ALERT  0x03   // temp <= min, no auto-reheat → alert only
#define EVT_SENSOR_FAIL     0x04   // temperature sensor offline
#define EVT_SENSOR_RECOVER  0x05   // temperature sensor back online
#define EVT_MAX_ON_TIMEOUT  0x06   // relay forced OFF after max-on limit
#define EVT_TYPE_COUNT      6

// ── Buffer sizes ─────────────────────────────────────────────────

#define EVENT_BUF_MAX   20     // last 20 events (rare, ~few/day)
#define TELEM_BUF_MAX   100    // ~4 hours at 2.5 min intervals

// ── Structures ───────────────────────────────────────────────────

/// A single buffered event (6 bytes on wire).
typedef struct {
    uint8_t  type;       // EVT_* constant
    uint8_t  temp;       // integer °C at time of event
    uint32_t timestamp;  // Unix timestamp (UTC)
} buffered_event_t;

/// A single buffered telemetry snapshot (10 bytes on wire).
typedef struct {
    int16_t  temp_raw;      // °C × 100
    uint8_t  relay_on;
    uint8_t  min_temp;
    uint8_t  max_temp;
    uint8_t  auto_reheat;
    uint32_t timestamp;     // Unix timestamp (UTC)
} buffered_telemetry_t;

// ── API ──────────────────────────────────────────────────────────

/// Initialise the buffer mutex.  Call once from app_main.
void event_buffer_init(void);

/// Push an event into the ring buffer (thread-safe).
void event_buffer_push_event(uint8_t type, uint8_t temp);

/// Copy up to max_count buffered events into *out (oldest first).
/// Returns the actual number copied.
uint8_t event_buffer_get_events(buffered_event_t *out, uint8_t max_count);

/// Push a telemetry snapshot into the ring buffer (thread-safe).
void event_buffer_push_telemetry(int16_t temp_raw, uint8_t relay,
                                  uint8_t min_t, uint8_t max_t,
                                  uint8_t auto_rh);

/// Copy up to max_count buffered telemetry entries into *out (oldest first).
/// Returns the actual number copied.
uint8_t event_buffer_get_telemetry(buffered_telemetry_t *out, uint8_t max_count);

/// Clear both event and telemetry buffers (called after app ack).
void event_buffer_clear(void);
