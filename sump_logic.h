// sump_logic.h - pure logic for the Sump Monitor (no Arduino dependencies).
//
// Kept in its own header so it can be compiled and unit-tested on a normal computer
// (see temp_AI_files/SumpMonitorController/host_test/). Everything here is deterministic:
// time is passed in as a millis()-style 32-bit counter, and all comparisons are
// wrap-safe unsigned subtractions.
//
// Contents:
//   1. Level parameter validation        (trigger / off / alarm inches)
//   2. MQTT command-line parsing         ("pump_on 120", "set trigger=5.5 off=1")
//   3. SumpReporter                      (baseline vs. "water is rising" reporting schedule)
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// 1. Level parameters
// ---------------------------------------------------------------------------------------------
struct SumpLevels {
  float trigger;  // inches: automatic pump starts at or above this
  float off;      // inches: pump begins its stop extension at or below this
  float alarm;    // inches: high-water alarm above this (clears 0.5 in lower)
};

// Returns NULL when the combination is acceptable, otherwise a short reason.
// Written as !(condition) so that NaN is rejected too.
// The limits keep a stray or mistaken "set" from producing a pump that short-cycles on residual water
// (off too low / trigger too close to off) or an alarm that cannot fire (alarm near the sensor's 18 in range).
inline const char* sumpValidateLevels(const SumpLevels& L) {
  if (!(L.off >= 0.5f && L.off <= 6.0f)) return "off must be 0.5 to 6.0 in";
  if (!(L.trigger >= 2.0f && L.trigger <= 12.0f)) return "trigger must be 2.0 to 12.0 in";
  if (!(L.trigger >= L.off + 1.0f)) return "trigger must be at least off + 1.0 in";
  if (!(L.alarm >= L.trigger + 1.0f)) return "alarm must be at least trigger + 1.0 in";
  if (!(L.alarm <= 17.0f)) return "alarm must be 17.0 in or less";
  return NULL;
}

// ---------------------------------------------------------------------------------------------
// 2. Command parsing
// ---------------------------------------------------------------------------------------------
enum SumpCmd {
  SUMP_CMD_NONE = 0,
  SUMP_CMD_STATS,
  SUMP_CMD_PUMP_OFF,
  SUMP_CMD_PUMP_ON,
  SUMP_CMD_DISABLE,
  SUMP_CMD_ENABLE,
  SUMP_CMD_SET,
  SUMP_CMD_SILENCE,
  SUMP_CMD_HELP,
  SUMP_CMD_UNKNOWN
};

#define SUMP_FORCE_MAX_SEC 300       // a forced run never exceeds 5 minutes
#define SUMP_FORCE_MIN_SEC 5
#define SUMP_DISABLE_MAX_HOURS 24    // pump disable is capped at 24 hours
#define SUMP_DISABLE_MIN_HOURS 1

struct SumpCmdParsed {
  SumpCmd cmd;
  int seconds;            // pump_on: run time, default and maximum SUMP_FORCE_MAX_SEC
  int hours;              // disable: default and maximum SUMP_DISABLE_MAX_HOURS
  bool has_trigger, has_off, has_alarm;
  float trigger, off, alarm;
  const char* err;        // NULL when the line parsed cleanly
};

// Parses a full-string float: rejects empty strings, trailing junk, NaN and absurd values.
inline bool sumpParseFloat(const char* s, float* out) {
  if (!s || !*s) return false;
  char* end = NULL;
  float v = strtof(s, &end);
  if (end == s || *end != '\0') return false;
  if (!(v > -1000000.0f && v < 1000000.0f)) return false;  // also false for NaN
  *out = v;
  return true;
}

inline bool sumpParseInt(const char* s, int* out) {
  if (!s || !*s) return false;
  char* end = NULL;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0') return false;
  if (v < -1000000L || v > 1000000L) return false;
  *out = (int)v;
  return true;
}

// Splits `s` in place on whitespace/commas and lower-cases it. Returns the token count (<= maxTok).
inline int sumpTokenize(char* s, char** tok, int maxTok) {
  int n = 0;
  char* p = s;
  while (*p && n < maxTok) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
    if (!*p) break;
    tok[n++] = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != ',') {
      if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
      p++;
    }
    if (*p) *p++ = '\0';
  }
  return n;
}

inline SumpCmd sumpCmdFromWord(const char* w) {
  if (!strcmp(w, "stats") || !strcmp(w, "dump")) return SUMP_CMD_STATS;
  if (!strcmp(w, "pump_off") || !strcmp(w, "stop")) return SUMP_CMD_PUMP_OFF;
  if (!strcmp(w, "pump_on") || !strcmp(w, "force_on")) return SUMP_CMD_PUMP_ON;
  if (!strcmp(w, "disable") || !strcmp(w, "pump_disable")) return SUMP_CMD_DISABLE;
  if (!strcmp(w, "enable") || !strcmp(w, "pump_enable")) return SUMP_CMD_ENABLE;
  if (!strcmp(w, "set") || !strcmp(w, "set_levels")) return SUMP_CMD_SET;
  if (!strcmp(w, "silence") || !strcmp(w, "mute")) return SUMP_CMD_SILENCE;
  if (!strcmp(w, "help")) return SUMP_CMD_HELP;
  return SUMP_CMD_UNKNOWN;
}

// Parses one command line (modified in place). Accepted forms (case-insensitive):
//   stats | pump_off | pump_on [seconds] | disable [hours] | enable | silence | help
//   set trigger=5.5 off=1.0 alarm=10     (any subset; unspecified values keep their current setting)
// Bare "on" / "off" are deliberately NOT accepted, so a Home Assistant switch with default ON/OFF
// payloads can never start or stop the pump by accident.
inline void sumpParseCommand(char* line, SumpCmdParsed* out) {
  memset(out, 0, sizeof(*out));
  out->seconds = SUMP_FORCE_MAX_SEC;
  out->hours = SUMP_DISABLE_MAX_HOURS;
  out->err = NULL;

  char* tok[8];
  int n = sumpTokenize(line, tok, 8);
  if (n == 0) { out->cmd = SUMP_CMD_NONE; return; }

  out->cmd = sumpCmdFromWord(tok[0]);
  if (out->cmd == SUMP_CMD_UNKNOWN) { out->err = "unknown command (send: help)"; return; }

  switch (out->cmd) {
    case SUMP_CMD_PUMP_ON:
      if (n >= 2) {
        const char* a = tok[1];
        if (!strncmp(a, "seconds=", 8)) a += 8;
        int v;
        if (!sumpParseInt(a, &v)) { out->err = "pump_on: seconds must be a whole number"; return; }
        if (v < SUMP_FORCE_MIN_SEC || v > SUMP_FORCE_MAX_SEC) { out->err = "pump_on: seconds must be 5 to 300"; return; }
        out->seconds = v;
      }
      break;
    case SUMP_CMD_DISABLE:
      if (n >= 2) {
        const char* a = tok[1];
        if (!strncmp(a, "hours=", 6)) a += 6;
        int v;
        if (!sumpParseInt(a, &v)) { out->err = "disable: hours must be a whole number"; return; }
        if (v < SUMP_DISABLE_MIN_HOURS || v > SUMP_DISABLE_MAX_HOURS) { out->err = "disable: hours must be 1 to 24"; return; }
        out->hours = v;
      }
      break;
    case SUMP_CMD_SET:
      if (n < 2) { out->err = "set: give trigger=, off= and/or alarm="; return; }
      for (int i = 1; i < n; i++) {
        char* eq = strchr(tok[i], '=');
        if (!eq) { out->err = "set: use key=value (trigger, off, alarm)"; return; }
        *eq = '\0';
        const char* key = tok[i];
        const char* val = eq + 1;
        float f;
        if (!sumpParseFloat(val, &f)) { out->err = "set: value is not a number"; return; }
        if (!strcmp(key, "trigger")) { out->trigger = f; out->has_trigger = true; }
        else if (!strcmp(key, "off")) { out->off = f; out->has_off = true; }
        else if (!strcmp(key, "alarm")) { out->alarm = f; out->has_alarm = true; }
        else { out->err = "set: unknown key (use trigger, off, alarm)"; return; }
      }
      break;
    default:
      break;  // stats, pump_off, enable, silence, help take no arguments (extra tokens ignored)
  }
}

// ---------------------------------------------------------------------------------------------
// 3. Reporting schedule
// ---------------------------------------------------------------------------------------------
// Normal state: one report every `baselineMs`.
// "Elevated" state: one report every `elevatedMs`, entered when the water level starts rising
// (up by `riseDelta` inches within the last ~10 minutes) or any alarm cause is present.
// Elevated ends when things have been calm for `calmMs` (no alarm, pump idle, level at or below
// `normalLevel`, not rising), and is forcibly ended after `maxElevatedMs` (6 h). After that forced
// end, elevated reporting cannot restart until a full calm period has been seen (otherwise a
// condition that never clears would simply re-enter at once and the 6 h limit would mean nothing).
//
// Call update() often (every loop pass). It returns true when a report should be sent right now.
// Reports triggered by other events (pump started, alarm raised...) should call noteReport().
#define SUMP_HIST_N 20  // samples kept: 20 x sampleMs (30 s) = 10 minutes

struct SumpReporter {
  // configuration (set by init)
  uint32_t baselineMs, elevatedMs, maxElevatedMs, calmMs, sampleMs;
  float riseDelta, normalLevel;

  // state
  bool elevated;
  bool lockout;           // true after a forced 6 h end, until a calm period is seen
  bool calmTiming;
  uint32_t elevatedSince, calmSince, lastReport, lastSample;
  bool haveReport, haveSample;
  float hist[SUMP_HIST_N];
  uint8_t histCount, histHead;

  void init(uint32_t baseline_ms, uint32_t elevated_ms, uint32_t max_elevated_ms, uint32_t calm_ms,
            uint32_t sample_ms, float rise_delta, float normal_level) {
    baselineMs = baseline_ms; elevatedMs = elevated_ms; maxElevatedMs = max_elevated_ms;
    calmMs = calm_ms; sampleMs = sample_ms; riseDelta = rise_delta; normalLevel = normal_level;
    elevated = false; lockout = false; calmTiming = false;
    elevatedSince = calmSince = lastReport = lastSample = 0;
    haveReport = haveSample = false;
    histCount = histHead = 0;
    for (int i = 0; i < SUMP_HIST_N; i++) hist[i] = 0.0f;
  }

  void noteReport(uint32_t now) { lastReport = now; haveReport = true; }

  // True when the level has risen by riseDelta or more above the lowest recent sample.
  bool rising(float level) const {
    if (histCount < 2) return false;
    float lo = hist[0];
    for (uint8_t i = 1; i < histCount; i++) if (hist[i] < lo) lo = hist[i];
    return (level - lo) >= riseDelta;
  }

  uint32_t elevatedAgeMs(uint32_t now) const { return elevated ? (now - elevatedSince) : 0; }

  // level: inches (ignored when !levelValid); abnormal: any alarm cause; pumpRunning: pump relay on.
  bool update(uint32_t now, float level, bool levelValid, bool abnormal, bool pumpRunning) {
    if (levelValid && (!haveSample || (uint32_t)(now - lastSample) >= sampleMs)) {
      lastSample = now;
      haveSample = true;
      hist[histHead] = level;
      histHead = (uint8_t)((histHead + 1) % SUMP_HIST_N);
      if (histCount < SUMP_HIST_N) histCount++;
    }

    bool isRising = levelValid && rising(level);
    bool calmNow = levelValid && !abnormal && !pumpRunning && !isRising && level <= normalLevel;
    if (calmNow) {
      if (!calmTiming) { calmTiming = true; calmSince = now; }
    } else {
      calmTiming = false;
    }
    bool calmSustained = calmTiming && (uint32_t)(now - calmSince) >= calmMs;

    bool due = false;

    if (!elevated) {
      if (lockout && calmSustained) lockout = false;
      if (!lockout && (isRising || abnormal)) {
        elevated = true;
        elevatedSince = now;
        due = true;  // report immediately on entering
      }
    } else {
      if ((uint32_t)(now - elevatedSince) >= maxElevatedMs) {
        elevated = false;
        lockout = true;
        due = true;  // one last report as we drop back to the baseline schedule
      } else if (calmSustained) {
        elevated = false;
        due = true;  // report that things are back to normal
      }
    }

    uint32_t interval = elevated ? elevatedMs : baselineMs;
    if (!haveReport || (uint32_t)(now - lastReport) >= interval) due = true;

    if (due) noteReport(now);
    return due;
  }
};
