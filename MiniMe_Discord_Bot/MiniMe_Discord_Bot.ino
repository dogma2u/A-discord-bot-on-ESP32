/*
 MiniMe Bot Commands
 Public Commands:
• !weather <zip> — Fetches the weather report for a US ZIP code.
• !temp — Reads the current indoor temperature sensor.
• !sysinfo — Displays system diagnostics (uptime, heap, RSSI, etc.).
• !time — Displays the current bot time.
• !news — Space and high-tech science headlines.
• !physics — Latest arXiv physics papers.
• !apod — NASA Astronomy Picture of the Day.
• !iss — Current International Space Station position.
• !help — Shows this command list.
 Owner-Only Commands:
• !status — Checks WiFi and Discord Gateway connection status.
• !ask <question> — Asks DeepSeek (text AI reply in chat).
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
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";
const char* BOT_TOKEN     = "";
const char* WEATHER_API_KEY = "";
const char* NASA_API_KEY    = "";
const char* DEEPSEEK_API_KEY = ""; // from https://platform.deepseek.com/api_keys

// ====== OWNER AND CHANNEL IDS ======
const String OWNER_ID_STR        = "";
const char*  TEMP_CHANNEL_ID_STR = "";  
const String TARGET_CHANNEL_ID = "";

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
bool skipHttpHeaders(Client& client, unsigned long timeoutMs) {
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > timeoutMs) {
      return false;
    }
  }
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) return true;
    if (millis() - timeout > timeoutMs) return false;
  }
  return true;
}
String collapseWhitespace(String s) {
  s.replace("\n", " ");
  s.replace("\r", " ");
  s.replace("\t", " ");
  while (s.indexOf("  ") >= 0) {
    s.replace("  ", " ");
  }
  s.trim();
  return s;
}
String truncateText(const String& s, int maxLen) {
  if (s.length() <= maxLen) return s;
  return s.substring(0, maxLen - 3) + "...";
}
bool getScienceNews(String& outReport) {
  httpsClient.stop();
  httpsClient.setInsecure();
  if (!httpsClient.connect("api.spaceflightnewsapi.net", 443)) {
    outReport = "Science news connection failed.";
    return false;
  }
  httpsClient.print(
    "GET /v4/articles/?limit=3 HTTP/1.1\r\n"
    "Host: api.spaceflightnewsapi.net\r\n"
    "User-Agent: MiniMeBot/1.0\r\n"
    "Connection: close\r\n\r\n");
  if (!skipHttpHeaders(httpsClient, 8000)) {
    outReport = "Science news timeout.";
    httpsClient.stop();
    return false;
  }
  StaticJsonDocument<192> filter;
  filter["results"][0]["title"] = true;
  filter["results"][0]["news_site"] = true;
  filter["results"][0]["url"] = true;
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, httpsClient, DeserializationOption::Filter(filter));
  httpsClient.stop();
  if (err) {
    outReport = "Science news JSON parse error.";
    return false;
  }
  JsonArray results = doc["results"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) {
    outReport = "No science headlines right now.";
    return false;
  }
  outReport = "🛰️ **Space & high-tech headlines:**\n";
  int n = 0;
  for (JsonObject item : results) {
    if (n >= 3) break;
    String title = collapseWhitespace(item["title"] | "Untitled");
    String site = item["news_site"] | "Source";
    String url = item["url"] | "";
    outReport += String(n + 1) + ". **" + truncateText(title, 140) + "** (" + site + ")";
    if (url.length()) outReport += "\n" + url;
    outReport += "\n";
    n++;
  }
  return n > 0;
}
bool getPhysicsPapers(String& outReport) {
  WiFiClient client;
  if (!client.connect("export.arxiv.org", 80)) {
    outReport = "arXiv connection failed.";
    return false;
  }
  client.print(
    "GET /api/query?search_query=cat:physics&start=0&max_results=3&sortBy=submittedDate&sortOrder=descending HTTP/1.1\r\n"
    "Host: export.arxiv.org\r\n"
    "User-Agent: MiniMeBot/1.0 (ESP32 Discord bot)\r\n"
    "Connection: close\r\n\r\n");
  if (!skipHttpHeaders(client, 8000)) {
    outReport = "arXiv timeout.";
    client.stop();
    return false;
  }
  String xml;
  unsigned long start = millis();
  while (millis() - start < 8000) {
    while (client.available()) {
      xml += client.readString();
      if (xml.length() > 24000) break;
    }
    if (!client.connected() && !client.available()) break;
    delay(10);
  }
  client.stop();
  if (xml.length() < 50) {
    outReport = "arXiv response empty.";
    return false;
  }
  outReport = "⚛️ **Latest arXiv physics papers:**\n";
  int from = 0;
  int n = 0;
  while (n < 3) {
    int entry = xml.indexOf("<entry>", from);
    if (entry < 0) break;
    int entryEnd = xml.indexOf("</entry>", entry);
    if (entryEnd < 0) break;
    String block = xml.substring(entry, entryEnd);
    int t0 = block.indexOf("<title>");
    int t1 = block.indexOf("</title>");
    String title = "Untitled";
    if (t0 >= 0 && t1 > t0) {
      title = collapseWhitespace(block.substring(t0 + 7, t1));
    }
    int i0 = block.indexOf("<id>");
    int i1 = block.indexOf("</id>");
    String id = "";
    if (i0 >= 0 && i1 > i0) {
      id = collapseWhitespace(block.substring(i0 + 4, i1));
    }
    outReport += String(n + 1) + ". **" + truncateText(title, 140) + "**";
    if (id.length()) outReport += "\n" + id;
    outReport += "\n";
    n++;
    from = entryEnd + 8;
  }
  if (n == 0) {
    outReport = "No physics papers found.";
    return false;
  }
  return true;
}
bool getApod(String& outReport) {
  httpsClient.stop();
  httpsClient.setInsecure();
  if (!httpsClient.connect("api.nasa.gov", 443)) {
    outReport = "NASA APOD connection failed.";
    return false;
  }
  String path = String("/planetary/apod?api_key=") + NASA_API_KEY;
  httpsClient.print(String("GET ") + path + " HTTP/1.1\r\n" +
                    "Host: api.nasa.gov\r\n" +
                    "User-Agent: MiniMeBot/1.0\r\n" +
                    "Connection: close\r\n\r\n");
  if (!skipHttpHeaders(httpsClient, 8000)) {
    outReport = "NASA APOD timeout.";
    httpsClient.stop();
    return false;
  }
  StaticJsonDocument<128> filter;
  filter["title"] = true;
  filter["explanation"] = true;
  filter["date"] = true;
  filter["url"] = true;
  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, httpsClient, DeserializationOption::Filter(filter));
  httpsClient.stop();
  if (err) {
    outReport = "NASA APOD JSON parse error.";
    return false;
  }
  String title = doc["title"] | "Astronomy Picture of the Day";
  String date = doc["date"] | "";
  String expl = collapseWhitespace(doc["explanation"] | "");
  String url = doc["url"] | "";
  outReport = "🌌 **NASA APOD";
  if (date.length()) outReport += " (" + date + ")";
  outReport += ":**\n• **" + title + "**\n" + truncateText(expl, 350);
  if (url.length()) outReport += "\n" + url;
  return true;
}
bool getIssPosition(String& outReport) {
  WiFiClient client;
  if (!client.connect("api.open-notify.org", 80)) {
    outReport = "ISS tracker connection failed.";
    return false;
  }
  client.print(
    "GET /iss-now.json HTTP/1.1\r\n"
    "Host: api.open-notify.org\r\n"
    "Connection: close\r\n\r\n");
  if (!skipHttpHeaders(client, 5000)) {
    outReport = "ISS tracker timeout.";
    client.stop();
    return false;
  }
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, client);
  client.stop();
  if (err) {
    outReport = "ISS tracker JSON parse error.";
    return false;
  }
  String lat = doc["iss_position"]["latitude"] | "?";
  String lon = doc["iss_position"]["longitude"] | "?";
  outReport = "🌍 **ISS now:**\n"
              "• **Latitude:** " + lat + "\n"
              "• **Longitude:** " + lon;
  return true;
}
bool readHttpBodyAfterHeaders(Client& client, bool chunked, int contentLength,
                              String& outBody, unsigned long deadlineMs) {
  outBody = "";
  const size_t maxBody = 12000;
  if (chunked) {
    while (millis() < deadlineMs) {
      while (!client.available() && client.connected() && millis() < deadlineMs) {
        delay(5);
      }
      if (!client.available()) break;
      String sizeLine = client.readStringUntil('\n');
      sizeLine.trim();
      if (sizeLine.length() == 0) continue;
      int sc = sizeLine.indexOf(';');
      if (sc >= 0) sizeLine = sizeLine.substring(0, sc);
      long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize <= 0) break;
      long got = 0;
      while (got < chunkSize && millis() < deadlineMs) {
        if (client.available()) {
          outBody += (char)client.read();
          got++;
          if (outBody.length() >= maxBody) return true;
        } else if (!client.connected()) {
          break;
        } else {
          delay(1);
        }
      }
      client.readStringUntil('\n');
    }
    return outBody.length() > 0;
  }
  if (contentLength > 0) {
    while ((int)outBody.length() < contentLength && millis() < deadlineMs) {
      while (client.available()) {
        outBody += (char)client.read();
        if ((int)outBody.length() >= contentLength || outBody.length() >= maxBody) break;
      }
      if (!client.connected() && !client.available()) break;
      delay(5);
    }
    return outBody.length() > 0;
  }
  while (millis() < deadlineMs) {
    while (client.available()) {
      outBody += (char)client.read();
      if (outBody.length() >= maxBody) return true;
    }
    if (!client.connected() && !client.available()) break;
    delay(10);
  }
  return outBody.length() > 0;
}
bool askDeepSeek(const String& question, String& outReport) {
  if (DEEPSEEK_API_KEY == nullptr || strlen(DEEPSEEK_API_KEY) == 0) {
    outReport = "DeepSeek API key not set. Add DEEPSEEK_API_KEY in the sketch.";
    return false;
  }
  String q = question;
  q.trim();
  if (q.length() == 0) {
    outReport = "Usage: !ask <question>";
    return false;
  }
  if (q.length() > 500) {
    q = q.substring(0, 500);
  }
  StaticJsonDocument<1536> req;
  req["model"] = "deepseek-chat";
  req["max_tokens"] = 220;
  req["temperature"] = 0.7;
  req["stream"] = false;
  JsonArray messages = req.createNestedArray("messages");
  JsonObject sys = messages.createNestedObject();
  sys["role"] = "system";
  sys["content"] = "You are MiniMe on an ESP32 Discord bot. Answer clearly and briefly for science, tech, and physics. Keep replies under 200 words.";
  JsonObject user = messages.createNestedObject();
  user["role"] = "user";
  user["content"] = q;
  String body;
  serializeJson(req, body);

  httpsClient.stop();
  httpsClient.setInsecure();
  httpsClient.setTimeout(25000);
  if (!httpsClient.connect("api.deepseek.com", 443)) {
    outReport = "DeepSeek connection failed.";
    return false;
  }
  String request =
    "POST /chat/completions HTTP/1.1\r\n"
    "Host: api.deepseek.com\r\n"
    "Authorization: Bearer " + String(DEEPSEEK_API_KEY) + "\r\n"
    "Content-Type: application/json\r\n"
    "Accept: application/json\r\n"
    "Accept-Encoding: identity\r\n"
    "User-Agent: MiniMeBot/1.0\r\n"
    "Content-Length: " + String(body.length()) + "\r\n"
    "Connection: close\r\n\r\n" +
    body;
  httpsClient.print(request);

  unsigned long deadline = millis() + 30000UL;
  while (httpsClient.available() == 0) {
    if (millis() > deadline) {
      outReport = "DeepSeek timeout waiting for headers.";
      httpsClient.stop();
      return false;
    }
    delay(10);
  }
  String statusLine = httpsClient.readStringUntil('\n');
  statusLine.trim();
  bool chunked = false;
  int contentLength = -1;
  while (millis() <= deadline) {
    String line = httpsClient.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) {
      chunked = true;
    }
    if (lower.startsWith("content-length:")) {
      contentLength = lower.substring(lower.indexOf(':') + 1).toInt();
    }
  }

  String respBody;
  if (!readHttpBodyAfterHeaders(httpsClient, chunked, contentLength, respBody, deadline)) {
    httpsClient.stop();
    outReport = "DeepSeek empty response. " + statusLine;
    return false;
  }
  httpsClient.stop();

  int jsonStart = respBody.indexOf('{');
  if (jsonStart < 0) {
    outReport = "DeepSeek: no JSON body. " + truncateText(statusLine, 80);
    return false;
  }
  if (jsonStart > 0) {
    respBody = respBody.substring(jsonStart);
  }

  StaticJsonDocument<128> filter;
  filter["choices"][0]["message"]["content"] = true;
  filter["error"]["message"] = true;
  StaticJsonDocument<6144> doc;
  DeserializationError err = deserializeJson(doc, respBody, DeserializationOption::Filter(filter));
  if (err) {
    outReport = "DeepSeek JSON parse error (" + String(err.c_str()) + ").";
    return false;
  }
  if (doc.containsKey("error")) {
    String emsg = doc["error"]["message"] | "API error";
    outReport = "DeepSeek error: " + truncateText(emsg, 200);
    return false;
  }
  String answer = collapseWhitespace(doc["choices"][0]["message"]["content"] | "");
  if (answer.length() == 0) {
    // Fallback: crude extract if filter missed nested shape
    int ckey = respBody.indexOf("\"content\"");
    if (ckey >= 0) {
      int colon = respBody.indexOf(':', ckey);
      int q1 = respBody.indexOf('"', colon + 1);
      if (q1 >= 0) {
        String extracted;
        for (int i = q1 + 1; i < (int)respBody.length(); i++) {
          char c = respBody[i];
          if (c == '\\' && i + 1 < (int)respBody.length()) {
            char n = respBody[i + 1];
            if (n == 'n') { extracted += ' '; i++; continue; }
            if (n == '"' || n == '\\') { extracted += n; i++; continue; }
          }
          if (c == '"') break;
          extracted += c;
          if (extracted.length() > 1800) break;
        }
        answer = collapseWhitespace(extracted);
      }
    }
  }
  if (answer.length() == 0) {
    outReport = "DeepSeek returned an empty answer. " + truncateText(statusLine, 60);
    return false;
  }
  outReport = "🧠 **DeepSeek:**\n" + truncateText(answer, 1800);
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
      "• `!news` — Space and high-tech science headlines.\n"
      "• `!physics` — Latest arXiv physics papers.\n"
      "• `!apod` — NASA Astronomy Picture of the Day.\n"
      "• `!iss` — Current International Space Station position.\n"
      "• `!help` — Shows this command list.\n\n"
      "**👑 Owner-Only Commands:**\n"
      "• `!status` — Checks WiFi and Discord Gateway connection status.\n"
      "• `!ask <question>` — Asks DeepSeek (text AI reply in chat).\n"
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
  if (cmd.startsWith("!news")) {
    String report;
    if (getScienceNews(report)) {
      sendDiscordMessage(channelId, report);
      drawStatus("News", "Sent");
    } else {
      sendDiscordMessage(channelId, report);
      drawStatus("News", "Error");
    }
    return;
  }
  if (cmd.startsWith("!physics")) {
    String report;
    if (getPhysicsPapers(report)) {
      sendDiscordMessage(channelId, report);
      drawStatus("Physics", "Sent");
    } else {
      sendDiscordMessage(channelId, report);
      drawStatus("Physics", "Error");
    }
    return;
  }
  if (cmd.startsWith("!apod")) {
    String report;
    if (getApod(report)) {
      sendDiscordMessage(channelId, report);
      drawStatus("APOD", "Sent");
    } else {
      sendDiscordMessage(channelId, report);
      drawStatus("APOD", "Error");
    }
    return;
  }
  if (cmd.startsWith("!iss")) {
    String report;
    if (getIssPosition(report)) {
      sendDiscordMessage(channelId, report);
      drawStatus("ISS", "Sent");
    } else {
      sendDiscordMessage(channelId, report);
      drawStatus("ISS", "Error");
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
  if (cmd.startsWith("!ask")) {
    int spaceIdx = cmd.indexOf(' ');
    if (spaceIdx < 0) {
      sendDiscordMessage(channelId, "Usage: !ask <question>");
      return;
    }
    String question = cmd.substring(spaceIdx + 1);
    question.trim();
    String report;
    drawStatus("DeepSeek", "Thinking...");
    if (askDeepSeek(question, report)) {
      sendDiscordMessage(channelId, report);
      drawStatus("DeepSeek", "Sent");
    } else {
      sendDiscordMessage(channelId, report);
      drawStatus("DeepSeek", "Error");
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
