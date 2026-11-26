# Home Assistant Integration Plan - Sump Monitor (2026-11-25)

Companion to `Code_Analysis_and_v1.1_Changes_2026-11-25.md`. This is a plan and a decision, not an implementation:
**no firmware networking code for Home Assistant (HA) has been written yet, and nothing was changed on the HA side.** The
source for every claim about the house system is the reference set in
`~/Documents/Documents - AW_M3_MB_Pro/Projects/HomeAssist_OmniLinkBridge_MQTT/Reference/`
(mainly `WardHouse_HomeAutomation_SYSTEM_REFERENCE_20261125_124947.md`, cited below as "SysRef §n", and
`WardHouse_OmniPro_Config_Explained.md`). I did not log in to the HA box, the Pi, or the broker.

## 1. Decision

**Publish straight from the sump firmware to the existing mosquitto broker using MQTT discovery, read-only in the first
version. Keep the hardwired HAI path (K1/K2 into an OmniPro zone) as the independent fail-safe alarm.** The two are
complementary: MQTT gives level, runtime and diagnostics but depends on WiFi, the broker and HA being up; the hardwired
zone carries one bit but works with all of that down.

## 2. What the reference docs establish

| Fact | Source |
|---|---|
| One anonymous mosquitto broker, `192.168.1.12:1883`, no auth, no ACL, TLS off, retained messages persist across restarts | SysRef §1, §3 |
| HA (HAOS, Core 2026.11.2) is at `192.168.1.13` and its MQTT integration points at that broker with no credentials | SysRef §6.3 |
| Discovery prefix is `homeassistant`; there are already 573 retained discovery configs | SysRef §3.1, §5.4 |
| The newest hand-built device, Front of Garage IO, was integrated by "MQTT discovery ONLY": a per-device topic prefix (`front_driveway`), retained `<prefix>/status` = `online`/`offline` as birth and will, entities created by discovery with nothing hand-added to HA | SysRef §8.8 |
| `netmon` uses one retained JSON `netmon/state` plus `netmon/availability` as the last will | SysRef §7.2 |
| The sump controller is already in the router as a DHCP reservation, `.62`, MAC `7c:df:a1:94:c9:54`, and is listed as "not in HA" | SysRef §2.5, §8.6 |
| Gotcha: a retained `offline` from a ghost session blanked a whole card for about 29 h; fix was a **stable client id** plus **re-publishing `online` on every heartbeat** | SysRef §5.5(3), §7.3 |
| Gotcha: retained state can outlive reality; open item: `expire_after` not set on netmon sensors, so a silently dead publisher shows stale values | SysRef §5.5(2), §11(2) |
| The broker is wide open to anything on the LAN and is "the single point of compromise for OmniPro control" | SysRef §11(7) |
| HA logs "Name has no usable address" for `homeauto.local` once at each start-up, then connects | SysRef §6.2 (2026-11-25 entry) |
| The OmniPro already has a sump alarm path: zone 023 "Lawn Sump water flow", zone 087 "Lawn Sump alarm condition", macro 124 (when zone 087 is NOT READY: 4-hour flag, announce "FRONT LAWN SUMP PUMP FAILURE") and macro 125 (send the text) | `WardHouse_OmniPro_Config_Explained.md` §9.15 and the macro listing |
| Notifications in HA go through `script.house_notify` (the SMS service was retired 2026-11-25) | SysRef §11(14) |

## 3. Options considered

| Option | Verdict |
|---|---|
| **A. Firmware publishes MQTT + discovery to the existing broker** | **Chosen.** Same pattern as netmon and Front of Garage IO. Entities appear in HA by themselves. Push, not polling. No change to the pump logic's structure |
| B. Re-implement in ESPHome | Rejected. About 1,500 lines of tuned pump/alarm logic would have to be re-expressed in YAML and lambdas, with no way to test it on the real pump here; it also replaces the IDE-OTA workflow that was just made to work. ESPHome is a fair choice for a *new* simple device, not for porting this one |
| C. Web server on the ESP32, HA polls it with a REST sensor | Rejected as the main path. Adds a listening HTTP server to a safety device and polling latency; a tiny read-only `/status` page may still be worth adding later for debugging |
| D. Hardwired K1/K2 into an OmniPro zone only | Kept as the **fail-safe**, not the monitor. One bit per relay, no level or runtime, and K1/K2 are not driven by the firmware yet (see analysis doc D2) |
| E. Both A and D | **Recommended end state.** Different failure modes, so they cover each other |

## 4. Design

### 4.1 Topics (prefix `sump_monitor`, matching `front_driveway` / `netmon`)

| Topic | Content | Retained |
|---|---|---|
| `sump_monitor/status` | `online` / `offline`. Last will = `offline`; re-published `online` every 60 s (SysRef §7.3 lesson) | yes |
| `sump_monitor/state` | One JSON snapshot every 10 s and immediately on any pump/alarm change | **no** (so a dead controller cannot leave a stale level behind) |
| `homeassistant/<component>/sump_monitor/<object>/config` | Discovery payloads, published on every (re)connect | yes |
| `sump_monitor/cmd/...` | Not in version 1 (see 4.5) | - |

Client id fixed at `sump_monitor`, keep-alive 30 s. Every sensor carries `availability_topic: sump_monitor/status` and
`expire_after: 60`, which also closes the "silently dead publisher" gap noted in SysRef §11(2).

### 4.2 State payload (about 330 bytes)

```json
{"level_in":3.4,"pump":"OFF","pump_state":"idle","high_water":false,"pump_trouble":false,"sensor_fault":false,
 "alarm":false,"muted":false,"today_s":412,"five_day_s":1980,"last_run_s":38,"last_run_utc":"2026-11-25T14:02:11Z",
 "rssi":-71,"uptime_s":86400,"fw":"1.1","ip":"192.168.1.62","reset":"power-on"}
```

`level_in` is `null` while the sensor is faulted. Every field already exists in firmware v1.1 (`pumpStateName()`,
`boot_reason`, the alarm cause flags, the statistics arrays), which is why v1.1 was structured the way it was.

### 4.3 Entities (version 1, all read-only)

| Entity | Type / class | Notes |
|---|---|---|
| Water level | sensor, `distance`, `in`, measurement | the main one for graphs |
| Pump state | sensor, enum: idle/auto/extension/manual/maintenance/paused/ota | |
| Pump running | binary_sensor, `running` | feeds a history-stats "runs per hour" helper |
| High water | binary_sensor, `problem` | |
| Pump trouble | binary_sensor, `problem` | 3 back-to-back timeouts |
| Sensor fault | binary_sensor, `problem` | |
| Runtime today / 5-day / last run | sensors, `duration`, `s` | |
| Last run time | sensor, `timestamp` | UTC ISO-8601 from the device |
| WiFi RSSI, uptime, reset reason, firmware, IP | sensors, entity_category `diagnostic` | the reset reason is the watchdog/brownout early warning |

One HA device: identifiers `["sump_monitor"]`, name "Sump Monitor", model "Feather ESP32-S2 TFT + ADS1115", `sw_version` taken from
`FW_VERSION`. Entity ids will come out as `sensor.sump_monitor_water_level`, `binary_sensor.sump_monitor_high_water`, and so on.
Please confirm HA's `homeassistant:` customisations do not already claim `sump_monitor_*`.

Example discovery payload (level sensor):

```json
{"name":"Water level","unique_id":"sump_monitor_water_level","state_topic":"sump_monitor/state",
 "value_template":"{{ value_json.level_in }}","unit_of_measurement":"in","device_class":"distance",
 "state_class":"measurement","availability_topic":"sump_monitor/status","expire_after":60,
 "device":{"identifiers":["sump_monitor"],"name":"Sump Monitor","manufacturer":"DIY",
           "model":"Feather ESP32-S2 TFT + ADS1115","sw_version":"1.1"}}
```

### 4.4 Firmware architecture

- **The pump control loop must never wait on the network.** MQTT runs in its own small FreeRTOS task (priority equal to
  `loop()`, stack about 6 KB). It reads a snapshot of the status variables under a short critical section and publishes it.
  A broker outage costs the monitor nothing but telemetry. `loop()` keeps the loop-task watchdog from v1.1; the MQTT task is not
  watched, so a stuck connect cannot reset the controller.
- Reconnect with back-off (5 s, doubling to 60 s); discovery configs go out after every successful connect.
- Use the broker **IP** (`192.168.1.12`, which has a reservation), not `homeauto.local`: the `.local` name already fails to
  resolve at HA start-up (SysRef §6.2), and mDNS lookups from the ESP are not worth a new failure mode.
- `secrets.h` gets one new line, `SECRET_MQTT_HOST`. The broker is anonymous, so there are no new credentials.

### 4.5 Library choice (decision needed) and flash budget

The OTA slot is 1,310,720 bytes and v1.1 is already 1,106,633 (84%).

| Client | Added to the image | Notes |
|---|---|---|
| `esp-mqtt`, bundled with core 4.0.0 (no install; I built a probe against it) | **+125,476 bytes measured** (WiFi-only probe 948,321 vs the same probe with the client 1,073,797) | event driven, own task and reconnect built in. Would bring the sketch to roughly 94% |
| PubSubClient (not installed) | about 10-15 KB, **my estimate, not measured** | smallest; needs `arduino-cli lib install PubSubClient` into `~/ArduinoLocal/libraries`; its `connect()` blocks, which is why it must live in the task above; set the buffer to at least 1024 bytes for the discovery payloads |

**Recommendation: PubSubClient.** At 94% there is no room left for the planned DS18B20 temperature work, and the partition scheme
cannot be changed over WiFi. This needs your OK to install a library.

### 4.6 Security stance

- Version 1 is **read-only**: nothing in HA can start or stop the pump. The broker is open to the whole LAN (SysRef §11(7)); a
  writable relay topic on it would be a hazard to the house.
- A later "silence beeper" button is harmless and could be added. A "run pump" command should wait until the broker has auth and
  an ACL (the ACL file exists but is not enabled, SysRef §3).

## 5. HA-side preparation

- `HomeAssistant/sump_monitor.package.draft.yaml` (next to this file) is a **draft** package: alerts for high water, pump
  trouble, sensor fault and "controller silent for 5 minutes". It follows the house convention (package file plus a mirror, alerts
  through `script.house_notify`). It has **not** been copied to HA.
- Open point: I do not know the field names `script.house_notify` expects. The draft marks them `TODO confirm`.
- "Pump runs per hour" is best made as a History Stats helper in the UI on `binary_sensor.sump_monitor_pump_running` (type
  count, last hour) rather than YAML, because History Stats' YAML support has been changing between releases.
- Dashboards in this house are storage-mode and hand-styled (SysRef §6.2); I did not design a card. A level gauge plus the three
  problem indicators and the runtime numbers is the obvious first card.

## 6. The hardwired path (K1/K2)

- From the old context file: K1 = sump status (1 kΩ in a supervised zone, relay OFF; open circuit on alarm), K2 = system trouble
  (short on trouble; N.C. contacts so a power failure also shows as trouble). That file is ambiguous about K2's polarity (it says
  "relay OFF = no short" and also "N.C. contacts for auto-short on power fail"), and the firmware drives both relays LOW at boot, so
  the real wiring has to be read off the hardware before any code touches them. The OmniPro side already exists (zone 087 and
  macros 124/125).
- **Not implemented, deliberately.** The moment K1 changes state the house announces "front lawn sump pump failure" and sends
  texts. Before coding it I need: which zones K1 and K2 actually land on, and a deliberate test plan (for example a test window with
  the macro's text action disabled). Suggested logic when you are ready: K1 on = high-water alarm or pump trouble for more than
  10 minutes; K2 on = sensor fault, reset by watchdog, or no power.
- The two paths will alert independently. That is intended; give the HA entities distinct names so a double alert is easy to read.

## 7. Rollout and test plan

| Phase | What | Needs from you |
|---|---|---|
| 0 | Bench-test v1.1 (checklist in the analysis doc), then leave it running a day | upload, observe |
| 1 | Add the MQTT task and discovery to the firmware as v1.2; compile-check on 4.0.0; keep `ENABLE_MQTT` as a compile switch so it can be turned off | answers to section 8 |
| 2 | Upload v1.2; watch the broker: `ssh awardblvr@192.168.1.12 "docker exec mosquitto sh -c 'timeout 15 mosquitto_sub -h localhost -v -t sump_monitor/#'"` (from the SysRef cookbook). Confirm `status`, `state` every 10 s and the retained discovery topics | you run the command (or tell me to) |
| 3 | Entities appear in HA; check names, units, `expire_after` by unplugging the sump controller briefly | a look at HA |
| 4 | Install the alert package; test each alert with a harmless trigger | your OK to edit HA |
| 5 | Optional: silence button, K1/K2 logic, DS18B20 | decisions |

Once v1.2 publishes, the controller can be checked from any machine that can reach the broker, which also sidesteps the
Mac-to-device UDP problem (the broker is on the Pi, not on the sump controller).

## 8. Questions for you

1. OK to install **PubSubClient** into `~/ArduinoLocal/libraries` (recommended), or use the bundled esp-mqtt and accept 94% flash?
2. Topic prefix `sump_monitor` and device name "Sump Monitor": fine?
3. Version 1 strictly read-only: agreed?
4. What fields does `script.house_notify` take, and who should be alerted for each cause?
5. Which OmniPro zones are K1 and K2 wired to (is "Lawn Sump alarm condition", zone 087, this controller)?
6. Do you want me to look at the live HA box and broker (over SSH, read-only) to confirm entity names and the script, or keep working
   from the documents only?
