/*
 MiniMe Bot Commands
 Public Commands:
• !weather <zip> — Fetches the weather report for a US ZIP code.
• !temp — Reads the current indoor temperature sensor.
• !sysinfo — Displays system diagnostics (uptime, heap, RSSI, etc.).
• !time — Displays the current bot time.
• !help — Shows this command list.
 Owner-Only Commands:
• !status — Checks WiFi and Discord Gateway connection status.
• !led on / !led off — Controls the RGB NeoPixel LED.
• !set1 on / !set1 off — Controls digital output pin 1.
• !set2 on / !set2 off — Controls digital output pin 2.
• !servo <0-90> — Moves the servo motor to a specific angle.
• !display <text> — Writes custom text to the OLED screen.
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_NeoPixel.h>
#include <driver/ledc.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
// ====== USER CONFIG ======
const char* WIFI_SSID     = "ssid";
const char* WIFI_PASSWORD = "password";
const char* BOT_TOKEN     = "bot token";
const char* WEATHER_API_KEY = " WEATHER_API_KEY";
// ====== OWNER AND CHANNEL IDS ======
const String OWNER_ID_STR        = "OWNER_ID_STR ";
const char*  TEMP_CHANNEL_ID_STR = "TEMP_CHANNEL_ID_ST";  // main channel
const String TARGET_CHANNEL_ID = "TARGET_CHANNEL_ID";
// ====== GPIO CONFIG ======
const int RGB_LED_PIN = 48;
const int PIN_SERVO   = 47;
const int PIN_SET1    = 6;
const int PIN_SET2    = 7;
const int PIN_DS18B20 = 10;
// I2C SH1106
const int PIN_I2C_SDA = 8;
const int PIN_I2C_SCL = 9;
// ====== TIME CONFIG (NTP) ======
WiFiUDP ntpUDP;
const long UTC_OFFSET_SEC = -28800; // Adjust for your local time offset in seconds
NTPClient timeClient(ntpUDP, "pool.ntp.org", UTC_OFFSET_SEC, 60000);
// ====== SCHEDULED & INTERVAL TASKS ======
const unsigned long SYSINFO_INTERVAL_MS = 600000UL; // 10 minutes
unsigned long lastSysInfoMillis = 0;
int lastSentHour = -1;
// ====== DISCORD GATEWAY ======
WebSocketsClient gatewayWS;
WiFiClientSecure httpsClient;
bool gatewayConnected     = false;
bool identified           = false;
int  heartbeatIntervalMs   = 0;
unsigned long lastHeartbeatMillis = 0;
int lastSeq               = 0;
// ====== DISPLAY ======
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
// ====== TEMP SENSOR ======
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);
// ====== NEOPIXEL ======
Adafruit_NeoPixel pixels(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
// ====== DISPLAY UTILS ======
void drawStatus(const String& line1, const String& line2 = "", const String& line3 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, line1.c_str());
  if (line2.length()) u8g2.drawStr(0, 22, line2.c_str());
  if (line3.length()) u8g2.drawStr(0, 34, line3.c_str());
  u8g2.sendBuffer();
}
// ====== SERVO ======
void setupServo() {
  ledc_timer_config_t timer = {
    .speed_mode       = LEDC_LOW_SPEED_MODE,
    .duty_resolution  = LEDC_TIMER_14_BIT,
    .timer_num        = LEDC_TIMER_0,
    .freq_hz          = 50,
    .clk_cfg          = LEDC_AUTO_CLK
  };
  ledc_timer_config(&timer);
  ledc_channel_config_t channel = {
    .gpio_num         = PIN_SERVO,
    .speed_mode       = LEDC_LOW_SPEED_MODE,
    .channel          = LEDC_CHANNEL_0,
    .intr_type        = LEDC_INTR_DISABLE,
    .timer_sel        = LEDC_TIMER_0,
    .duty             = 0,
    .hpoint           = 0
  };
  ledc_channel_config(&channel);
}
void setServoAngle(int angleDeg) {
  if (angleDeg < 0) angleDeg = 0;
  if (angleDeg > 90) angleDeg = 90;
  int pulseUs = 500 + (1500 * angleDeg / 90);
  uint32_t max_duty = (1 << 14) - 1;
  uint32_t duty = (pulseUs * max_duty) / 20000;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
void setupPins() {
  pinMode(RGB_LED_PIN, OUTPUT);
  digitalWrite(RGB_LED_PIN, LOW);
  pixels.begin();
  pixels.show();
  pinMode(PIN_SET1, OUTPUT);
  pinMode(PIN_SET2, OUTPUT);
  digitalWrite(PIN_SET1, LOW);
  digitalWrite(PIN_SET2, LOW);
  setupServo();
  setServoAngle(0);
}
bool readTemperature(float &tempC, float &tempF) {
  sensors.requestTemperatures();
  float c = sensors.getTempCByIndex(0);
  if (c == DEVICE_DISCONNECTED_C) {
    return false;
  }
  tempC = c;
  tempF = c * 9.0f / 5.0f + 32.0f;
  return true;
}
String getSystemInfo() {
  long rssi = WiFi.RSSI();
  uint32_t freeHeap = ESP.getFreeHeap();
  unsigned long uptimeSec = millis() / 1000;
  unsigned long days = uptimeSec / 86400;
  unsigned long hours = (uptimeSec % 86400) / 3600;
  unsigned long minutes = (uptimeSec % 3600) / 60;
  String uptimeStr = String(days) + "d " + String(hours) + "h " + String(minutes) + "m";
  return "📊 **System Diagnostics:**\n"
         "• **Uptime:** " + uptimeStr + "\n"
         "• **Free Heap:** " + String(freeHeap) + " bytes\n"
         "• **WiFi RSSI:** " + String(rssi) + " dBm\n"
         "• **Gateway Status:** " + (gatewayConnected ? "Connected" : "Disconnected");
}
bool sendDiscordMessage(const String& channelId, const String& content) {
  httpsClient.stop();
  httpsClient.setInsecure();
  if (!httpsClient.connect("discord.com", 443)) {
    Serial.println("[REST] HTTPS connect failed");
    return false;
  }
  String url = "/api/v10/channels/" + channelId + "/messages";
  StaticJsonDocument<1024> doc;
  doc["content"] = content;
  doc["tts"] = false;
  String body;
  serializeJson(doc, body);
  String request =
    "POST " + url + " HTTP/1.1\r\n"
    "Host: discord.com\r\n"
    "Authorization: Bot " + String(BOT_TOKEN) + "\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: " + String(body.length()) + "\r\n"
    "Connection: close\r\n\r\n" +
    body;
  httpsClient.print(request);
  unsigned long start = millis();
  while (httpsClient.connected() && millis() - start < 5000) {
    if (httpsClient.available()) {
      String line = httpsClient.readStringUntil('\n');
      if (line == "\r") break;
    }
  }
  httpsClient.stop();
  return true;
}
void sendIdentify() {
  StaticJsonDocument<1024> doc;
  doc["op"] = 2;
  JsonObject d = doc.createNestedObject("d");
  d["token"] = BOT_TOKEN;
  JsonObject props = d.createNestedObject("properties");
  props["os"]     = "linux";
  props["browser"] = "esp32";
  props["device"]  = "esp32";
  d["compress"]         = false;
  d["large_threshold"] = 50;
  d["intents"] = 37377; // GUILDS + GUILD_MESSAGES + DIRECT_MESSAGES + MESSAGE_CONTENT
  String payload;
  serializeJson(doc, payload);
  gatewayWS.sendTXT(payload);
  Serial.println("[GW] IDENTIFY sent");
}
void sendHeartbeat() {
  StaticJsonDocument<256> doc;
  doc["op"] = 1;
  if (lastSeq == 0) {
    doc["d"] = nullptr;
  } else {
    doc["d"] = lastSeq;
  }
  String payload;
  serializeJson(doc, payload);
  gatewayWS.sendTXT(payload);
  Serial.println("[GW] HEARTBEAT sent");
}
bool isOwner(const String& authorId) {
  return authorId == OWNER_ID_STR;
}
bool getWeather(const String& zip, String& outReport) {
  WiFiClient client;
  if (!client.connect("api.openweathermap.org", 80)) {
    outReport = "Weather service connection failed.";
    return false;
  }
  String url = "/data/2.5/weather?zip=" + zip + ",US&units=imperial&appid=" + WEATHER_API_KEY;
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: api.openweathermap.org\r\n" +
               "Connection: close\r\n\r\n");
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000) {
      outReport = "Weather service timeout.";
      client.stop();
      return false;
    }
  }
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, client);
  if (err) {
    outReport = "Weather JSON parse error.";
    return false;
  }
  String city = doc["name"] | "Unknown";
  float tempF = doc["main"]["temp"] | 0.0f;
  float tempC = (tempF - 32.0f) * 5.0f / 9.0f;
  int humidity = doc["main"]["humidity"] | 0;
  String cond = doc["weather"][0]["description"] | "Unknown";
  // Formatted like sysinfo with a cloud icon
  outReport = "☁️ **Weather Report (" + city + " - " + zip + "):**\n" +
              "• **Condition:** " + cond + "\n" +
              "• **Temperature:** " + String(tempF, 1) + "°F (" + String(tempC, 1) + "°C)\n" +
              "• **Humidity:** " + String(humidity) + "%";
              "";
  return true;
}
void handleCommand(const String& content, const String& authorId,
                   const String& channelId, bool isDM)
{
  String cmd = content;
  cmd.trim();
if (cmd.startsWith("!help")) {
    String helpMsg =
      "🤖 **MiniMe Bot Commands**\n\n"
      "**👤 Public Commands:**\n"
      "• `!weather <zip>` — Fetches the weather report for a US ZIP code.\n"
      "• `!temp` — Reads the current indoor temperature sensor.\n"
      "• `!sysinfo` — Displays system diagnostics (uptime, heap, RSSI, etc.).\n"
      "• `!time` — Displays the current bot time.\n"
      "• `!help` — Shows this command list.\n\n"
      "**👑 Owner-Only Commands:**\n"
      "• `!status` — Checks WiFi and Discord Gateway connection status.\n"
      "• `!led on` / `!led off` — Controls the RGB NeoPixel LED.\n"
      "• `!set1 on` / `!set1 off` — Controls digital output pin 1.\n"
      "• `!set2 on` / `!set2 off` — Controls digital output pin 2.\n"
      "• `!servo <0-90>` — Moves the servo motor to a specific angle.\n"
      "• `!display <text>` — Writes custom text to the OLED screen.";
    sendDiscordMessage(channelId, helpMsg);
    drawStatus("Help", "Command Sent");
    return;
  }
  if (cmd.startsWith("!weather")) {
    int spaceIdx = cmd.indexOf(' ');
    if (spaceIdx < 0) {
      sendDiscordMessage(channelId, "Usage: !weather <zip>");
      return;
    }
    String zip = cmd.substring(spaceIdx + 1);
    zip.trim();
    if (zip.length() < 5) {
      sendDiscordMessage(channelId, "Invalid ZIP code.");
      return;
    }
    String report;
    if (getWeather(zip, report)) {
      sendDiscordMessage(channelId, report);
      drawStatus("Weather", zip);
    } else {
      sendDiscordMessage(channelId, report);
      drawStatus("Weather", "Error");
    }
    return;
  }
  if (cmd.startsWith("!temp")) {
    float c, f;
    if (readTemperature(c, f)) {
      String msg = "Current Temp: " + String(c, 1) + "°C / " + String(f, 1) + "°F";
      sendDiscordMessage(channelId, msg);
      drawStatus("Temp", msg);
    } else {
      sendDiscordMessage(channelId, "Temperature sensor error.");
      drawStatus("Temp error");
    }
    return;
  }
  if (cmd.startsWith("!sysinfo")) {
    sendDiscordMessage(channelId, getSystemInfo());
    drawStatus("SysInfo", "Sent");
    return;
  }
  if (cmd.startsWith("!status")) {
    String msg = "Status: WiFi OK, Gateway " + String(gatewayConnected ? "OK" : "DOWN");
    sendDiscordMessage(channelId, msg);
    drawStatus("Status", msg);
    return;
  }
  if (cmd.startsWith("!time")) {
    timeClient.update();
    String currentTime = timeClient.getFormattedTime(); // Format: HH:MM:SS
    String msg = "🕒 Current Bot Time: " + currentTime;
    sendDiscordMessage(channelId, msg);
    drawStatus("Time", currentTime);
    return;
  }
  if (!isOwner(authorId)) {
    if (!isDM) {
      sendDiscordMessage(channelId, "You are not allowed to use this command.");
    }
    return;
  }
  if (cmd.startsWith("!led")) {
    if (cmd.indexOf("on") > 0) {
        pixels.setPixelColor(0, pixels.Color(255, 255, 255));
        pixels.show();
        sendDiscordMessage(channelId, "LED ON");
        drawStatus("LED", "ON");
    } else if (cmd.indexOf("off") > 0) {
        pixels.setPixelColor(0, pixels.Color(0, 0, 0));
        pixels.show();
        sendDiscordMessage(channelId, "LED OFF");
        drawStatus("LED", "OFF");
    } else {
        sendDiscordMessage(channelId, "Usage: !led on/off");
    }
    return;
  }
  if (cmd.startsWith("!set1")) {
    if (cmd.indexOf("on") > 0) {
      digitalWrite(PIN_SET1, HIGH);
      sendDiscordMessage(channelId, "set1 ON");
      drawStatus("set1", "ON");
    } else if (cmd.indexOf("off") > 0) {
      digitalWrite(PIN_SET1, LOW);
      sendDiscordMessage(channelId, "set1 OFF");
      drawStatus("set1", "OFF");
    } else {
      sendDiscordMessage(channelId, "Usage: !set1 on/off");
    }
    return;
  }
  if (cmd.startsWith("!set2")) {
    if (cmd.indexOf("on") > 0) {
      digitalWrite(PIN_SET2, HIGH);
      sendDiscordMessage(channelId, "set2 ON");
      drawStatus("set2", "ON");
    } else if (cmd.indexOf("off") > 0) {
      digitalWrite(PIN_SET2, LOW);
      sendDiscordMessage(channelId, "set2 OFF");
      drawStatus("set2", "OFF");
    } else {
      sendDiscordMessage(channelId, "Usage: !set2 on/off");
    }
    return;
  }
  if (cmd.startsWith("!servo")) {
    int spaceIdx = cmd.indexOf(' ');
    if (spaceIdx < 0) {
      sendDiscordMessage(channelId, "Usage: !servo <0-90>");
      return;
    }
    String arg = cmd.substring(spaceIdx + 1);
    arg.trim();
    int angle = arg.toInt();
    if (angle < 0 || angle > 90) {
      sendDiscordMessage(channelId, "Angle out of range. Allowed: 0–90 degrees.");
      return;
    }
    setServoAngle(angle);
    String msg = "Servo set to " + String(angle) + " degrees.";
    sendDiscordMessage(channelId, msg);
    drawStatus("Servo", msg);
    return;
  }
  if (cmd.startsWith("!display")) {
    int spaceIdx = cmd.indexOf(' ');
    if (spaceIdx < 0) {
      sendDiscordMessage(channelId, "Usage: !display <text>");
      return;
    }
    String text = cmd.substring(spaceIdx + 1);
    text.trim();
    drawStatus("Display:", text);
    sendDiscordMessage(channelId, "Display updated.");
    return;
  }
}
void gatewayEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      gatewayConnected = false;
      identified       = false;
      Serial.println("[GW] DISCONNECTED");
      drawStatus("Gateway", "Disconnected");
      break;
    case WStype_CONNECTED:
      gatewayConnected = true;
      Serial.println("[GW] CONNECTED");
      drawStatus("Gateway", "Connected");
      break;
    case WStype_TEXT: {
      StaticJsonDocument<4096> doc;
      DeserializationError err = deserializeJson(doc, payload, length);
      if (err) return;
      int op = doc["op"] | -1;
      if (doc.containsKey("s") && !doc["s"].isNull()) {
        lastSeq = doc["s"].as<int>();
      }
      if (op == 10) {
        heartbeatIntervalMs = doc["d"]["heartbeat_interval"] | 0;
        lastHeartbeatMillis = millis();
        sendIdentify();
        identified = true;
        return;
      }
      if (op == 11) return;
      if (op == 0) {
        const char* t = doc["t"];
        if (!t) return;
        if (strcmp(t, "MESSAGE_CREATE") == 0) {
          JsonObject d = doc["d"];
          if (d["author"]["bot"] == true) return;
          String content   = d["content"].as<String>();
          String channelId = d["channel_id"].as<String>();
          String authorId  = d["author"]["id"].as<String>();
          bool isDM = d["guild_id"].isNull();
          handleCommand(content, authorId, channelId, isDM);
        }
      }
      break;
    }
    default:
      break;
  }
}
void backgroundTasks() {
  unsigned long now = millis();
  // 1. Every 10 Minutes: System Health Report
  if (now - lastSysInfoMillis >= SYSINFO_INTERVAL_MS || lastSysInfoMillis == 0) {
    lastSysInfoMillis = now;
    sendDiscordMessage(TARGET_CHANNEL_ID, getSystemInfo());
  }
  // 2. Scheduled Daily Summaries (6 AM, 12 PM, 6 PM)
  timeClient.update();
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  if ((currentHour == 6 || currentHour == 12 || currentHour == 18) && currentMinute == 0) {
    if (lastSentHour != currentHour) {
      lastSentHour = currentHour;
      float c, f;
      if (readTemperature(c, f)) {
        String report = "⏰ **Scheduled Summary (" + String(currentHour) + ":00):**\n" +
                        "• **Indoor Temp:** " + String(c, 1) + "°C / " + String(f, 1) + "°F";
        sendDiscordMessage(TARGET_CHANNEL_ID, report);
        drawStatus("Scheduled", "Report Sent");
      } else {
        sendDiscordMessage(TARGET_CHANNEL_ID, "⏰ **Scheduled Summary:** Temperature sensor error.");
      }
    }
  } else {
    if (lastSentHour != -1 && currentHour != 6 && currentHour != 12 && currentHour != 18) {
      lastSentHour = -1;
    }
  }
}
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  drawStatus("WiFi", "Connecting...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  drawStatus("WiFi", "Connected");
}
void connectGateway() {
  gatewayWS.beginSSL("gateway.discord.gg", 443, "/?v=10&encoding=json");
  gatewayWS.onEvent(gatewayEvent);
  gatewayWS.setReconnectInterval(5000);
}
void setup() {
  Serial.begin(115200);
  delay(50);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  u8g2.begin();
  drawStatus("Booting...", "ESP32-S3 Discord bot");
  setupPins();
  sensors.begin();
  connectWiFi();
  timeClient.begin();
  connectGateway();
  drawStatus("Ready", "Idle presence");
}
void loop() {
  gatewayWS.loop();
  if (heartbeatIntervalMs > 0 && gatewayConnected && identified) {
    unsigned long now = millis();
    if (now - lastHeartbeatMillis >= (unsigned long)heartbeatIntervalMs) {
      lastHeartbeatMillis = now;
      sendHeartbeat();
    }
  }
  backgroundTasks();
  delay(5);
}
