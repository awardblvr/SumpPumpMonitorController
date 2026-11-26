// SumpMqtt.ino - MQTT / Home Assistant interface for the Sump Monitor (firmware 2.0)
//
// Second tab of the sketch; the Arduino build appends it after SumpMonitorController.ino, so it sees all of
// that file's globals and macros. See docs/MQTT_Interface_v2.0_2026-11-25.md for the full topic/command reference.
//
// ARCHITECTURE - why it is built this way
//   * ALL pump logic stays in loop(). This file adds one FreeRTOS task ("mqtt") that owns the WiFiClient and
//     PubSubClient. PubSubClient's connect() blocks for seconds when the broker is down; running it in its own
//     task means a broker/WiFi problem can never stall pump control or trip the loop watchdog.
//   * The two sides only exchange data through small mutex-protected mailboxes:
//       loop() -> task :  state JSON, stats JSON (latest wins), outgoing responses/events (small queue)
//       task -> loop() :  incoming command lines (small queue)
//     Commands are therefore EXECUTED in loop(), in the same context as every other pump decision.
//   * Nothing here ever blocks loop() for more than a few milliseconds (mutex waits are capped at 50 ms).
//
// TOPICS (prefix "Sump_pump"; topics are case-sensitive)
//   Sump_pump/status    "online"/"offline", retained; "offline" is the MQTT last will; "online" re-sent every 60 s
//   Sump_pump/state     JSON snapshot: on any change, every 5 min normally, every 2 min while water is rising/problem
//   Sump_pump/stats     JSON full dump of measurements, parameters, time (only when the "stats" command is sent)
//   Sump_pump/event     JSON pushed immediately when a problem is raised/cleared (high water, pump trouble, sensor fault)
//   Sump_pump/response  JSON reply to every command
//   Sump_pump/cmd       commands IN (plain text). NEVER publish commands with the retain flag.
//
// COMMANDS (case-insensitive, details in sump_logic.h)
//   stats | pump_off | pump_on [seconds 5-300] | disable [hours 1-24] | enable | silence | help
//   set trigger=5.5 off=1.0 alarm=10        (any subset; validated; saved to flash only when changed)

#if ENABLE_MQTT

#include <PubSubClient.h>
#include <esp_timer.h>
#include <time.h>

// Optional overrides - define any of these in secrets.h; the defaults match the house broker.
#ifndef SECRET_MQTT_HOST
#define SECRET_MQTT_HOST "192.168.1.12"   // IP address (not homeauto.local: .local names do not resolve reliably)
#endif
#ifndef SECRET_MQTT_PORT
#define SECRET_MQTT_PORT 1883
#endif
#ifndef SECRET_MQTT_USER
#define SECRET_MQTT_USER ""               // empty = anonymous (the house broker is anonymous today)
#endif
#ifndef SECRET_MQTT_PASS
#define SECRET_MQTT_PASS ""
#endif

#define MQTT_BASE "Sump_pump"
#define T_STATUS MQTT_BASE "/status"
#define T_STATE MQTT_BASE "/state"
#define T_STATS MQTT_BASE "/stats"
#define T_EVENT MQTT_BASE "/event"
#define T_RESP MQTT_BASE "/response"
#define T_CMD MQTT_BASE "/cmd"
#define MQTT_CLIENT_ID "Sump_pump"        // fixed on purpose: a changing id leaves ghost sessions behind the broker
#define MQTT_DISCOVERY_PREFIX "homeassistant"
#define STATE_EXPIRE_SEC 900              // HA marks sensors unavailable after 3 missed baseline reports
#define CMD_IGNORE_AFTER_SUBSCRIBE_MS 1500  // retained leftovers are delivered right after subscribing - ignore them
#define SET_MIN_INTERVAL_MS 10000UL         // minimum time between two level changes that write flash

// ------------------------------------------------------------------------------------------------
// Mailboxes between loop() and the MQTT task
// ------------------------------------------------------------------------------------------------
static SemaphoreHandle_t mq_mutex = NULL;

static char mb_state[1000];
static bool mb_state_new = false;
static char mb_stats[1800];
static bool mb_stats_new = false;

enum { MSG_RESPONSE = 0, MSG_EVENT = 1 };
struct OutMsg { uint8_t kind; char text[400]; };
#define OUTQ_LEN 6
static OutMsg outq[OUTQ_LEN];
static uint8_t outq_head = 0, outq_count = 0;

struct InCmd { char text[96]; };
#define INQ_LEN 4
static InCmd inq[INQ_LEN];
static uint8_t inq_head = 0, inq_count = 0;

static volatile bool mq_connected = false;
static volatile bool mq_connected_event = false;   // set on every (re)connect; loop() answers with an immediate state report
static volatile uint32_t mq_subscribed_ms = 0;
static volatile bool mq_cmd_armed = false;         // commands are only accepted once the post-subscribe window has passed
static volatile bool mq_arm_pending = false;
static volatile bool mq_ha_birth = false;          // Home Assistant just (re)started: republish discovery and state
static TaskHandle_t mq_task = NULL;
static uint32_t cmd_dropped = 0, mq_reconnects = 0, mq_pub_fail = 0;   // plain counters: written by either task, read for the stats dump
static bool report_now_flag = false;               // loop()-only: "publish state at the next opportunity"

static WiFiClient mqNet;
static PubSubClient mqtt(mqNet);

static bool mqLock(uint32_t ms) { return mq_mutex && xSemaphoreTake(mq_mutex, pdMS_TO_TICKS(ms)) == pdTRUE; }
static void mqUnlock() { xSemaphoreGive(mq_mutex); }

// ---- loop() side producers --------------------------------------------------------------------
static void mqSetState(const char* json) {
  if (!mqLock(50)) return;
  strncpy(mb_state, json, sizeof(mb_state) - 1);
  mb_state[sizeof(mb_state) - 1] = '\0';
  mb_state_new = true;
  mqUnlock();
}

static void mqSetStats(const char* json) {
  if (!mqLock(50)) return;
  strncpy(mb_stats, json, sizeof(mb_stats) - 1);
  mb_stats[sizeof(mb_stats) - 1] = '\0';
  mb_stats_new = true;
  mqUnlock();
}

static void mqPushOut(uint8_t kind, const char* text) {
  if (!mqLock(50)) return;
  if (outq_count == OUTQ_LEN) {              // full: drop the oldest
    outq_head = (uint8_t)((outq_head + 1) % OUTQ_LEN);
    outq_count--;
  }
  uint8_t idx = (uint8_t)((outq_head + outq_count) % OUTQ_LEN);
  outq[idx].kind = kind;
  strncpy(outq[idx].text, text, sizeof(outq[idx].text) - 1);
  outq[idx].text[sizeof(outq[idx].text) - 1] = '\0';
  outq_count++;
  mqUnlock();
}

static bool mqPopCmd(char* out, size_t n) {
  bool got = false;
  if (!mqLock(10)) return false;
  if (inq_count > 0) {
    strncpy(out, inq[inq_head].text, n - 1);
    out[n - 1] = '\0';
    inq_head = (uint8_t)((inq_head + 1) % INQ_LEN);
    inq_count--;
    got = true;
  }
  mqUnlock();
  return got;
}

// ---- MQTT task side consumers -----------------------------------------------------------------
static bool mqTakeState(char* dst, size_t n) {
  bool got = false;
  if (!mqLock(20)) return false;
  if (mb_state_new) {
    strncpy(dst, mb_state, n - 1);
    dst[n - 1] = '\0';
    mb_state_new = false;
    got = true;
  }
  mqUnlock();
  return got;
}

static bool mqTakeStats(char* dst, size_t n) {
  bool got = false;
  if (!mqLock(20)) return false;
  if (mb_stats_new) {
    strncpy(dst, mb_stats, n - 1);
    dst[n - 1] = '\0';
    mb_stats_new = false;
    got = true;
  }
  mqUnlock();
  return got;
}

// (Takes plain types, not OutMsg*: the Arduino build generates function prototypes ABOVE the struct definitions.)
static bool mqTakeOut(uint8_t* kind, char* text, size_t n) {
  bool got = false;
  if (!mqLock(20)) return false;
  if (outq_count > 0) {
    *kind = outq[outq_head].kind;
    strncpy(text, outq[outq_head].text, n - 1);
    text[n - 1] = '\0';
    outq_head = (uint8_t)((outq_head + 1) % OUTQ_LEN);
    outq_count--;
    got = true;
  }
  mqUnlock();
  return got;
}

static void onMqttMessage(char* topic, uint8_t* payload, unsigned int len) {
  if (!strcmp(topic, MQTT_DISCOVERY_PREFIX "/status")) {              // Home Assistant's birth message
    if (len == 6 && !memcmp(payload, "online", 6)) mq_ha_birth = true;
    return;
  }
  if (strcmp(topic, T_CMD) != 0 || len == 0) return;                  // empty = our own retained-clear echo
  if (len >= sizeof(InCmd::text)) { cmd_dropped++; return; }
  // Not armed yet = within CMD_IGNORE_AFTER_SUBSCRIBE_MS of subscribing: anything arriving now is a retained
  // leftover (or noise), never a live command. The task arms commands after the window.
  if (!mq_cmd_armed) { cmd_dropped++; return; }
  if (!mqLock(20)) { cmd_dropped++; return; }
  if (inq_count < INQ_LEN) {
    uint8_t idx = (uint8_t)((inq_head + inq_count) % INQ_LEN);
    memcpy(inq[idx].text, payload, len);
    inq[idx].text[len] = '\0';
    inq_count++;
  } else {
    cmd_dropped++;
  }
  mqUnlock();
}

// ------------------------------------------------------------------------------------------------
// Home Assistant MQTT discovery (published, retained, on every connect)
// ------------------------------------------------------------------------------------------------
#define D_STATE "\"state_topic\":\"" T_STATE "\""
#define D_CMD "\"command_topic\":\"" T_CMD "\""
#define D_EXP "\"expire_after\":" "900"

struct DiscEntity { const char* comp; const char* obj; const char* name; const char* body; };

static const DiscEntity ENTITIES[] = {
  // --- sensors ---
  {"sensor", "water_level", "Water level",
   D_STATE ",\"value_template\":\"{{ value_json.level_in }}\",\"unit_of_measurement\":\"in\",\"device_class\":\"distance\",\"state_class\":\"measurement\"," D_EXP},
  {"sensor", "pump_state", "Pump state",
   D_STATE ",\"value_template\":\"{{ value_json.pump_state }}\",\"icon\":\"mdi:water-pump\"," D_EXP},
  {"sensor", "runtime_today", "Runtime today",
   D_STATE ",\"value_template\":\"{{ value_json.today_s }}\",\"unit_of_measurement\":\"s\",\"device_class\":\"duration\",\"state_class\":\"measurement\"," D_EXP},
  {"sensor", "runtime_5day", "Runtime 5 days",
   D_STATE ",\"value_template\":\"{{ value_json.five_day_s }}\",\"unit_of_measurement\":\"s\",\"device_class\":\"duration\",\"state_class\":\"measurement\"," D_EXP},
  {"sensor", "last_run_duration", "Last run duration",
   D_STATE ",\"value_template\":\"{{ value_json.last_run_s }}\",\"unit_of_measurement\":\"s\",\"device_class\":\"duration\",\"state_class\":\"measurement\"," D_EXP},
  {"sensor", "last_run_time", "Last run time",
   D_STATE ",\"value_template\":\"{{ value_json.last_run_utc }}\",\"device_class\":\"timestamp\"," D_EXP},
  {"sensor", "disabled_remaining", "Pump disable remaining",
   D_STATE ",\"value_template\":\"{{ value_json.disabled_min }}\",\"unit_of_measurement\":\"min\",\"icon\":\"mdi:timer-outline\"," D_EXP},
  {"sensor", "report_mode", "Reporting mode",
   D_STATE ",\"value_template\":\"{{ value_json.report_mode }}\",\"entity_category\":\"diagnostic\"," D_EXP},
  {"sensor", "wifi_signal", "WiFi signal",
   D_STATE ",\"value_template\":\"{{ value_json.rssi }}\",\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\"," D_EXP},
  {"sensor", "uptime", "Uptime",
   D_STATE ",\"value_template\":\"{{ value_json.uptime_s }}\",\"unit_of_measurement\":\"s\",\"device_class\":\"duration\",\"entity_category\":\"diagnostic\"," D_EXP},
  {"sensor", "reset_reason", "Last reset reason",
   D_STATE ",\"value_template\":\"{{ value_json.reset }}\",\"entity_category\":\"diagnostic\"," D_EXP},
  {"sensor", "firmware", "Firmware",
   D_STATE ",\"value_template\":\"{{ value_json.fw }}\",\"entity_category\":\"diagnostic\"," D_EXP},
  {"sensor", "ip_address", "IP address",
   D_STATE ",\"value_template\":\"{{ value_json.ip }}\",\"entity_category\":\"diagnostic\"," D_EXP},
  // --- binary sensors ---
  {"binary_sensor", "pump_running", "Pump running",
   D_STATE ",\"value_template\":\"{{ value_json.pump }}\",\"device_class\":\"running\"," D_EXP},
  {"binary_sensor", "high_water", "High water",
   D_STATE ",\"value_template\":\"{{ 'ON' if value_json.high_water else 'OFF' }}\",\"device_class\":\"problem\"," D_EXP},
  {"binary_sensor", "pump_trouble", "Pump trouble",
   D_STATE ",\"value_template\":\"{{ 'ON' if value_json.pump_trouble else 'OFF' }}\",\"device_class\":\"problem\"," D_EXP},
  {"binary_sensor", "sensor_fault", "Level sensor fault",
   D_STATE ",\"value_template\":\"{{ 'ON' if value_json.sensor_fault else 'OFF' }}\",\"device_class\":\"problem\"," D_EXP},
  {"binary_sensor", "alarm", "Alarm",
   D_STATE ",\"value_template\":\"{{ 'ON' if value_json.alarm else 'OFF' }}\",\"device_class\":\"problem\"," D_EXP},
  {"binary_sensor", "pump_disabled", "Pump disabled",
   D_STATE ",\"value_template\":\"{{ 'ON' if value_json.pump_disabled else 'OFF' }}\",\"icon\":\"mdi:water-pump-off\"," D_EXP},
  // --- buttons (send a command) ---
  {"button", "stats_dump", "Stats dump", D_CMD ",\"payload_press\":\"stats\",\"icon\":\"mdi:clipboard-text-outline\""},
  {"button", "pump_stop", "Stop pump", D_CMD ",\"payload_press\":\"pump_off\",\"icon\":\"mdi:stop\""},
  {"button", "pump_force_on", "Run pump 5 min", D_CMD ",\"payload_press\":\"pump_on\",\"icon\":\"mdi:play\""},
  {"button", "pump_disable_24h", "Disable pump 24 h", D_CMD ",\"payload_press\":\"disable\",\"icon\":\"mdi:water-pump-off\""},
  {"button", "pump_enable", "Enable pump", D_CMD ",\"payload_press\":\"enable\",\"icon\":\"mdi:water-pump\""},
  {"button", "silence_alarm", "Silence alarm", D_CMD ",\"payload_press\":\"silence\",\"icon\":\"mdi:bell-off\""},
  // --- numbers (change a level; the firmware validates and saves to flash only if different) ---
  {"number", "trigger_level", "Trigger level",
   D_STATE ",\"value_template\":\"{{ value_json.trigger_in }}\"," D_CMD ",\"command_template\":\"set trigger={{ value }}\","
   "\"min\":2.0,\"max\":12.0,\"step\":0.1,\"mode\":\"box\",\"unit_of_measurement\":\"in\",\"entity_category\":\"config\""},
  {"number", "off_level", "Off level",
   D_STATE ",\"value_template\":\"{{ value_json.off_in }}\"," D_CMD ",\"command_template\":\"set off={{ value }}\","
   "\"min\":0.5,\"max\":6.0,\"step\":0.05,\"mode\":\"box\",\"unit_of_measurement\":\"in\",\"entity_category\":\"config\""},
  {"number", "alarm_level", "Alarm level",
   D_STATE ",\"value_template\":\"{{ value_json.alarm_in }}\"," D_CMD ",\"command_template\":\"set alarm={{ value }}\","
   "\"min\":2.0,\"max\":17.0,\"step\":0.1,\"mode\":\"box\",\"unit_of_measurement\":\"in\",\"entity_category\":\"config\""},
};
static const size_t ENTITY_COUNT = sizeof(ENTITIES) / sizeof(ENTITIES[0]);

static bool mqPub(const char* topic, const char* payload, bool retained) {
  bool ok = mqtt.publish(topic, payload, retained);
  if (!ok) mq_pub_fail++;
  return ok;
}

static void publishDiscovery() {
  static char topic[96];
  static char payload[1400];
  for (size_t i = 0; i < ENTITY_COUNT; i++) {
    const DiscEntity& e = ENTITIES[i];
    snprintf(topic, sizeof(topic), MQTT_DISCOVERY_PREFIX "/%s/sump_pump/%s/config", e.comp, e.obj);
    int n = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"sump_pump_%s\",%s,\"availability_topic\":\"" T_STATUS "\","
        "\"device\":{\"identifiers\":[\"sump_pump\"],\"name\":\"Sump Pump\",\"manufacturer\":\"DIY\","
        "\"model\":\"Feather ESP32-S2 TFT + ADS1115\",\"sw_version\":\"" FW_VERSION "\"}}",
        e.name, e.obj, e.body);
    if (n <= 0 || n >= (int)sizeof(payload)) { mq_pub_fail++; continue; }   // would be truncated - skip, never send half a JSON
    mqPub(topic, payload, true);
    vTaskDelay(pdMS_TO_TICKS(10));   // pacing only; incoming packets are handled by the main task loop afterwards
  }
}

// ------------------------------------------------------------------------------------------------
// MQTT task
// ------------------------------------------------------------------------------------------------
static uint32_t last_discovery_ms = 0;
static bool discovery_done = false;

static bool mqttConnectOnce() {
  mqtt.setServer(SECRET_MQTT_HOST, SECRET_MQTT_PORT);
  bool ok;
  if (SECRET_MQTT_USER[0]) {
    ok = mqtt.connect(MQTT_CLIENT_ID, SECRET_MQTT_USER, SECRET_MQTT_PASS, T_STATUS, 1, true, "offline");
  } else {
    ok = mqtt.connect(MQTT_CLIENT_ID, T_STATUS, 1, true, "offline");
  }
  if (!ok) return false;

  mq_reconnects++;
  mqPub(T_STATUS, "online", true);

  // Commands stay DISARMED until CMD_IGNORE_AFTER_SUBSCRIBE_MS after this point. The window must start BEFORE
  // subscribing: retained messages are delivered immediately, and a retained "pump_on" must never be executed.
  mq_cmd_armed = false;
  mq_subscribed_ms = millis();
  mq_arm_pending = true;
  mqtt.subscribe(T_CMD);
  mqtt.subscribe(MQTT_DISCOVERY_PREFIX "/status");   // Home Assistant's birth message: it asks devices to republish

  // Discovery configs are retained, so after a quick reconnect they are already there: re-send at most once a minute
  // (a flapping session must not turn into 28 publishes per attempt).
  if (!discovery_done || (uint32_t)(millis() - last_discovery_ms) >= 60000UL) {
    publishDiscovery();
    discovery_done = true;
    last_discovery_ms = millis();
  }
  mq_connected = true;
  mq_connected_event = true;
  return true;
}

static void mqttTask(void*) {
  static char tl_state[1000];
  static char tl_stats[1800];
  static char tl_out[400];
  uint32_t backoff = 5000, last_attempt_ms = 0, last_online_ms = 0;
  bool attempted = false;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      mq_connected = false;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (!mqtt.connected()) {
      mq_connected = false;
      mq_cmd_armed = false;
      // Unsigned elapsed-time compare (wrap-safe for the whole 49.7 day millis() range); a minimum of
      // `backoff` between attempts, successful or not, so a dropping session cannot cause a reconnect storm.
      if (!attempted || (uint32_t)(millis() - last_attempt_ms) >= backoff) {
        attempted = true;
        last_attempt_ms = millis();
        if (mqttConnectOnce()) {
          backoff = 5000;
          last_online_ms = millis();
        } else {
          backoff = (backoff * 2 > 60000) ? 60000 : backoff * 2;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // WiFi blipped but the TCP session survived: the link is usable again, so say so (display + a state report)
    if (!mq_connected) {
      mq_connected = true;
      mq_connected_event = true;
    }

    mqtt.loop();

    // Arm commands once the post-subscribe window is over, and wipe any retained command from the broker
    if (mq_arm_pending && (uint32_t)(millis() - mq_subscribed_ms) >= CMD_IGNORE_AFTER_SUBSCRIBE_MS) {
      mq_arm_pending = false;
      mq_cmd_armed = true;
      mqtt.publish(T_CMD, (const uint8_t*)"", 0, true);   // empty retained message deletes a retained command
    }

    // Home Assistant restarted: re-send discovery and ask loop() for a fresh state (state is not retained)
    if (mq_ha_birth) {
      mq_ha_birth = false;
      publishDiscovery();
      last_discovery_ms = millis();
      mq_connected_event = true;
    }

    if (mqTakeState(tl_state, sizeof(tl_state))) mqPub(T_STATE, tl_state, false);
    if (mqTakeStats(tl_stats, sizeof(tl_stats))) mqPub(T_STATS, tl_stats, false);
    uint8_t out_kind;
    while (mqTakeOut(&out_kind, tl_out, sizeof(tl_out))) mqPub(out_kind == MSG_EVENT ? T_EVENT : T_RESP, tl_out, false);

    // Re-assert "online" regularly: a ghost session's retained last will can otherwise blank every entity for hours
    if ((uint32_t)(millis() - last_online_ms) >= 60000) {
      last_online_ms = millis();
      mqPub(T_STATUS, "online", true);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void mqttBegin() {
  mq_mutex = xSemaphoreCreateMutex();
  mqtt.setBufferSize(2048);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(3);
  mqtt.setCallback(onMqttMessage);
  xTaskCreate(mqttTask, "mqtt", 6144, NULL, 1, &mq_task);
}

bool mqttIsConnected() { return mq_connected; }

// Discards queued commands. Called when an OTA update starts or fails: commands that piled up while loop() was
// busy flashing must not run afterwards, long after whoever sent them stopped expecting them to.
void mqttFlushCommands() {
  if (!mqLock(50)) return;
  inq_count = 0;
  mqUnlock();
}

// ------------------------------------------------------------------------------------------------
// JSON builders (run in loop(), so they can read pump state without locking)
// ------------------------------------------------------------------------------------------------
static const char* tf(bool b) { return b ? "true" : "false"; }

static void fmtLocalTime(char* out, size_t n) {
  if (!time_synced) { snprintf(out, n, "unsynced"); return; }
  time_t now;
  time(&now);
  struct tm t;
  localtime_r(&now, &t);
  strftime(out, n, "%Y-%m-%d %H:%M:%S %Z", &t);
}

static void fmtLevel(char* out, size_t n) {
  if (sensor_fault) snprintf(out, n, "null");
  else snprintf(out, n, "%.2f", current_avg_inches);
}

static void fmtLastRunUtc(char* out, size_t n) {
  if (last_start == 0) { snprintf(out, n, "null"); return; }
  time_t t = (time_t)last_start;
  struct tm g;
  gmtime_r(&t, &g);
  char tmp[24];
  strftime(tmp, sizeof(tmp), "%Y-%m-%dT%H:%M:%SZ", &g);
  snprintf(out, n, "\"%s\"", tmp);
}

static void fmtIp(char* out, size_t n) {
  IPAddress ip = WiFi.localIP();
  snprintf(out, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static long disabledMinutes() {
  if (!pump_disabled) return 0;
  long ms = (long)(pump_disabled_until_ms - millis());
  return ms > 0 ? ms / 60000 : 0;
}

static char jbuf[1800];   // loop()-only scratch for building JSON

static void buildStateJson() {
  char tm_s[32], lvl[16], lr[32], ip[20];
  fmtLocalTime(tm_s, sizeof(tm_s));
  fmtLevel(lvl, sizeof(lvl));
  fmtLastRunUtc(lr, sizeof(lr));
  fmtIp(ip, sizeof(ip));
  uint32_t five = 0;
  for (int i = 0; i < 5; i++) five += daily_secs[i];

  int n = snprintf(jbuf, sizeof(jbuf),
      "{\"time\":\"%s\",\"level_in\":%s,\"pump\":\"%s\",\"pump_state\":\"%s\",\"high_water\":%s,\"pump_trouble\":%s,"
      "\"sensor_fault\":%s,\"alarm\":%s,\"muted\":%s,\"pump_disabled\":%s,\"disabled_min\":%ld,\"forced\":%s,"
      "\"report_mode\":\"%s\",\"today_s\":%lu,\"five_day_s\":%lu,\"last_run_s\":%lu,\"last_run_utc\":%s,"
      "\"rssi\":%d,\"uptime_s\":%lu,\"fw\":\"" FW_VERSION "\",\"ip\":\"%s\",\"reset\":\"%s\","
      "\"trigger_in\":%.2f,\"off_in\":%.2f,\"alarm_in\":%.2f}",
      tm_s, lvl, pump_on ? "ON" : "OFF", pumpStateName(), tf(high_water_alarm), tf(pump_trouble),
      tf(sensor_fault), tf(alarm_active), tf(alarm_silenced), tf(pump_disabled), disabledMinutes(), tf(forced_run),
      reporter.elevated ? "elevated" : "baseline", (unsigned long)daily_secs[0], (unsigned long)five,
      (unsigned long)last_dur_sec, lr,
      WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0, (unsigned long)(esp_timer_get_time() / 1000000ULL), ip, boot_reason,
      trigger_inches, off_inches, alarm_inches);
  if (n <= 0 || n >= (int)sizeof(mb_state)) { mq_pub_fail++; return; }   // never publish truncated JSON (limit = the mailbox size)
  mqSetState(jbuf);
}

static void buildStatsJson() {
  char tm_s[32], lvl[16], lr[32], ip[20];
  fmtLocalTime(tm_s, sizeof(tm_s));
  fmtLevel(lvl, sizeof(lvl));
  fmtLastRunUtc(lr, sizeof(lr));
  fmtIp(ip, sizeof(ip));
  uint32_t five = 0;
  for (int i = 0; i < 5; i++) five += daily_secs[i];
  time_t now = 0;
  if (time_synced) time(&now);
  long pause_left = pump_in_pause ? (long)PAUSE_SEC - (long)((millis() - pump_pause_start_ms) / 1000) : 0;
  if (pause_left < 0) pause_left = 0;
  long forced_left = (manual_pump_active && pump_on) ? (long)(manual_pump_end_ms - millis()) / 1000 : 0;
  if (forced_left < 0) forced_left = 0;

  int n = snprintf(jbuf, sizeof(jbuf),
      "{\"kind\":\"stats\",\"fw\":\"" FW_VERSION "\",\"build\":\"" __DATE__ " " __TIME__ "\",\"time\":\"%s\",\"epoch\":%ld,"
      "\"tz\":\"%s\",\"time_synced\":%s,\"uptime_s\":%lu,\"reset\":\"%s\","
      "\"free_heap\":%lu,\"min_free_heap\":%lu,"
      "\"wifi_rssi\":%d,\"ip\":\"%s\",\"mac\":\"%s\","
      "\"level_in\":%s,\"adc_raw\":%d,\"sensor_volts\":%.3f,\"sensor_ok\":%s,\"adc_present\":%s,"
      "\"pump\":\"%s\",\"pump_state\":\"%s\",\"pump_run_s\":%lu,\"manual\":%s,\"forced\":%s,\"forced_left_s\":%ld,"
      "\"extension\":%s,\"maintenance\":%s,\"paused\":%s,\"pause_left_s\":%ld,\"pump_disabled\":%s,\"disabled_min\":%ld,"
      "\"consecutive_timeouts\":%u,"
      "\"high_water\":%s,\"pump_trouble\":%s,\"sensor_fault\":%s,\"alarm\":%s,\"muted\":%s,"
      "\"trigger_in\":%.2f,\"off_in\":%.2f,\"alarm_in\":%.2f,\"alarm_clear_in\":%.2f,"
      "\"extension_s\":%d,\"timeout_s\":%d,\"pause_s\":%d,\"sensor_offset_in\":%.2f,"
      "\"maint_hour\":%d,\"maint_trigger_in\":%.2f,\"maint_target_in\":%.2f,\"maint_max_s\":%lu,"
      "\"force_max_s\":%d,\"disable_max_h\":%d,"
      "\"today_s\":%lu,\"five_day_s\":%lu,\"daily_s\":[%lu,%lu,%lu,%lu,%lu],\"last_run_s\":%lu,\"last_run_utc\":%s,"
      "\"report_mode\":\"%s\",\"report_baseline_s\":%lu,\"report_elevated_s\":%lu,\"elevated_max_h\":%lu,\"elevated_age_s\":%lu,"
      "\"mqtt_reconnects\":%lu,\"mqtt_pub_fail\":%lu,\"cmd_dropped\":%lu,\"mqtt_task_stack_free\":%lu}",
      tm_s, (long)now, TZ_INFO, tf(time_synced), (unsigned long)(esp_timer_get_time() / 1000000ULL), boot_reason,
      (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
      WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0, ip, WiFi.macAddress().c_str(),
      lvl, (int)last_adc_raw, last_adc_vin, tf(!sensor_fault), tf(ads_present),
      pump_on ? "ON" : "OFF", pumpStateName(), pump_on ? (unsigned long)((millis() - pump_start_ms) / 1000) : 0UL,
      tf(manual_pump_active), tf(forced_run), forced_left,
      tf(pump_extension_active), tf(maintenance_mode), tf(pump_in_pause), pause_left, tf(pump_disabled), disabledMinutes(),
      (unsigned)pump_consecutive_timeouts,
      tf(high_water_alarm), tf(pump_trouble), tf(sensor_fault), tf(alarm_active), tf(alarm_silenced),
      trigger_inches, off_inches, alarm_inches, alarm_inches - ALARM_HYSTERESIS_INCHES,
      (int)EXTENSION_SEC, (int)TIMEOUT_SEC, (int)PAUSE_SEC, (double)SENSOR_OFFSET_INCHES,
      (int)MAINTENANCE_HOUR, (double)MAINTENANCE_TRIGGER_INCHES, (double)MAINTENANCE_TARGET_INCHES, (unsigned long)MAINTENANCE_MAX_SEC,
      (int)SUMP_FORCE_MAX_SEC, (int)SUMP_DISABLE_MAX_HOURS,
      (unsigned long)daily_secs[0], (unsigned long)five,
      (unsigned long)daily_secs[0], (unsigned long)daily_secs[1], (unsigned long)daily_secs[2],
      (unsigned long)daily_secs[3], (unsigned long)daily_secs[4],
      (unsigned long)last_dur_sec, lr,
      reporter.elevated ? "elevated" : "baseline", (unsigned long)(REPORT_BASELINE_MS / 1000), (unsigned long)(REPORT_ELEVATED_MS / 1000),
      (unsigned long)(REPORT_ELEVATED_MAX_MS / 3600000UL), (unsigned long)(reporter.elevatedAgeMs(millis()) / 1000),
      (unsigned long)mq_reconnects, (unsigned long)mq_pub_fail, (unsigned long)cmd_dropped,
      (unsigned long)(mq_task ? uxTaskGetStackHighWaterMark(mq_task) : 0));
  if (n <= 0 || n >= (int)sizeof(jbuf)) { mq_pub_fail++; return; }
  mqSetStats(jbuf);
}

// ------------------------------------------------------------------------------------------------
// Events (problem raised / cleared) and the periodic / on-change state report
// ------------------------------------------------------------------------------------------------
static void pushEvent(const char* name, const char* state, const char* detail) {
  char tm_s[32], lvl[16];
  fmtLocalTime(tm_s, sizeof(tm_s));
  fmtLevel(lvl, sizeof(lvl));
  char msg[300];
  int n = snprintf(msg, sizeof(msg),
      "{\"event\":\"%s\",\"state\":\"%s\",\"detail\":\"%s\",\"level_in\":%s,\"time\":\"%s\"}",
      name, state, detail, lvl, tm_s);
  if (n <= 0 || n >= (int)sizeof(msg)) return;
  mqPushOut(MSG_EVENT, msg);
  report_now_flag = true;   // a problem change always triggers a state report too
}

void mqttPublishEvent(const char* name, const char* detail) {
  pushEvent(name, "info", detail);
}

void mqttPublishCauseChange(uint8_t now_bits, uint8_t prev_bits) {
  static const char* const NAMES[3] = {"high_water", "pump_trouble", "sensor_fault"};
  static const char* const DETAIL[3] = {"water level above the alarm threshold", "repeated pump timeouts with water still high",
                                        "level sensor not responding"};
  for (int i = 0; i < 3; i++) {
    uint8_t bit = (uint8_t)(1 << i);
    if ((now_bits & bit) != (prev_bits & bit)) {
      pushEvent(NAMES[i], (now_bits & bit) ? "raised" : "cleared", DETAIL[i]);
    }
  }
}

void reportingTick() {
  static uint32_t last_sig = 0xFFFFFFFFu;
  static uint32_t last_pub_ms = 0;
  static bool pending = false;
  uint32_t now = millis();

  uint32_t sig = (uint32_t)pump_on | ((uint32_t)pump_disabled << 1) | ((uint32_t)pump_in_pause << 2) |
                 ((uint32_t)forced_run << 3) | ((uint32_t)maintenance_mode << 4) | ((uint32_t)alarm_active << 5) |
                 ((uint32_t)high_water_alarm << 6) | ((uint32_t)pump_trouble << 7) | ((uint32_t)sensor_fault << 8) |
                 ((uint32_t)alarm_silenced << 9) | ((uint32_t)reporter.elevated << 10) |
                 ((uint32_t)manual_pump_active << 11) | ((uint32_t)pump_extension_active << 12);

  // "Back to normal" must be reachable even if the OFF level was raised: calm needs the level at or below this
  float normal_level = off_inches + 0.5f;
  if (normal_level < REPORT_NORMAL_INCHES) normal_level = REPORT_NORMAL_INCHES;
  reporter.normalLevel = normal_level;

  bool due = reporter.update(now, current_avg_inches, !sensor_fault, alarm_active, pump_on);
  if (due) pending = true;
  if (sig != last_sig) { last_sig = sig; pending = true; }   // any state change is pushed (rate-limited below)
  if (mq_connected_event) { mq_connected_event = false; pending = true; }
  if (report_now_flag) { report_now_flag = false; pending = true; }

  if (pending && (uint32_t)(now - last_pub_ms) >= 2000) {   // at most one state message per 2 s, even when flapping
    pending = false;
    last_pub_ms = now;
    buildStateJson();
    reporter.noteReport(now);
  }
}

// ------------------------------------------------------------------------------------------------
// Command execution (runs in loop())
// ------------------------------------------------------------------------------------------------
static const char* cmdName(SumpCmd c) {
  switch (c) {
    case SUMP_CMD_STATS: return "stats";
    case SUMP_CMD_PUMP_OFF: return "pump_off";
    case SUMP_CMD_PUMP_ON: return "pump_on";
    case SUMP_CMD_DISABLE: return "disable";
    case SUMP_CMD_ENABLE: return "enable";
    case SUMP_CMD_SET: return "set";
    case SUMP_CMD_SILENCE: return "silence";
    case SUMP_CMD_HELP: return "help";
    default: return "unknown";
  }
}

// Sends the JSON response, puts a short message on the display, and asks for a state report.
static void respond(SumpCmd c, bool ok, const char* msg, const char* screen, uint16_t color, const char* extra = "") {
  char tm_s[32];
  fmtLocalTime(tm_s, sizeof(tm_s));
  char out[400];
  int n = snprintf(out, sizeof(out), "{\"cmd\":\"%s\",\"ok\":%s,\"msg\":\"%s\"%s,\"time\":\"%s\"}", cmdName(c), tf(ok), msg, extra, tm_s);
  if (n > 0 && n < (int)sizeof(out)) mqPushOut(MSG_RESPONSE, out);
  showScreenMessage(screen, color);
  report_now_flag = true;
  if (enaDbgSer) Serial.printf("CMD %s -> %s: %s\n", cmdName(c), ok ? "ok" : "REFUSED", msg);
}

static void executeCommand(char* line) {
  SumpCmdParsed p;
  sumpParseCommand(line, &p);
  if (p.cmd == SUMP_CMD_NONE) return;
  if (p.err) { respond(p.cmd, false, p.err, "CMD: bad command (send help)", ST77XX_RED); return; }

  switch (p.cmd) {
    case SUMP_CMD_STATS:
      buildStatsJson();
      respond(p.cmd, true, "statistics published to " T_STATS, "CMD: stats dump sent", ST77XX_CYAN);
      break;

    case SUMP_CMD_PUMP_OFF: {
      if (!pump_on && !manual_pump_active && !maintenance_mode) {
        respond(p.cmd, true, "pump was not running", "CMD: pump not running", ST77XX_CYAN);
        break;
      }
      manual_pump_active = false;
      manual_press_count = 0;
      stopPump();                          // saves statistics (unless this was the maintenance cycle); clears forced/extension
      maintenance_mode = false;
      maintenance_extension_active = false;
      // Hold automatic restart for the normal cool-down; otherwise a high level would restart the pump within 100 ms
      pump_in_pause = true;
      pump_pause_start_ms = millis();
      pause_by_command = true;
      respond(p.cmd, true, "pump stopped; automatic restart held for 60 s", "CMD: pump STOPPED (60 s hold)", ST77XX_ORANGE);
      break;
    }

    case SUMP_CMD_PUMP_ON:
      if (pump_disabled) { respond(p.cmd, false, "refused: pump is disabled (send enable first)", "CMD refused: pump disabled", ST77XX_RED); break; }
      if (maintenance_mode) { respond(p.cmd, false, "refused: 7 AM maintenance cycle running (15 s max), retry shortly", "CMD refused: maintenance run", ST77XX_RED); break; }
      if (forced_run && pump_on) { respond(p.cmd, false, "refused: a forced run is already active", "CMD refused: already forced", ST77XX_RED); break; }
      {
        unsigned long now_ms = millis();
        unsigned long want_end = now_ms + (unsigned long)p.seconds * 1000UL;
        if (pump_on) {
          // Joining a run already in progress (automatic or Button 1): keep the WHOLE continuous run within the
          // forced maximum, measured from when the pump actually started - otherwise an automatic run at 239 s
          // that is "forced" for 300 s more would reach about 540 s.
          unsigned long max_end = pump_start_ms + (unsigned long)SUMP_FORCE_MAX_SEC * 1000UL;
          if ((long)(max_end - now_ms) < 5000L) {
            respond(p.cmd, false, "refused: the pump has already been running for almost 5 minutes", "CMD refused: run time limit", ST77XX_RED);
            break;
          }
          if ((long)(want_end - max_end) > 0) want_end = max_end;
        }
        int run_sec = (int)((want_end - now_ms) / 1000UL);
        pump_in_pause = false;
        pause_by_command = false;
        pump_extension_active = false;
        manual_pump_active = true;          // reuses the manual-mode timer: Button 1 or pump_off stops it, automatic control stands aside
        forced_run = true;
        manual_press_count = 1;
        manual_press_window_start_ms = now_ms - MANUAL_PRESS_WINDOW_MS - 1;   // "add time" window already closed
        manual_pump_end_ms = want_end;
        startPump();
        char msg[96], scr[48];
        snprintf(msg, sizeof(msg), "pump forced ON for %d s (Button 1 or pump_off stops it)", run_sec);
        snprintf(scr, sizeof(scr), "CMD: pump FORCED ON %ds", run_sec);
        respond(p.cmd, true, msg, scr, ST77XX_RED);
      }
      break;

    case SUMP_CMD_DISABLE: {
      pump_disabled = true;
      pump_disabled_until_ms = millis() + (unsigned long)p.hours * 3600000UL;
      manual_pump_active = false;
      manual_press_count = 0;
      stopPump();
      maintenance_mode = false;
      maintenance_extension_active = false;
      pump_in_pause = false;
      pause_by_command = false;
      char msg[96], scr[48];
      snprintf(msg, sizeof(msg), "pump DISABLED for %d h (send enable to cancel; a reset also clears it)", p.hours);
      snprintf(scr, sizeof(scr), "CMD: pump DISABLED %dh", p.hours);
      respond(p.cmd, true, msg, scr, ST77XX_MAGENTA);
      break;
    }

    case SUMP_CMD_ENABLE:
      if (!pump_disabled) { respond(p.cmd, true, "pump was not disabled", "CMD: pump already enabled", ST77XX_CYAN); break; }
      pump_disabled = false;
      respond(p.cmd, true, "pump enabled; automatic control resumes", "CMD: pump ENABLED", ST77XX_GREEN);
      break;

    case SUMP_CMD_SET: {
      SumpLevels L;
      L.trigger = p.has_trigger ? p.trigger : trigger_inches;
      L.off = p.has_off ? p.off : off_inches;
      L.alarm = p.has_alarm ? p.alarm : alarm_inches;
      const char* err = sumpValidateLevels(L);
      if (err) { respond(p.cmd, false, err, "CMD: levels REJECTED", ST77XX_RED); break; }
      // Flash-wear guard: an actual CHANGE is allowed at most once per SET_MIN_INTERVAL_MS (unchanged values are free)
      static uint32_t last_change_ms = 0;
      static bool had_change = false;
      bool differs = fabsf(L.trigger - trigger_inches) > 0.0005f || fabsf(L.off - off_inches) > 0.0005f ||
                     fabsf(L.alarm - alarm_inches) > 0.0005f;
      if (differs && had_change && (uint32_t)(millis() - last_change_ms) < SET_MIN_INTERVAL_MS) {
        respond(p.cmd, false, "refused: wait 10 s between level changes (flash wear)", "CMD refused: too soon", ST77XX_RED);
        break;
      }
      bool changed = applyLevelParams(L);
      if (changed) { last_change_ms = millis(); had_change = true; }
      char extra[96], scr[48];
      snprintf(extra, sizeof(extra), ",\"trigger_in\":%.2f,\"off_in\":%.2f,\"alarm_in\":%.2f", trigger_inches, off_inches, alarm_inches);
      snprintf(scr, sizeof(scr), "CMD: T%.1f O%.2f A%.1f%s", trigger_inches, off_inches, alarm_inches, changed ? " saved" : " same");
      respond(p.cmd, true, changed ? "levels updated and saved to flash" : "no change (flash not written)", scr, ST77XX_GREEN, extra);
      break;
    }

    case SUMP_CMD_SILENCE:
      if (!alarm_active) { respond(p.cmd, true, "no alarm active", "CMD: no alarm active", ST77XX_CYAN); break; }
      alarm_silenced = true;
      alarm_silenced_ms = millis();
      digitalWrite(BEEPER_PIN, LOW);
      respond(p.cmd, true, "alarm beeper silenced (re-sounds after 30 min or on a new problem)", "CMD: alarm silenced", ST77XX_CYAN);
      break;

    case SUMP_CMD_HELP:
      respond(p.cmd, true, "stats | pump_off | pump_on [sec 5-300] | disable [hours 1-24] | enable | silence | set trigger=N off=N alarm=N",
              "CMD: help sent", ST77XX_CYAN);
      break;

    default:
      break;
  }
}

void processCommands() {
  char line[96];
  while (mqPopCmd(line, sizeof(line))) executeCommand(line);
}

#else  // !ENABLE_MQTT : stubs so the rest of the sketch builds unchanged

void mqttBegin() {}
bool mqttIsConnected() { return false; }
void mqttFlushCommands() {}
void mqttPublishEvent(const char*, const char*) {}
void mqttPublishCauseChange(uint8_t, uint8_t) {}
void reportingTick() {}
void processCommands() {}

#endif  // ENABLE_MQTT
