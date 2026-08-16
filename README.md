# A Discord Bot on ESP32

MiniMe is firmware for an **ESP32-S3** that runs a Discord bot on the chip itself. It connects to Wi‑Fi, joins the Discord Gateway, reads sensors, and drives hardware from chat commands.

**Do not put real tokens, API keys, or IDs in GitHub.** Keep those only in your local copy of `MiniMe_Discord_Bot.ino`.

---

## What this bot can do

### Public commands (anyone in a channel the bot can see)

| Command | What it does |
|---|---|
| `!weather <zip>` | US ZIP weather from OpenWeatherMap (condition, °F/°C, humidity) |
| `!temp` | Indoor temperature from the DS18B20 sensor |
| `!sysinfo` | Uptime, free heap, Wi‑Fi RSSI, gateway status |
| `!time` | Current bot time from NTP |
| `!help` | Lists all commands |

### Owner-only commands

Only the Discord user ID in `OWNER_ID_STR` can use these. Other users in a server channel get “You are not allowed to use this command.”

| Command | What it does |
|---|---|
| `!status` | Wi‑Fi and Discord Gateway status |
| `!led on` / `!led off` | RGB NeoPixel on GPIO 48 |
| `!set1 on` / `!set1 off` | Digital output GPIO 6 |
| `!set2 on` / `!set2 off` | Digital output GPIO 7 |
| `!servo <0-90>` | Servo on GPIO 47 |
| `!display <text>` | Custom text on the SH1106 OLED |

### Automatic posts

These go to `TARGET_CHANNEL_ID` without anyone typing a command:

- **Every 10 minutes:** system diagnostics (`!sysinfo` style)
- **Daily at 6:00, 12:00, and 18:00** (bot local time): indoor temperature summary

Time uses NTP (`pool.ntp.org`) and `UTC_OFFSET_SEC` (default `-28800` = UTC−8).

---

## Fill in these values in the sketch

Open `MiniMe_Discord_Bot/MiniMe_Discord_Bot.ino` and replace the placeholders:

```cpp
const char* WIFI_SSID     = "ssid";
const char* WIFI_PASSWORD = "password";
const char* BOT_TOKEN     = "bot token";
const char* WEATHER_API_KEY = "WEATHER_API_KEY";

const String OWNER_ID_STR        = "OWNER_ID_STR";
const char*  TEMP_CHANNEL_ID_STR = "TEMP_CHANNEL_ID_ST";  // reserved / main channel
const String TARGET_CHANNEL_ID = "TARGET_CHANNEL_ID";
```

| Field | Used for |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | ESP32 station Wi‑Fi |
| `BOT_TOKEN` | Discord Gateway + REST messages |
| `WEATHER_API_KEY` | OpenWeatherMap `!weather` |
| `OWNER_ID_STR` | Who can run hardware commands |
| `TARGET_CHANNEL_ID` | Auto sysinfo and scheduled summaries |
| `TEMP_CHANNEL_ID_STR` | Placeholder for a “main” channel ID (not used by commands yet) |

IDs are **digits only**, no quotes in Discord itself. Paste them as strings in the sketch, for example `"123456789012345678"`.

---

## How to get a Discord bot token (`BOT_TOKEN`)

1. Open [Discord Developer Portal](https://discord.com/developers/applications) and sign in.
2. **New Application** → name it (for example MiniMe) → Create.
3. Left sidebar: **Bot**.
4. If there is no bot yet, click **Add Bot**.
5. Under **Token**, click **Reset Token** / **Copy**. That string is `BOT_TOKEN`.
6. Treat it like a password. Anyone with it can control the bot.
7. Enable these **Privileged Gateway Intents** on the Bot page (this firmware uses them):
   - **Message Content Intent**
   - **Server Members Intent** is not required for this sketch
   - Presence is not required
8. Also turn on:
   - **Message Content Intent**
   - The sketch identifies with intents `37377` (guilds, guild messages, DMs, message content).

### Invite the bot to your server

1. Developer Portal → your app → **OAuth2** → **URL Generator**.
2. Scopes: `bot`.
3. Bot permissions (minimum that matches this firmware):
   - View Channels
   - Send Messages
   - Read Message History
4. Copy the generated URL, open it in a browser, pick your server, authorize.
5. In Discord, confirm the bot appears offline until the ESP32 connects.

---

## How to get your owner ID (`OWNER_ID_STR`)

This is **your Discord user ID**, not the bot’s ID.

1. Discord: **User Settings** → **Advanced** → enable **Developer Mode**.
2. Right-click **your own avatar** (in a server member list, a DM, or your profile) → **Copy User ID**.
3. Paste that into `OWNER_ID_STR`.

If owner commands never work, you copied a channel ID or the bot’s application ID by mistake.

---

## How to get channel IDs (`TARGET_CHANNEL_ID`, `TEMP_CHANNEL_ID_STR`)

1. Developer Mode must be on (same as above).
2. Right-click the **text channel** where you want auto reports → **Copy Channel ID**.
3. Put that in `TARGET_CHANNEL_ID`.
4. Optionally copy another channel for `TEMP_CHANNEL_ID_STR` if you plan to use it later.

The bot must be able to **see and send** in that channel (channel permissions + invite permissions).

---

## How to get an OpenWeatherMap key (`WEATHER_API_KEY`)

1. Create a free account at [OpenWeatherMap](https://home.openweathermap.org/users/sign_up).
2. Sign in → [API keys](https://home.openweathermap.org/api_keys).
3. Copy the default key, or generate one.
4. Paste it into `WEATHER_API_KEY` with **no extra spaces**.
5. New keys can take up to a few hours to activate.
6. `!weather` uses Current Weather Data with `zip={zip},US` and `units=imperial`.

---

## Hardware (default pins)

| Device | GPIO |
|---|---|
| RGB NeoPixel | 48 |
| Servo | 47 |
| Digital out 1 (`!set1`) | 6 |
| Digital out 2 (`!set2`) | 7 |
| DS18B20 data | 10 |
| OLED SH1106 SDA | 8 |
| OLED SH1106 SCL | 9 |

Board target: **ESP32-S3**. Change pins in the sketch if your wiring differs.

---

## Arduino IDE setup

1. Install [Arduino IDE](https://www.arduino.cc/en/software) and the **esp32** board package (Espressif).
2. Board: an ESP32-S3 module that matches your hardware.
3. Libraries (Library Manager):
   - WebSockets (by Markus Sattler)
   - ArduinoJson
   - U8g2
   - OneWire
   - DallasTemperature
   - Adafruit NeoPixel
   - NTPClient
4. Open `MiniMe_Discord_Bot/MiniMe_Discord_Bot.ino`.
5. Fill in Wi‑Fi, token, weather key, and IDs **locally**.
6. Upload. Serial monitor 115200: look for `[GW] CONNECTED` and `[GW] IDENTIFY sent`.
7. In Discord, try `!help`.

---

## Safety

- Never commit a sketch that contains a live bot token or API key.
- If a token leaks, reset it in the Developer Portal immediately.
- Owner commands move a servo and drive GPIO. Only put your user ID in `OWNER_ID_STR`.
