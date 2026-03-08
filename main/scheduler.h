#pragma once

/// FreeRTOS task: checks ON-only timer schedules every 60 s and turns
/// the relay ON when the current time matches a timer's scheduled hour.
///
/// Timers are wake-up alarms — they only turn the geyser ON.
/// The thermostat (temp_max) is responsible for turning it OFF.
///
/// The task only acts when time_sync reports a valid clock source
/// (NTP via WiFi or BLE phone sync).  Without a valid time, the
/// scheduler is silently paused.
void scheduler_task(void *param);
