/*
  ┌──────────────────────────────────────────────────────────────────────────┐
  │   M5StickC Plus 1.1 + ENV III (SHT30 + QMP6988)                          │
  │   Clock + Temp/Humidity + Pressure (+ Altitude) + Stocks (NTP optional)  │
  └──────────────────────────────────────────────────────────────────────────┘

  File      : main.ino
  Author    : Renan Duarte
  Created   : 2025-09-18
  Version   : 1.0.0
  License   : MIT (see end of file or choose your preferred license)

  Overview:
    • Displays temperature (°C), humidity (%RH), battery level (%)
    • Shows pressure and computed altitude (m) using QNH from METAR (EDDB)
    • Clock/date via NTP and timezone set to NASDAQ time (EST5EDT)
    • Simple stock quote viewer (NVDA / ASTS) from a custom HTTP JSON API
    • Button A toggles the stock symbol, Button B forces a refresh
    • Beeps on button actions

  Hardware:
    • Board  : M5StickC Plus 1.1 (ESP32)
    • Unit   : M5Unit ENV III  — SHT30 (temp/humidity), QMP6988 (pressure)
    • Display: Built-in TFT (landscape: rotation=3)
    • Audio  : Built-in speaker for short tones
    • I2C    : Uses M5 defaults via M5UnitENV helper (no manual pin config)

  Networking & Time (optional):
    • Wi-Fi SSID/PASS configured in this file
    • NTP server: pool.ntp.org
    • TZ string : "EST5EDT,M3.2.0,M11.1.0" (U.S. Eastern, with DST)

  Data Sources:
    • QNH (sea-level pressure) fetched from NOAA METAR for EDDB:
      https://tgftp.nws.noaa.gov/data/observations/metar/stations/EDDB.TXT
      Parsed token "Qxxxx" → QNH in hPa.
    • Stock quotes from HTTP JSON endpoint (URL + secret in this file).

  Altitude Calculation:
    • Uses barometric formula (standard atmosphere, up to ~11 km):
        h = 44330 * (1 - (P / P0)^(1/5.255))
      where:
        P  = local pressure in hPa (sensor Pa ÷ 100)
        P0 = QNH (sea-level pressure) in hPa
        h  = altitude in meters

  Refresh & UI:
    • Screen/data refresh interval: 45 s (DRAW_INTERVAL_MS)
    • Top bar: "T C / H% / Battery%" and "Pressure / Altitude"
    • Main area: selected stock LAST, then BID/ASK, and timestamp (YYYY-MM-DD HH:MM)

  Buttons & Sounds:
    • Btn A → toggle stock symbol (NVDA ↔ ASTS), beep tone A
    • Btn B → force refresh, beep tone B

  Dependencies (Arduino / M5Stack ecosystem):
    • M5Unified
    • M5UnitENV
    • ArduinoJson
    • WiFi / HTTPClient / time.h (bundled with ESP32 core)

  Build Notes:
    • Target board: M5StickC Plus
    • Set correct USB/serial port and upload speed in Arduino IDE or PlatformIO
    • Wi-Fi power save is disabled (WiFi.setSleep(false)) for responsiveness

  Known Notes / Gotchas:
    • Labeling: this sketch computes pressure in hPa (Pa ÷ 100) for display,
      ensure the on-screen unit matches the value shown.
    • METAR parsing: code looks for "Qxxxx". If your station uses "Axxxx"
      (inHg*100), add a conversion path.

  Changelog:
    • 1.0.0 (2025-09-18) — Initial version by Renan Duarte.

  MIT License (short form):
    Copyright (c) 2025 Renan Duarte
    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, subject to the following conditions:
    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
    "AS IS", WITHOUT WARRANTY OF ANY KIND.
*/


#include <M5Unified.h>
#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <WebServer.h>
#include "M5UnitENV.h"

// Stock object def
class Stock {
  public:float last, bid, ask;
  public:double volume;
  public:String symbol, lastTradeTime, deltaIndicator;
  Stock() {
    Set("");
  }
  public:void Set(String symbol_val) {
    symbol = symbol_val;
    last = NAN;
    bid = NAN;
    ask = NAN;
    volume = NAN;
    lastTradeTime = "";
    deltaIndicator = "";
  }
  public:void Debug() {
    Serial.print(symbol + " (" + deltaIndicator + ") - last:");
    Serial.print(last, 2);  // print with 2 decimals
    Serial.print(", bid:");
    Serial.print(bid, 2);  // print with 2 decimals
    Serial.print(", ask:");
    Serial.print(ask, 2);  // print with 2 decimals
    Serial.print(", volume: ");
    Serial.print(volume, 0); // print with 0 decimals
    Serial.print(" (" + lastTradeTime + ")");
    Serial.println("");
  }
};

// ====== AWS
const String AWS_URL = "https://*";
const String AWS_SEC = "*";

// ====== Wi-Fi / NTP (optional) ======
const char* WIFI_SSID = "*";
const char* WIFI_PASS = "*";
const char* NTP_SERVER = "pool.ntp.org";

// beeper notes
const int BEEP_NOTE_A = 661;
const int BEEP_NOTE_B = 112;

// Timezones
const char* TZ_NY = "EST5EDT,M3.2.0,M11.1.0";  // NASDAQ time

// ====== ENV III sensors ======
SHT3X sht30;  // temp/humidity
QMP6988 qmp;  // pressure

// ====== Display settings ======
const uint32_t DRAW_INTERVAL_MS = 45*1000;
uint32_t lastDraw = -DRAW_INTERVAL_MS;

// ====== Altitude calc ======
const float SEA_LEVEL_HPA = 1013.25f;

// hardware and logic plus others...
StaticJsonDocument<5096> doc;
bool hardwareStarted = false;
struct tm tinfo;
bool settingStock = false;
WebServer server(80);
Stock stock;

void connectWiFiIfConfigured() {

  // debug...
  Serial.println("Starting WiFi...");

  if (strlen(WIFI_SSID) == 0) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wifi is connected!");
    Serial.println(WiFi.localIP());
    Serial.println("Configuring NTP and Time...");
    configTzTime(TZ_NY, NTP_SERVER);
    // Wait briefly for NTP
    for (int i = 0; i < 30; ++i) {
      if (getLocalTime(&tinfo)) {
        Serial.println("NTP connection has been made.");
        char buffer[64];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tinfo);
        Serial.println(buffer);
        break;
      }
      delay(200);
    }

    // avoid sleeping
    WiFi.setSleep(false);
  } else {
    Serial.println("Wifi is not connected...");
  }
}

bool initENV() {
  Serial.println("Starting Sensors...");
  if (!qmp.begin(&Wire, QMP6988_SLAVE_ADDRESS_L, 0, 26, 400000U)) {
    Serial.println("Couldn't find QMP6988");
    while (1) delay(1);
  }
  if (!sht30.begin(&Wire, SHT3X_I2C_ADDR, 0, 26, 400000U)) {
    Serial.println("Couldn't find SHT3X");
    while (1) delay(1);
  }
  Serial.println("Sensors where configured...");
  return true;
}

void initDisplay() {
  Serial.println("Starting Display...");
  // init display
  M5.Display.fillScreen(TFT_BLACK);  // clear screen
  M5.Display.setCursor(0, 0);
  M5.Display.setRotation(3);         // 3 = landscape, 0 = portrait
  // font config
  M5.Display.setTextSize(2);  // font size multiplier
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  // Startup Text :)
  // Clear whole screen
  M5.Display.fillRect(0, 0, M5.Display.width(), M5.Display.height(), TFT_BLACK);

  // creating buffers
  char tbuf1[20], tbuf2[20];
  // printing and storing buffers
  snprintf(tbuf1, sizeof(tbuf1), "Stock");
  snprintf(tbuf2, sizeof(tbuf2), "Watcher");

  // printing pseudo Logo
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextSize(4.0);  // big clock
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(tbuf1, M5.Display.width() / 2, 40);
  M5.Display.drawString(tbuf2, M5.Display.width() / 2, 80);

  // debug...
  Serial.println("Display is started.");
}
void initWebServer() {
  Serial.println("trying to start web server and config OTA...");
  // webserver and OTA...
  server.on("/", []() {
    server.send(200, "text/plain", stock.lastTradeTime);
  });
  ElegantOTA.begin(&server);
  server.begin();
  
  Serial.println("webserver is completed.");
}
void setup() {

  // board init
  auto cfg = M5.config();
  M5.begin(cfg);
  delay(100);

  // display setup
  initDisplay();

  // speaker
  M5.Speaker.begin();

  // serial init
  Serial.begin(115200);
  for (int i=0; i<10; i++) {
    Serial.println("........");
    delay(250);
  }

  // Optional NTP
  connectWiFiIfConfigured();

  // sensor setup
  initENV();

  // init web server and ota... etc
  initWebServer();

  // finished
  hardwareStarted = true;
}

float fetchQNH() {
  // verify wifi first
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return SEA_LEVEL_HPA;
  }

  HTTPClient http;
  http.begin("https://tgftp.nws.noaa.gov/data/observations/metar/stations/EDDB.TXT");
  // validate http code first
  int httpCode = http.GET();
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      // Look for "Qxxxx" inside the METAR
      int idx = payload.indexOf(" Q");
      if (idx == -1) idx = payload.indexOf("\nQ"); // sometimes newline
      if (idx > 0) {
        String qnhStr = payload.substring(idx + 2, idx + 6);
        float qnh = qnhStr.toFloat(); // in hPa
        Serial.printf("QNH: %.1f hPa\n", qnh);
        return qnh;
      } else {
        Serial.println("No QNH found in METAR");
      }
    }
  } else {
    Serial.printf("HTTP error: %s\n", http.errorToString(httpCode).c_str());
  }  
  http.end();
  return SEA_LEVEL_HPA;
}

bool fetchPayloadFromInternet() {
  // verify wifi first
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return false;
  }

  String url = AWS_URL + "?sec_key=" + AWS_SEC;

  Serial.println("fetching stock price from Internet (AWS) api...");
  Serial.println(url);

  HTTPClient http;

  if (!http.begin(url)) {
    Serial.println("http.begin() FAILED");
    return false;
  }

  // setting headers
  http.addHeader("User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/139.0.0.0 Safari/537.36");
  http.addHeader("Upgrade-Insecure-Requests", "1");
  http.addHeader("Accept", "application/json, text/plain, */*");

  int httpCode = http.GET();

  Serial.printf("httpCode: %d", httpCode);
  Serial.println("");

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[HTTP] error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }

  // parsing it
  String payload = http.getString();
  http.end();
  return parsePayload(payload);
}

bool parsePayload(String payload) {
  doc.clear();
  auto err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("Error in deserializeJson...");
    return false;
  } else {
    Serial.println("JSON has been parsed...");
    return true;
  }
}

bool getStockPrice(String symbol) {
  if (fetchPayloadFromInternet()) {
    // reset
    stock.Set(symbol);
    // persist
    stock.last = doc[symbol]["last"].as<float>();
    stock.bid = doc[symbol]["bid"].as<float>();
    stock.ask = doc[symbol]["ask"].as<float>();
    stock.volume = doc[symbol]["volume"].as<double>();
    stock.deltaIndicator = doc[symbol]["deltaIndicator"].as<String>();
    stock.lastTradeTime = doc[symbol]["lastTradeTime"].as<String>();
    stock.Debug();
    return true;
  }
  return false;
}

void drawTopBar(float tempC, float hum, float pressure, float altitude) {

  Serial.println("Drawing Top Bar...");
  // Top bar background
  M5.Display.fillRect(0, 0, M5.Display.width(), M5.Display.height(), TFT_BLACK);

  // Small text centered line with T/RH
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextSize(1.9);
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);

  String line = "";
  line += isnan(tempC) ? "--" : String(tempC, 1);
  line += " C H:";
  line += isnan(hum) ? "--" : String(hum, 0);
  line += "%";
  line += " B:" + String(M5.Power.getBatteryLevel()) + "%";

  M5.Display.drawString(line, M5.Display.width() / 2, 11);

  // Small text centered line with T/RH
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_BLUE, TFT_BLACK);

  String line2 = "";
  line2 += isnan(pressure) ? "--" : String(pressure, 1);
  line2 += " Pa  A: ";
  line2 += isnan(altitude) ? "--" : String(altitude, 0);
  line2 += "m";

  M5.Display.drawString(line2, M5.Display.width() / 2, 30);
}


void drawClockAndDate() {

  // sync time
  Serial.println("Fetching time/clock data...");
  getLocalTime(&tinfo);

  // stock price
  String symbol;
  if (settingStock) {
    symbol = "ASTS";
  } else {
    symbol = "NVDA";
  }
 
  if (!getStockPrice(symbol)) {
    Serial.println("error fetching stock data...");
  }

  // Clear main area except top bar
  M5.Display.fillRect(0, 50, M5.Display.width(), M5.Display.height() - 50, TFT_BLACK);

  // creating buffers
  char tbuf1[20], tbuf2[20], tbuf3[20];
  // printing and storing buffers
  snprintf(tbuf1, sizeof(tbuf1), "%s:%.2f", symbol, stock.last);
  snprintf(tbuf2, sizeof(tbuf2), "%.2f / %.2f", stock.bid, stock.ask);
  snprintf(tbuf3, sizeof(tbuf3), "%04d-%02d-%02d %02d:%02d", tinfo.tm_year + 1900, tinfo.tm_mon + 1, tinfo.tm_mday, tinfo.tm_hour, tinfo.tm_min);

  // printing LAST
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextSize(3.5);  // big clock
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(tbuf1, M5.Display.width() / 2, 66);

  // printing BID and ASK
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextSize(2.0);  // big clock
  if (stock.deltaIndicator == "up") {
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  } else {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
  }
  M5.Display.drawString(tbuf2, M5.Display.width() / 2, 90);

  // Small date at bottom
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.drawString(tbuf3, M5.Display.width() / 2, 118);
}

void drawScreen() {

  Serial.println("Drawing screen...");
  // variables
  float tempC = NAN, hum = NAN;
  float pressPa = NAN, pressHpa = NAN, altitude = NAN;

  // temperature sensor
  Serial.println("Fetching SHT30 data...");
  if (sht30.update()) {
    tempC = sht30.cTemp;
    hum = sht30.humidity;
    Serial.println(tempC);
    Serial.println(hum);
  }
  // pressure sensor
  Serial.println("Fetching QMP data...");
  if (qmp.update()) {
    pressPa = qmp.calcPressure();
    Serial.println(pressPa);
    // conversions
    Serial.println("Converting Pressure...");
    pressHpa = (pressPa > 0) ? pressPa / 100.0f : NAN;
    Serial.println(pressHpa);
    // Altitude
    Serial.println("Calculating Altitude...");
    if (!isnan(pressHpa) && pressHpa > 0) {
      float seaLevel = fetchQNH();
      altitude = 44330.0f * (1.0f - pow(pressHpa / seaLevel, 1.0f / 5.255f));
      Serial.println(altitude);
    }
  }

  // drawing data
  drawTopBar(tempC, hum, pressHpa, altitude);
  drawClockAndDate();

  Serial.println("Screen refresh is completed.");
}

void loop() {
  // system loop
  if (hardwareStarted) {
    // handlers
    M5.update();
    server.handleClient();
    ElegantOTA.loop();
    // check for the last stock refresh :)
    uint32_t now = millis();
    bool refreshData = now - lastDraw >= DRAW_INTERVAL_MS;

    // check button A, if pressed, then switch stocks
    if (M5.BtnA.wasPressed()) {
      Serial.println("Button A was pressed.");
      M5.Speaker.tone(BEEP_NOTE_A, 200);
      settingStock = !settingStock;
      refreshData = true;
    }

    //  check button B, if pressed, force data & screen refresh
    if (M5.BtnB.wasPressed()) {
      Serial.println("Button B was pressed.");
      M5.Speaker.tone(BEEP_NOTE_B, 200);
      refreshData = true;
    }

    // only if needed
    if (refreshData) {
      lastDraw = now;
      // refreshing screen and data, otherwise, sleep
      drawScreen();
    } else {
      delay(100);
    }
  } else {
    // waiting for system wake up
    delay(1000);
  }
}
