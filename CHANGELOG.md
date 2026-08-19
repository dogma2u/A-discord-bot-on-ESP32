# Changelog

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
