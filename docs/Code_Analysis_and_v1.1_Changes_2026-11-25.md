# Code Analysis and Firmware v1.1 Changes (2026-11-25)

Follow-up to `Lessons_Learned_OTA_and_Version_Display_2026-11-25.md`. This session did three things: (1) checked what can be
checked about running the sketch on ESP32 core 4.0.0, (2) read the whole sketch for logic faults and fixed what could be
fixed safely, (3) prepared the Home Assistant work (see `HomeAssistant_Integration_Plan_2026-11-25.md`).

**Status: v1.1 is edited and compiles. It has NOT been uploaded or run on the device.** Nothing here was tested on
hardware. Every behaviour claim below is from reading the code, the Arduino-ESP32 4.0.0 core sources, and the ADS1X15 library
source, plus compile checks. Section 6 is the checklist for proving it on the bench.

## 1. Core 4.0.0 check (task 1)

| Check | Result |
|---|---|
| v1.0 sketch compiled on core 4.0.0, `--warnings all` | Clean, 0 warnings. 1,106,417 bytes (84% of the 1,310,720 OTA slot), 84,412 bytes RAM (25%) |
| v1.1 sketch, same flags (after the review fixes) | Clean, 0 warnings. 1,106,633 bytes (84%), 84,412 bytes RAM |
| v1.1 with the bench-only `ENABLE_DUMMY_SENSOR 1` | Clean. 1,106,993 bytes |
| Independent read-only review of the diff (a separate agent, no hints about my conclusions) | Found F17-F21 and the sensor-fault policy gap; all fixed or documented here. It also checked the day-rollover arithmetic on a host program against `gmtime` for every date 1970-2105 (0 mismatches). Still a reading, not a bench test |
| APIs used by v1.1 exist in 4.0.0 | `enableLoopWDT/disableLoopWDT/feedLoopWDT`, `configTzTime`, `WiFi.setSleep/setHostname/setAutoReconnect/persistent`, `esp_reset_reason` all present in the core sources and compile |
| `WiFi.begin()` is non-blocking in 4.0.0 | Confirmed in `libraries/WiFi/src/STA.cpp` (`esp_wifi_connect()` returns at once). The v1.0 code had polled status after it anyway |
| Task watchdog config in 4.0.0 | `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`, panic-on-timeout on, idle task NOT watched, and `loopTaskWDTEnabled = false` at start. So in v1.0 a hung `loop()` was never reset |
| ADC library behaviour | `readADC_SingleEnded()` ends in an unbounded `while (!conversionComplete());` (see finding F1) |
| Only core installed | 4.0.0. The 3.3.10 build the IDE replaced is gone, so there is no side-by-side comparison; a rollback means Boards Manager |
| Behaviour on the device | **Not verified from here.** Use section 6 |

## 2. Findings fixed in v1.1

Severity is my judgement of the consequence if it happened, not how likely it is.

| # | Finding | Severity | Fix in v1.1 |
|---|---|---|---|
| F1 | If the ADS1115 stops answering on I2C mid-run (loose wire, relay EMI), the library spins forever inside `loop()`. A running pump stays on, the display freezes, and nothing resets because the loop-task watchdog is off | High | Probe the bus first, bounded 30 ms timeout per conversion, any failure marks the ADC missing and retries `begin()` (with an I2C bus restart) every 10 s. `enableLoopWDT()` at the end of `setup()` as the backstop: a stalled loop resets the chip and drops the relay |
| F2 | Missing ADS1115 at boot switched to **random simulated water levels (1-7 in) that drive the real pump**; a failed read after boot cannot be seen at all | High | Dummy mode now needs `ENABLE_DUMMY_SENSOR 1` (default 0). A failed read keeps the last good level for about 10 s, then raises SENSOR FAULT: red alarm, beeper, "Level: ERR", `ADC:FAULT`. While faulted, automatic control **will not start** the pump from the frozen level (`SENSOR_FAULT_BLOCKS_AUTO_START`, see section 4); a run already in progress still ends at the 240 s timeout |
| F3 | `attemptNTPSync()` blocked `loop()` for up to ~17 s (15 s WiFi wait + 2 s) every 2 min, then every 15 min, while the time was unsynced. The 6:45 AM resync blocked 2 s. During that time pump timeouts, the manual timer and level monitoring all stood still | High | Network code is now `networkTick()`: nothing waits. WiFi reconnect is re-issued every 30 s, SNTP is started once per WiFi connection |
| F4 | Boot could take 20-25 s before pump control started (WiFi wait 15 s, NTP 2 s, boot screen 3 s), longest after a power cut when the router is also booting | Medium | WiFi wait at boot is cosmetic and capped at 5 s; pump relay is driven LOW as the very first line of `setup()` |
| F5 | Daily statistics rolled over only inside `setup()`. "Today" kept accumulating after midnight until the next reboot, and the 5-day total was not a 5-day total | Medium | `rolloverDailyStats()` runs from `loop()`. It shifts by the real number of elapsed days (up to 5), so a controller that was off for days is also handled |
| F6 | Alarm-reset button did nothing while the level stayed above 10 in: it cleared `alarm_active`, and the high-water check set it again on the next pass | Medium | Button 2 now **mutes the beeper** and leaves the alarm showing. It re-sounds if a different alarm cause appears, or after 30 min if still active, and re-arms when everything clears |
| F7 | If WiFi was down at boot, `ArduinoOTA.begin()` was never called, so OTA stayed dead until a reboot even after WiFi came up | Medium | OTA callbacks set up in `setup()`, `ArduinoOTA.begin()` runs from `networkTick()` the first time WiFi is up |
| F8 | Fixed UTC-8 offset: the clock and the "7:00 AM" maintenance cycle were an hour off from March to November, and day rollover happened at 1 AM PDT | Medium | `configTzTime("PST8PDT,M3.2.0,M11.1.0", ...)` switches automatically. **Behaviour change:** in summer the cycle now runs at 7:00 PDT, which is one hour earlier on the wall clock than v1.0 did it |
| F9 | Maintenance could start while the pump was already running (auto, manual or cooling down). It then took the run out of the statistics, and its `stopPump()` could cut a real pump-down short | Low-Med | One decision per day; skipped if the pump is on, cooling down, in manual mode or OTA |
| F10 | Back-to-back 4-minute timeouts with water still high repeated forever with no alert | Medium | After 3 in a row: PUMP TROUBLE latch (alarm + display). Clears when the level reaches `OFF_INCHES` or a normal run completes |
| F11 | Single raw ADC sample per reading: one EMI spike from relay switching could trigger or stop the pump | Low-Med | Median of 5 conversions per reading (about 45 ms every 2 s) |
| F12 | Stale `pump_extension_active` flag after a manual or maintenance stop | Low | Cleared inside `stopPump()` |
| F13 | WiFi modem power-save on (core default). Likely contributor to the 850 ms ping outliers and flaky OTA at -72 dBm | Low-Med | `WiFi.setSleep(false)`, `setAutoReconnect(true)`, `persistent(false)`, DHCP hostname set to `SumpMonitor` |
| F14 | Dead and dangerous code: `calculateAverage()` read 20 entries from a 1-entry array (never called) | Low | Removed with the `readings[]` buffer |
| F15 | `String` concatenation every second on the display (heap churn on a 24/7 device); OTA progress divided by `total/100` | Low | `snprintf` into a char buffer; guard added |
| F16 | A reset caused by watchdog, brownout or crash looked like a normal power-up | Info | Boot screen shows "Reset: <reason>" in yellow when it was not a plain power-on |
| F17 | *(found by the independent review)* A failed OTA that began during the 5 s pump-extension window left `pump_extension_active` set with the pump forced off, and `stopPump()` could not clear it: automatic pumping stayed dead until a reset | High (rare trigger) | OTA `onStart` clears the extension and pause flags, and `stopPump()` clears the extension flag even when the pump is already off |
| F18 | *(review)* A wrong OTA password runs PBKDF2 plus the 3 s delay in `onError` with the watchdog still on (`onStart` never ran): about 4-5 s against the 5 s limit, so a watchdog reset was possible | Medium | `feedLoopWDT()` first thing in `onError` |
| F19 | *(review)* `WiFi.setHostname()` and `WiFi.persistent(false)` were called after `WiFi.mode()`, where the core ignores them | Low | Moved before `WiFi.mode()` |
| F20 | *(review, pre-existing)* A Button 1 press during the 7 AM run left `manual_pump_active` set after maintenance stopped the pump, blocking automatic control for the rest of the manual timer (20-160 s) | Low-Med | A manual flag with the pump off is now cleared at once |
| F21 | *(review, pre-existing)* The boot log printed 19 lines of 8 px on a 135 px screen, so "READY!" was clipped | Cosmetic | Blank spacer lines removed (15 lines, 16 with a sensor fault) |

Display changes you will notice: status line now says `SENSOR FAULT`, `HIGH WATER!`, `PUMP TROUBLE` (with `(mute)` when the beeper
is muted) instead of the generic `ALARM!`; `COOLING (Ns)` during the 60 s pause; if an alarm is showing and the pump is also running,
the pump gets its own line underneath.

Thresholds are unchanged: `TRIGGER_INCHES` 5.0, `OFF_INCHES` 1.0, 5 s extension, 240 s timeout, 60 s pause, alarm at 10 in
(clear at 9.5 in), maintenance 7:00 at 0.5 in to 0.25 in plus 3 s, max 15 s.

## 3. Found but NOT changed (needs your decision)

| # | Item | Why I left it |
|---|---|---|
| D1 | The 60 s cool-down after a timeout still applies when the level is above the 10 in alarm. A flooded sump gets a one-minute pump pause | It protects the motor and the old context file says not to touch timeout protection. I would skip the pause above the alarm level, but that is your risk call |
| D2 | K1 and K2 relays are never driven (`relay1_on` and `relay2_on` are never set; the display always shows K1:OFF K2:OFF). The OmniPro already has zone 87 "Lawn Sump alarm condition" with macros 124/125 that announce "front lawn sump pump failure" and send a text | Driving K1 sends real announcements and texts, and I have not confirmed the wiring polarity. Needs your OK on which zone K1/K2 land on |
| D3 | Long press (4 s) stops the pump only when it was started manually. The old context file says "emergency OFF at any time" | Doc and code disagree; stopping an automatic run just restarts it on the next pass anyway |
| D4 | Pressing Button 1 while an automatic run is in progress starts a 20 s manual timer. When it expires the pump stops, then automatic mode restarts it at once if the level is still above 5 in (one extra relay cycle) | Harmless but untidy; changing it alters button semantics |
| D5 | `fillScreen()` every second makes the display flicker | Cosmetic. A full-frame canvas needs about 65 KB of contiguous RAM; I did not want to add that before the network layer |
| D6 | Default OTA password is still in `secrets.h` | Needs a re-upload after changing; your call when |
| D7 | Button 2 now mutes a live alarm on a single sampled press (the loop samples every ~100 ms). In v1.0 a noise glitch could not silence anything, because the alarm re-armed at once. Worst case now is a 30 min mute | Accepted; requiring two consecutive samples would be a one-line change |

## 4. Cannot be fixed in software

- **An unplugged or broken sensor wire reads as 0.0 in, exactly like an empty sump.** The 0-10 V sensor has no live-zero, and the
  20k/10k divider pulls an open input to ground. A 4-20 mA sensor or a float switch as an independent second sensor is the
  real answer.
- **A single level sensor is a single point of failure for the whole control path.** The F2 sensor-fault alarm only covers the
  "chip stopped answering" case, not a sensor that answers wrongly. And when it does fault, the policy is a trade-off between two
  bad outcomes: with `SENSOR_FAULT_BLOCKS_AUTO_START 1` (the v1.1 default) a flood that begins during a fault is not pumped (the alarm
  sounds, and the manual button still works); with 0 the pump keeps acting on a frozen level and can run dry on and off at about
  80% duty (240 s on, 60 s pause) until the sensor returns. I chose 1 because the dry-run case is silent and indefinite while the
  flood case at least alarms. It is your call; the constant is at the top of the sketch.

## 5. Hypothesis for the leftover curiosity (task 5)

Python from the Mac gets `EHOSTUNREACH` ("No route to host") for this one device while ping and the IDE work. macOS returns that
error when ARP resolution fails. An ESP32 in WiFi modem-sleep at -72 dBm often misses ARP requests; ping retries and eventually
succeeds, a single UDP `sendto` does not. **Unproven.** v1.1 turns modem-sleep off, which gives a cheap test: after uploading
v1.1, retry `espota.py`; and right after any failure run `arp -n 192.168.1.62` (an `(incomplete)` entry supports the theory).

## 6. Bench checklist for v1.1 (about 15 minutes, then wait a day)

`FW_VERSION` is already set to 1.1 in the sketch. Upload from the IDE as before (Partition Scheme must still be "Default 4MB with spiffs").

1. Boot screen shows `Sump Monitor v1.1` and a `Reset:` line; idle screen shows `ver 1.1`. Boot to normal screen takes under about 12 s.
2. Level matches what you expect, NeoPixels animate, clock is correct for local time, K1/K2 lines show OFF.
3. RSSI line: note it; compare to the -72 dBm from before (modem-sleep off may change it slightly).
4. Press Button 1 once: pump runs 20 s, red countdown shows, then stops. Statistics line updates.
5. With the pump idle and water low, unplug the ADS1115 STEMMA QT cable at the ADS end: within about 10 s expect `SENSOR FAULT`
   (red), beeper, `Level: ERR`, `ADC:FAULT`. Press Button 2: beeper mutes, text stays. Plug it back: about 6 s later the fault clears.
6. Next morning: "Today" has reset at midnight and yesterday's runtime moved into the 5-day total; the 7:00 AM cycle ran at 7:00 local.
7. Try `espota.py` from the terminal (section 5).
8. Anything odd on the 4.0.0 core: Boards Manager, esp32 3.3.10, upload again. v1.1 should also build on 3.x, because every
   core API it adds (`enableLoopWDT`, `configTzTime`, `WiFi.setSleep`, `esp_reset_reason`) exists there, but I could not compile
   it against 3.x because 3.3.10 is no longer installed.

If the boot screen ever shows `Reset: WATCHDOG` or `CRASH`, note what the device was doing and tell me; that is the new
safety net working, and it is information v1.0 would have hidden.

9. **Do a second OTA upload** (for example change `FW_VERSION` to "1.1.1") before relying on v1.1. The first upload to v1.1 is
   carried out by the old v1.0 code, so it cannot show whether the new watchdog interferes with OTA; the second one can.
10. Optional: start an upload with a deliberately wrong OTA password. Expected: "OTA ERROR / Auth Failed" on screen, and **no**
    `Reset: WATCHDOG` on the next boot (this checks F18).

## 7. Risks in v1.1 that I could not rule out

- **False watchdog resets.** The watchdog resets the chip if `loop()` goes 5 s without finishing a pass. I checked every blocking
  call I could find in `loop()` (button hold wait is fed, stats-reset flash is under 1 s, the ADC read is bounded to about 200 ms
  worst case, flash writes are milliseconds) and OTA turns the watchdog off in `onStart`. In 4.0.0's `ArduinoOTA` the flash
  `begin()` runs before `onStart` but does not pre-erase the partition (erase is per 64 KB block while writing), so that window
  is short. I did not find a path that stalls, but only a bench run proves it. A false reset shows up as `Reset: WATCHDOG` on the
  boot screen; the pump relay is already off at that point.
- **Timezone switch.** The cycle at 7:00 will shift an hour on the wall clock in summer compared with v1.0 (item F8).
- **Sensor-fault policy.** On a sensor fault the controller raises the alarm and will not start the pump automatically from the
  frozen level (section 4). A real flood during a fault is therefore not pumped until the sensor returns or you press Button 1.
