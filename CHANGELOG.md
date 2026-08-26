# Changelog

Older sections describe that release as shipped. Current firmware and docs are **0.4.28** (see `VERSION` and README).

## 0.4.28

- Owner `!led`: keep `on` / `off`; add `!led <r> <g> <b>` (integers **0–255**) on the onboard RGB NeoPixel (GPIO 48, `NEO_GRB`). `on` is white 255 255 255. Helpers: `setLedRgb`, `parseRgbTriplet`.
- Mid-line `!led` keeps multi-word RGB args (same as `!ask` / `!display`). Discord `!help` / README use `!led on/off` wording.
- Firmware polish: `gwSendJson`, `drawDashBar`, `findFreeTrackedSlot`, `uptimeDhms`, HTTP open connect/timeout codes, dashboard user rows without `String names[]`, `sendDiscordMessage` via `httpsAwaitHeaders`, drop `botDiscordStatusSent` (status `0` = unset).
- CHANGELOG 0.4.13 mid-line note corrected for `!led`.

## 0.4.27

- Firmware cleanup: strip dead Serial debug and unused helpers; trim sketch comment noise (command list lives in Discord `!help`).
- Shared HTTPS/HTTP open helpers, fetch-command helpers, `discordDisplayName`, and tracked-user slot clear/fill.
- Owner/channel IDs as `const char*`; shared `boardMemTotals()` for OLED heap bar and `!sysinfo`.

## 0.4.26

- Remove unused `dashLastCmd`, `dashLastEvent`, `dashLastCmdMillis`, `TEMP_CHANNEL_ID_STR`, `drawTransient()`, `debugTouchSerial()`, `TOUCH_DEBUG_MS`, and `lastTouchDebugMillis`.
- README matches current firmware: poll-only OLED wake (no touch interrupt, no Serial touch debug); `!ask` Discord cap **2000** (not 3600); presence updates do not wake the OLED; `TOUCH_THRESHOLD` default **2000**; CPU 80 MHz when OLED is off and bot is Idle.

## 0.4.25

- OLED: uptime and temp on row 3; Sig / Heap / Srv shift down one row. User rows 7–14 and rows 15–16 unchanged.

## 0.4.24

- DS18B20 on GPIO 10: enable `INPUT_PULLUP` at boot and before each temperature read.
- README breadboard photo now includes the DS18B20 (`docs/minime-breadboard-v2.jpg`).

## 0.4.23

- OLED sleep: wake only when compensated touch is at or above trip. Ignore sticky hardware IRQ status, and attach the interrupt at the trip point (not 0) so idle below trip can dim/off.

## 0.4.22

- Comment out Serial (test logging off). Touch trip gap remains 2000.

## 0.4.21

- Touch trip gap default is **2000**.

## 0.4.20

- CPU 80 MHz when OLED is off and Discord bot status is Idle; 240 MHz otherwise. Serial remains test-only.

## 0.4.19

- Serial: dedicated `usb power:` line with VBUS millivolts and volts (divider on GPIO 1).

## 0.4.18

- Serial 115200 on; touch init and raw/comp/idle/trip/usb/touched lines every 500 ms.

## 0.4.17

- README touch tuning: lower `TOUCH_THRESHOLD` if the pad is hard to trigger; raise it if it false-triggers.

## 0.4.16

- Touch trip gap default is **450** (was 3500) to match a ~400–500 raw delta on this pad.

## 0.4.15

- Touch cal: 16-sample rolling idle average; trip is a constant gap above that average. USB VBUS ADC on GPIO 1 (10k/10k divider) scales touch raw so USB voltage does not walk the trigger. `!sysinfo` shows VBUS mV and idle/trip.

## 0.4.14

- Add `LICENSE` (MIT for original MiniMe files only; third-party libraries and APIs stay under their own terms). README points to it.

## 0.4.13

- Mid-sentence one-word args work for most commands (e.g. `… !weather 90210 …`). `!ask` / `!display` / `!led` keep multi-word args for the rest of the line.

## 0.4.12

- Commands may appear anywhere in a message (e.g. `Should I !ask what time is it.`); they are handled the same as if they started the line.

## 0.4.11

- Touch pad still wakes the OLED; it no longer sets Discord presence to Online.

## 0.4.10

- OLED row 14: `Bot:Online` / `Bot:Idle  ` left; fixed-slot `Www Mmm dd YYYY` right (space-padded day, DOW does not shift).

## 0.4.9

- Restore command handling that had regressed: lowercase the command word only (args like `!display` text keep their case).
- Unknown `!` commands reply `That is not a command.` (normal chat without `!` is ignored).
- `!weather` accepts only a 5-digit US ZIP; anything else returns `Invalid ZIP code.`

## 0.4.8

- OLED row 5 combines uptime and temp as fixed-width `Up:xxxxdxxhxxm T:xxxF/xxxC` (space-padded; sensor fail shows `T:--Error--`).
- User rows shift up to rows 6–13; row 14 left open.

## 0.4.7

- Fix `!ask` silent failures: Discord 2000-char post limit, larger REST JSON buffer, HTTP status check, HTTPS busy lock while DeepSeek runs, short fallback if the reply post fails.

## 0.4.6

- Boot auto `!sysinfo` waits until Gateway is connected and identified (no more false "Disconnected").

## 0.4.5

- Bot Discord presence: Online on commands, scheduled posts, and touch; Idle after 5 minutes quiet (Gateway OP 3).
- Serial logging commented out (including touch debug in `loop`) to reduce CPU load.

## 0.4.4

- Capacitive touch wake on GPIO 4: tap pad to turn the OLED back on after dim/off (1 min idle, 15 s fade).
- README: touch pad wiring, calibration, Serial debug, and threshold tuning. Display sleep timing matches firmware.
- Sketch section headers for user tracking, touch wake, Discord REST, and setup/loop.

## 0.4.3

- `!ask` is queued from the Gateway callback; DeepSeek HTTPS runs from `loop()` with heartbeats pumped so Discord stays connected.

## 0.4.2

- OLED: eight user rows; `Srv:` bar (0–90°, boot at 45°); command / `!display` text on rows 15–16 (dashboard is not wiped).
- `!display` is public; payload only, 50 characters (25 + 25), 6 seconds, overwrite restarts the timer.
- Member names load from `BOT_GUILD_ID` and both command-channel guilds (two servers).
- `!ask`: `max_tokens` 900, 12288-byte JSON parse, 3600-character Discord post.
- `!sysinfo` includes a GitHub firmware URL with Discord link embeds suppressed.
- `!help` and README command lists are alphabetical. README matches the current dashboard.

## 0.4.1

- Dashboard user stats: seven rows (was five).
- Sketch section comments for each major block (config, OLED, users, APIs, Gateway, setup/loop).
- README: 0.4.1 notes, U8g2 baseline (no setCursor), OLED sleep does not power down MCU or Wi-Fi, member load is 7.

## 0.4.0

- SSD1327 128×128 dashboard: MiniMe header, gateway, Pacific time, Sig/Heap bars, temp, uptime, five users with presence and 24h command counts.
- Startup REST member load (`BOT_GUILD_ID` / channel guild resolve) plus Presence Intent for On / Idle / DND / Off.
- OLED sleeps after 10 minutes with no real events (Sig/time/heap ticks do not count); commands, presence changes, and gateway messages wake it.
- Public `!ask`; `!status` is not in this firmware. Commands work in DMs and two allowed channels.
- US Pacific DST time, 8MB PSRAM gateway JSON, `!sysinfo` heap includes PSRAM.
- README rewrite, breadboard photo, no secrets in the GitHub sketch.

## 0.3.2

- Make `!ask` owner-only; update README and help text.

## 0.3.1

- Fix `!ask` DeepSeek parsing: handle chunked HTTP bodies, refuse gzip, clearer errors.

## 0.3.0

- Public `!ask <question>` via DeepSeek chat API (`DEEPSEEK_API_KEY`).

## 0.2.0

- Public science commands: `!news` (space/high-tech headlines), `!physics` (arXiv), `!apod` (NASA), `!iss` (ISS position).

## 0.1.1

- Minor firmware comment and formatting tweaks.

## 0.1.0

- Initial MiniMe ESP32-S3 Discord bot firmware (commands, weather, sensors, GPIO, OLED, scheduled reports).
- README for Discord token, weather API key, owner ID, and channel IDs.
