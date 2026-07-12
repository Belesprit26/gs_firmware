#pragma once

/// Background OTA (over-the-air update) task.
///
/// Responsibilities:
///   1. Confirm the running image is healthy and cancel the pending
///      rollback (esp_ota_mark_app_valid_cancel_rollback), so the
///      bootloader keeps the new firmware instead of reverting.
///   2. Periodically read the update manifest from RTDB
///      (/firmware/latest) and, if a strictly newer version is
///      published, download + validate it via esp_https_ota and reboot
///      at a relay-safe moment.
///
/// Requires WiFi + Firebase auth; waits internally until both are
/// ready, so it is safe to start unconditionally from app_main().
/// Create with xTaskCreate (~12 KB stack, low priority); never returns.
/// Pass NULL as the task parameter.
void ota_task(void *param);
