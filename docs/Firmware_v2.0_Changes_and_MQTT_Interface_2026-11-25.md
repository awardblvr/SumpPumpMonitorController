# Firmware 2.0 - Changes and MQTT Interface (2026-11-25)

Builds on v1.1 (see `Code_Analysis_and_v1.1_Changes_2026-11-25.md`, whose findings and fixes are all still in) and carries out the
decisions in `HomeAssistant_Integration_Plan_2026-11-25.md`, except where this document says otherwise (it wins: the plan
assumed a read-only first version and the topic prefix `sump_monitor`).

**Status: written and compiled; NOT uploaded, not run on the device, never connected to your broker or Home Assistant.**
What was checked, and how, is in section 9.

## 1. What you asked for, and where it is

| Request | Done as |
|---|---|
| Version 2.0 | `FW_VERSION "2.0"`, shown on the idle screen and the boot screen |
| PubSubClient | PubSubClient 2.8 found in `~/ArduinoLocal/libraries`; used inside its own FreeRTOS task so the broker or WiFi can never stall pump control |
| Topic `Sump_pump` | All topics start with `Sump_pump/` (exact case; MQTT is case-sensitive) |
| Command: statistics dump of everything | `stats` -> full JSON on `Sump_pump/stats` (measurements, parameters, time, counters) |
| Command: turn off the running pump | `pump_off` |
| Command: forcibly turn on (max 5 min) | `pump_on` (default 300 s, optionally 5-300) |
| Command: disable the pump for 24 h | `disable` (24 h, optionally 1-24), `enable` to cancel |
| Screen response ("PUMP DISABLED" instead of "WATCHING", level still shown) | Done; also `FORCED ON (Ns)`, `STOPPED (Ns)`, a one-line response message, and the time left on a disable |
| Change trigger levels by command, save to flash if different | `set trigger=5.5 off=1.0 alarm=10` (any subset) |
| Push on error/problem | Yes, immediately (see below) |
| Report every 2 min when the water starts rising, back to normal when it settles, reset after 6 h | Done (section 5) |

**"Push on a problem - I guess it works that way now anyway?"** No: v1.0/v1.1 had no network reporting at all, so nothing was
pushed. In v2.0 a problem (high water, pump trouble, level sensor fault) is published the moment it appears or clears, as an event
on `Sump_pump/event` and as a state report; the retained `Sump_pump/status` goes `offline` by itself if the controller dies.

## 2. Topics

| Topic | Direction | Content | Retained |
|---|---|---|---|
| `Sump_pump/status` | out | `online` / `offline` (MQTT last will); `online` is re-sent every 60 s | yes |
| `Sump_pump/state` | out | JSON snapshot (about 500 bytes): level, pump state, alarms, disable, statistics, WiFi, levels | no |
| `Sump_pump/stats` | out | JSON dump (about 1.3 KB), **only** when the `stats` command is sent | no |
| `Sump_pump/event` | out | `{"event":"high_water","state":"raised"/"cleared",...}` pushed at once | no |
| `Sump_pump/response` | out | JSON reply to every command | no |
| `Sump_pump/cmd` | in | plain-text commands | **never retain** |
| `homeassistant/<type>/sump_pump/<id>/config` | out | Home Assistant discovery, 28 entities | yes |

The `stats` reply is not retained, so be subscribed to `Sump_pump/#` before you send the command.

## 3. Commands

Case-insensitive, whitespace or comma separated. Send them as plain text to `Sump_pump/cmd`, **without** the retain flag.

| Command | What it does | Refused when |
|---|---|---|
| `stats` | Publishes the full dump to `Sump_pump/stats` | never |
| `pump_off` (or `stop`) | Stops the running pump (auto, manual, forced or the 7 AM cycle), saves the run to the statistics, and **holds automatic restart for 60 s** (screen: `STOPPED (60s)`). Without that hold a high level would restart the pump within 0.1 s. Use `disable` for a longer stop | - (answers "pump was not running" if idle) |
| `pump_on [sec]` | Forced run, 5-300 s, default 300. Automatic control stands aside; Button 1 or `pump_off` stops it early. **Does not stop at a low water level**, so it can run the pump dry for the whole time. If the pump is already running, the whole continuous run (measured from when it started) is held to 300 s | pump is disabled; 7 AM maintenance cycle running; a forced run already active; pump already ran almost 5 min |
| `disable [hours]` | Stops the pump now and keeps automatic control (and the 7 AM cycle) off for 1-24 h, default 24. Screen: `PUMP DISABLED` plus time left. **Cancels itself if the high-water alarm becomes active** (`DISABLE_CANCELS_AT_ALARM`, section 7) | hours outside 1-24 |
| `enable` | Cancels a disable | - |
| `silence` | Mutes the alarm beeper (same as Button 2) | - |
| `set trigger=N off=N alarm=N` | Changes any subset of the three levels (inches). Validated together, **saved to flash only for values that differ**; takes effect at once | invalid combination (below) |
| `help` | Lists the commands in the response | - |

Bare `ON` / `OFF` are rejected on purpose, so a Home Assistant switch with default payloads can never run or stop the pump.

Level rules for `set` (checked on the final combination, so a partial `set` is also checked against the values it keeps):
`0.5 <= off <= 6.0`, `2.0 <= trigger <= 12.0`, `trigger >= off + 1.0`, `alarm >= trigger + 1.0`, `alarm <= 17.0`. The limits stop a
mistaken or stray `set` from short-cycling the pump on residual water. The high-water alarm clears 0.5 in below `alarm`. A bad flash value at
boot falls back to the factory 5.0 / 1.0 / 10.0. Two level changes that actually write flash must be at least 10 s apart (flash wear);
repeating the current values is free and writes nothing.

Examples (from the Mac, through the Pi's broker; commands as in the house system reference):

```
# watch everything (leave running in one window)
ssh awardblvr@192.168.1.12 "docker exec mosquitto sh -c 'timeout 120 mosquitto_sub -h localhost -v -t \"Sump_pump/#\"'"
# send commands (no -r!)
ssh awardblvr@192.168.1.12 "docker exec mosquitto mosquitto_pub -h localhost -t Sump_pump/cmd -m stats"
ssh awardblvr@192.168.1.12 "docker exec mosquitto mosquitto_pub -h localhost -t Sump_pump/cmd -m 'set trigger=5.5'"
ssh awardblvr@192.168.1.12 "docker exec mosquitto mosquitto_pub -h localhost -t Sump_pump/cmd -m 'pump_on 30'"
```

## 4. What the display shows

- Status line (size 2): `HIGH WATER!` / `SENSOR FAULT` / `PUMP TROUBLE` (red, `(mute)` when silenced) take priority, then
  `PUMP ON (Ns)`, then **`PUMP DISABLED`** (magenta) in place of `WATCHING`, then `STOPPED (Ns)` after `pump_off`,
  `COOLING (Ns)` after a timeout, else `WATCHING`. The measured level at the top keeps updating in every state.
- Forced run: red `FORCED ON (Ns)` countdown line. Manual button runs keep their `PUMP ON (N sec)` line.
- A one-line response from the last command (`CMD: pump DISABLED 24h`, `CMD refused: pump disabled`, ...) for 6 s, or while
  disabled, `Disabled - back on in 23h59m`.
- `MQTT ok` (green) / `MQTT --` (red) under the version, top right.

## 5. Reporting schedule

- **Normal:** a state report every 5 minutes.
- **On any change:** pump on/off, extension, pause, forced, disabled, alarm cause raised or cleared, reporting mode switch - sent
  immediately, but never more than one state message per 2 seconds.
- **Water rising = elevated:** the level has risen 0.5 in or more above its lowest value in the last 10 minutes, or any alarm cause
  exists. Reports then go out every **2 minutes**.
- **Back to normal:** pump idle, no alarm, level at or below 2.0 in and not rising, for 5 minutes in a row.
- **6-hour limit:** elevated reporting stops by itself after 6 hours. It cannot restart until a full calm period has been seen
  (otherwise a condition that never clears would re-enter instantly and the limit would mean nothing).
- A slow dry-weather fill (about 0.01 in/min) never triggers it; a wet-weather rise does. A pump disabled while the level rises
  also triggers it by the rising rule.
- All of the above is in `sump_logic.h` and covered by the PC tests (section 9). The 5 min / 2 min / 6 h / 0.5 in / 2.0 in values
  are constants at the top of `SumpMonitorController.ino`.

## 6. Home Assistant entities (28, created by discovery under the device "Sump Pump")

- Sensors (13): water level, pump state, runtime today, runtime 5 days, last run duration, last run time, pump disable remaining,
  reporting mode, WiFi signal, uptime, last reset reason, firmware, IP address. Sensors expire after 15 min of silence.
- Binary sensors (6): pump running, high water, pump trouble, level sensor fault, alarm, pump disabled.
- Buttons (6): stats dump, stop pump, **run pump 5 min**, disable pump 24 h, enable pump, silence alarm.
- Numbers (3, category config): trigger level, off level, alarm level (each sends `set ...`).

The buttons and numbers are real controls. Do not put "Run pump 5 min" or the number boxes on a family-facing dashboard; the entities
can simply be left off any card, or disabled in HA's entity settings. Predicted entity ids look like `sensor.sump_pump_water_level`.
`docs/HomeAssistant/sump_pump.package.draft.yaml` is the draft alert package (not deployed; the older `sump_monitor.*` draft is superseded).

## 7. Decisions I made that you may want to flip

| # | Decision | Why |
|---|---|---|
| 1 | The pump disable is **not** remembered across a reset or power cut; a reset re-enables automatic pumping | Fail-safe toward pumping. Also in the sketch comments. Making it persistent would need a valid clock after boot, and an unreachable network could leave a pump off silently |
| 2 | Button 1 on the unit still works while the pump is disabled | Someone standing at it is deliberate |
| 3 | `pump_on` is refused while disabled | A remote "run" must not override a disable somebody set to work on the pump |
| 4 | `pump_off` holds automatic restart for 60 s (the existing cool-down) | Otherwise it is a no-op while the level is above the trigger |
| 5 | Baseline report interval 5 min | You gave 2 min as the "rising" rate, so the normal rate had to be slower. Change `REPORT_BASELINE_MS` |
| 6 | Retained command defence: commands arriving within 1.5 s of subscribing are ignored, and a retained message on `Sump_pump/cmd` is wiped after every connect | A retained `pump_on` would otherwise replay on every reconnect |
| 7 | Added a `silence` command and `help` (not requested) | Needed for a Home Assistant "silence" button; trivial and harmless |
| 8 | Sensor fault still blocks automatic pump START (v1.1 policy, `SENSOR_FAULT_BLOCKS_AUTO_START`); a forced `pump_on` still works during a fault | You can always run it by hand |
| 9 | **A disabled pump re-enables itself the moment the high-water alarm becomes active** (`DISABLE_CANCELS_AT_ALARM 1`; set 0 for a disable that always runs its full time). The screen and an event say so | Found in review: an anonymous broker means a stray `disable` could otherwise leave a flooding sump unpumped for up to 24 h. This does override your explicit disable in that one case, so it is your call |
| 10 | Stricter `set` limits (trigger 2.0-12.0, off 0.5-6.0, trigger at least off + 1.0) and a 10 s minimum between flash-writing changes | Review: the looser limits allowed values that short-cycle the pump; unlimited writes wear the flash |
| 11 | "Back to normal" level for the reporting schedule is `max(2.0, off + 0.5)` instead of a fixed 2.0 | Review: with a raised OFF level the fixed value could never be reached |

## 8. Security, plainly

The house broker is **anonymous and open to every device on the LAN** (SysRef section 11, item 7). Anything on the network can now publish
`pump_on`, `disable` or `set` to `Sump_pump/cmd`. The limits are real but small: a forced run is at most 5 minutes, a disable
at most 24 hours (cleared by a reset, or by the high-water alarm), levels are validated, and nothing can be made permanent except the three level
values in flash. Before you rely on the commands, the sound fix is mosquitto authentication plus an ACL that lets only Home Assistant
and your own user publish to `Sump_pump/cmd`. The firmware is ready for it: uncomment `SECRET_MQTT_USER` / `SECRET_MQTT_PASS` in `secrets.h`
(template updated), no code change. I did not touch the broker.

## 9. What was and was not checked

| Check | Result |
|---|---|
| Compile, core 4.0.0, `--warnings all`, PubSubClient 2.8 | Clean, 0 warnings. 1,137,725 bytes (87% of the OTA slot, about 173 KB spare), RAM 96,940 bytes (29%) |
| Unit tests on a PC for `sump_logic.h` (command parsing, level validation, reporting schedule incl. 6 h limit, lockout, millis() wrap) | 140 checks pass (`temp_AI_files/SumpMonitorController/host_test/test_sump_logic.cpp`) |
| The real JSON builders and discovery table, extracted from the sketch and run on a PC with worst-case values | All 30 messages are valid JSON; state 494 B (buffer 1000), stats 1,359 B (buffer 1800), largest discovery payload 496 B (buffer 1400); MQTT buffer 2048; 28 unique ids |
| Independent read-only review of the v2.0 code | Done, section 10 |
| **On the device, against the broker, in Home Assistant** | **Nothing.** Never run |

## 10. Independent review of v2.0 (a separate agent read the code, with no hints about my conclusions)

Found and fixed (all compile-checked; none bench-tested):

| # | Finding | Fix |
|---|---|---|
| 1 | **High.** A retained or stray `Sump_pump/cmd` message was executed on every (re)connect: the "ignore leftovers" window started after discovery, but the broker delivers retained messages within about 30 ms of subscribing | Commands are now disarmed from before `subscribe()` until 1.5 s later, and the retained command is wiped at the moment of arming |
| 2 | **High for remote monitoring.** MQTT stopped reconnecting once uptime passed about 25 days (a signed time compare went negative) | Unsigned elapsed-time compare with a 5-60 s back-off, which is wrap-safe for the whole 49.7 days |
| 3 | A stale command banner reappeared on the display after about 25 days (same signed-compare cause) | The timestamp is cleared once it expires |
| 4 | Possible reconnect storm (28 discovery publishes per attempt) if a session keeps dropping | At least `backoff` between attempts, and discovery is re-sent at most once a minute |
| 5 | `pump_on` on top of a pump already running let the continuous run exceed 5 minutes (an automatic run at 239 s plus 300 s) | The whole run is held to 300 s from when it started |
| 6 | Design exposure on an open broker: `disable` could leave a flooding sump unpumped; extreme `set` values; unlimited flash writes | Decisions 9 and 10 in section 7 |
| 7 | `MQTT --` stayed red after a WiFi blip that did not drop the TCP session | Link state re-asserted when the session is found alive |
| 8 | Fixed 2.0 in "back to normal" unreachable after raising the OFF level | Decision 11 |
| 9 | Smaller: state-size guard checked the wrong buffer; commands queued during a failed OTA ran afterwards; no stack-use figure | Guard fixed; the queue is flushed at OTA start and on error; `mqtt_task_stack_free` added to the stats dump |
| - | Home Assistant restarts left state "unknown" for up to 5 min (state is not retained) | The controller now subscribes to `homeassistant/status` and re-sends discovery and state when Home Assistant announces it is online |

Reviewer's open points that I could not settle offline: how Home Assistant renders a JSON `null` through the templates (`level_in` is
null during a sensor fault, `last_run_utc` until the first run: may log a template error or show "unknown"); whether
the `mdi:water-pump` icons exist in your Home Assistant's icon set (cosmetic); the MQTT task's stack headroom (see `mqtt_task_stack_free`
in the first stats dump; 6,144 bytes were allocated); events are QoS 0, so one written into a half-dead socket can be lost (the next
state report still converges).

## 11. First bring-up checklist (after you upload v2.0)

1. Boot screen says `v2.0`; idle screen shows `ver 2.0` and `MQTT ok` within about a minute. If it stays `MQTT --`, check
   the broker (`docker ps` on the Pi) and that the controller's IP can reach `192.168.1.12:1883`.
2. Subscribe to `Sump_pump/#`: expect `status online`, 28 retained discovery messages, then a `state` message.
3. In Home Assistant: Settings > Devices > "Sump Pump" should appear with its entities.
4. Send `help`, then `stats`: a response on `Sump_pump/response`, a dump on `Sump_pump/stats`.
5. `set trigger=5.0` (same value): response says no change (flash not written). Then `set trigger=5.5` and back to `5.0`.
6. Pump idle: `pump_on 10`: pump runs 10 s (screen: FORCED ON), response ok. Press Button 1 during it: stops.
7. `disable 1`: screen shows PUMP DISABLED with the level still updating; `pump_on` is refused; `enable` clears it.
8. Rising water cannot be tested safely without water; confirm instead that `report_mode` is `baseline` and that state messages
   arrive every 5 minutes.
9. Unplug the controller's power for a moment: `Sump_pump/status` should flip to `offline` (last will) within about 45 s and
   Home Assistant should show the entities unavailable.
