# Lessons Learned - OTA, credentials, and on-screen version (2026-11-25)

Notes from the session that pushed the code to GitHub, got wireless (OTA) updates working again, and added
a firmware version string to the display. Written so the next person (or the next Claude session) does not
have to rediscover any of it. No passwords are recorded here; see `secrets.h.example` for the setup.

## 1. Where things stand

| Item | State |
|---|---|
| Repository | `github.com:awardblvr/SumpPumpMonitorController`, single branch `main` |
| History | One squash commit (`ca5ba8f`) contains the WiFi/OTA firmware, the `secrets.h` split and the docs. The temporary `WiFi_Updating` branch was deleted locally and on GitHub |
| Running firmware | `FW_VERSION` **1.0**, uploaded over WiFi from the Arduino IDE, ESP32 core **4.0.0** |
| Device | `SumpMonitor` at 192.168.1.62 (DHCP), OTA on UDP/TCP 3232, password required |
| Credentials | Never in git. Real values live in the git-ignored `secrets.h`; the committed history only ever held placeholders |

## 2. Credentials (`secrets.h`)

- The sketch does `#include "secrets.h"` and reads `SECRET_WIFI_SSID`, `SECRET_WIFI_PASSWORD`,
  `SECRET_OTA_HOSTNAME`, `SECRET_OTA_PASSWORD`.
- `secrets.h` is listed in `.gitignore`. `secrets.h.example` is the committed template. A fresh clone must
  copy the example to `secrets.h` and fill it in, or the sketch will not compile.
- `.gitignore` also covers `.DS_Store`, `.idea/` and `*_Backups*` (the `AW_Backups/` folder used for
  safety copies before edits).

## 3. OTA: what worked and what did not

**Works:** uploading from **Arduino IDE** to the network port "SumpMonitor at 192.168.1.62".

**Fails (cause never found):** running `espota.py` (or any Python/Node socket) from a terminal on the
development Mac. It dies at once with `Sending invitation ... failed / Host ... Not Found`; Python reports
`OSError 65, No route to host` for UDP and TCP to 192.168.1.62 only. Facts gathered:

- `ping`, `nc`, and `sudo traceroute -P TCP -p 3232` all reach the device, so the network and the OTA
  listener are fine (a TCP connect to 3232 is *refused*, which is normal because invitations are UDP).
- Python reached the router (192.168.1.1) fine; only this device failed, with every interpreter tried
  (Homebrew 3.15, Apple's 3.9) and with sockets pinned to either network interface (WiFi `en0`,
  Thunderbolt Ethernet `en17`, both on 192.168.1.x).
- Ruled out: the Claude Code command sandbox (same failure in the user's own tmux pane and a plain iTerm
  tab) and macOS Local Network permission (iTerm and Arduino IDE were both switched on).
- Not tested: the corporate VPN, which was connected the whole time and is the only unusual factor left.
- The separate lab Mac is on the 17.x corporate network and cannot reach 192.168.1.x at all.
- Conclusion used in practice: do OTA from the Arduino IDE.

**"Authentication Failed" in the IDE** (both `PBKDF2-HMAC-SHA256` and `MD5` attempts fail) means the password
sent was not the device's. The IDE dialog wants `SECRET_OTA_PASSWORD` and nothing else. The IDE **caches**
what it was given, so a wrong value keeps failing across restarts. Fix (worked 2025-11-10 and 2026-11-25):

1. Quit Arduino IDE completely.
2. Delete `~/Library/Application Support/arduino-ide/Local Storage/leveldb`.
3. Reopen the IDE. It has also forgotten the board and port: **Tools → Board → esp32 → Adafruit Feather
   ESP32-S2 TFT**, then re-check the board options (below), then **Tools → Port → SumpMonitor at ...**.
4. Upload and enter the OTA password at the dialog.

The verbose upload line prints the password after `--auth=`; avoid pasting it anywhere shared.

**"Timeout waiting for result (attempt 1/10)"** after `Uploading ... 100% Done` is normal-ish: the device
reboots as it finishes. Verify by the on-screen version, not by that message.

## 4. Build settings that matter

- **Partition Scheme must be "Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)"**. After the IDE data was
  cleared it showed `tinyuf2_noota` (No OTA). OTA-only uploads do not rewrite the partition table, but a
  USB upload with the No-OTA scheme would, and OTA would stop working.
- The IDE reports `Maximum is 1310720 bytes` with the right scheme (2883584 with the wrong one). The current
  build is 1,106,768 bytes = 84% of the OTA slot, so there is about 200 KB of headroom.
- **The esp32 core changed under us:** the compile check used core 3.3.10 (1,040,444 bytes); the IDE then
  upgraded to **4.0.0** and the device now runs that build. It booted and shows the version, but the rest
  of the behaviour (level reading, relays, NeoPixels, WiFi) was not re-verified. Rollback: Boards Manager →
  esp32 → 3.3.10, then upload again.
- Command-line compile check used during the session (build output kept out of the repo):
  ```
  arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s2_tft:PartitionScheme=default \
    --libraries ~/ArduinoLocal/libraries --build-path <temp dir> ~/ArduinoLocal/SumpMonitorController
  ```

## 5. On-screen firmware version

- `#define FW_VERSION "1.0"` near the top of `SumpMonitorController.ino`. **Bump it before every upload.**
- Idle screen: white, text size 1, right-justified directly under the clock: x = 240 - 6 x length,
  y = 10. The Level line (size 2) ends well left of x = 186 and the next line starts at y = 25, so nothing
  overlaps. It is drawn outside the `time_synced` check, so it shows even before NTP sync.
- Boot screen: `debugPrint("Sump Monitor v" FW_VERSION, ...)` (it used to say a hard-coded "v2.0").
- Numbering: `1.0` is the first build that displays a version. The README's old `v1.0`-`v2.9` history
  entries are documentation revisions from 2025 and are a different sequence.

## 6. Hookup diagram notes

- `docs/SumpControllerESP32-hookup_Reverse.png` (back of the board, 2025-11-20) is the latest wiring picture.
  Its A4 and A5 labels both say "GPIO8"; the correct mapping is A4 = GPIO14, A5 = GPIO8. The "Relay - HAI"
  arrow is ambiguous between D12 and D13 (the code uses GPIO12 for K1 and GPIO13 for K2).
- There is no KiCad schematic for this project.

## 7. Working-environment gotchas (this Mac / Claude Code)

- `grep` is a shell function in this account and can swallow output; use `/usr/bin/grep` in scripts and
  tool calls.
- Claude Code's shell here is bash 3.2 and refuses compound commands with nested `$(...)`; run simple
  commands one at a time or put them in a script file.
- Verbose IDE output and some of this session's chat logs contained a typed password. If you ever paste
  upload output, trim the `--auth=` value first.
- The device's WiFi link is marginal (about -72 dBm on the display; one ping took 850 ms). A better
  access-point position or a DHCP reservation for 192.168.1.62 would make OTA more reliable.

## 8. Open items

- `docs/README.md` was updated with these lessons (the previous text is in git history). `README.pdf` was
  removed because it was a stale 2025 export; regenerate one from `README.md` if you want a PDF again.
- `TASK_CONTEXT.md` (in `~/ArduinoLocal/temp_AI_files/SumpMonitorController/`) still describes firmware
  "2.0", old line numbers and the old credentials layout.
- Consider replacing the default OTA password in `secrets.h` and re-uploading.
- Re-check normal operation (level, relays, NeoPixels, WiFi, NTP clock) now that the build uses core 4.0.0.

The step-by-step log of this session is in `ClaudeLog.md` in the repository root.
