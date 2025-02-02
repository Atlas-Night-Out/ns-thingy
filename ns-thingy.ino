#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "Free_Fonts.h"

//–––––– SD Card Configuration ––––––
#define SD_CLK_PIN    14
#define SD_CMD_PIN    15 
#define SD_D0_PIN     16
#define SD_D1_PIN     18
#define SD_D2_PIN     17 
#define SD_D3_PIN     21

#define PIN_NEOPIXEL  38

//–––––– TFT_eSPI Objects ––––––
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft); 
TFT_eSprite trd = TFT_eSprite(&tft);

//–––––– Global configuration variables ––––––
String wifiSSID;
String wifiPassword;
String nightscoutURL;
String accessToken;
bool mmol;
bool useLed;
uint8_t ledIntensity = 64;

// BG levels
int lowUrgent = 55;
int lowWarning = 70;
int highWarning = 180;
int highUrgent = 240;

//–––––– Global Data ––––––
unsigned long lastUpdate = 0;
unsigned long lastTimestamp = 0;

// BG values
float bg;
float delta;
String trend;
unsigned long timestamp;

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  
  if(!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN)){
    Serial.println("SD MMC: Pin change failed!");
    return;
  }

  if (!SD_MMC.begin()) {
    Serial.println("SD card initialization failed.");
  } else {
    if (!readConfig()){
        return;
      }
  }

  tft.fillScreen(TFT_BLACK);
  spr.createSprite(tft.width(), tft.height());
  spr.setPivot(260, 51); // for trend arrow

  createTrendArrow();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (millis() - lastUpdate > 300*1000 || lastUpdate == 0) {
    fetchData();
  }

  updateDisplay();
  updateLED();  
  delay(10000);
}

void connectWiFi() {
  setLEDColor(0, 0, ledIntensity);

  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(FONT2);

  if (wifiSSID != "") {
    WiFi.mode(WIFI_STA);
    tft.print("Connecting to " + wifiSSID + "...");
    
    Serial.print("Connecting to WiFi: ");
    Serial.print(wifiSSID);
    
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str(), 0, NULL, true);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      tft.print(".");
      Serial.print(".");
    }
    Serial.println("\nWiFi connected.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    tft.println("done.");
    setLEDColor(0, 0, 0);
  } else {
    tft.println("No WiFi connection defined.");
  }
}

bool readConfig() {
  String configFilename = "/config.json";
  
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(FONT2);
  tft.print("Reading config file...");
  
  File configFile = SD_MMC.open(configFilename);

  if (configFile) {
    size_t size = configFile.size();
    std::unique_ptr<char[]> buf(new char[size]);
    configFile.readBytes(buf.get(), size);
    configFile.close();

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, buf.get());
    if (!error) {
      wifiSSID      = doc["wifi_ssid"].as<String>();
      wifiPassword  = doc["wifi_password"].as<String>();
      nightscoutURL = doc["nightscout_url"].as<String>();
      accessToken   = doc["access_token"].as<String>();
      mmol          = doc["mmol"].as<bool>();
      useLed        = doc["use_led"].as<bool>();
      
      if (doc["rotate"].as<bool>()) {
        tft.setRotation(1);
      }
      tft.println("done.");
      Serial.println("Configuration loaded from SD.");
    } else {
      tft.fillScreen(TFT_BLACK);
      tft.println("failed. Incorrect file format.");
      Serial.println("JSON parse error.");
      return false;
    }
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.println("failed. Cannot find config.json.");
    Serial.println("Failed to open config.json.");
    return false;
  }
  return true;
}

void fetchData() {
  HTTPClient http;
  String url = nightscoutURL + "/api/v2/properties.json";
  Serial.println("Reading from " + url);

  if (accessToken.isEmpty()) {
    url = url + "?token=" + accessToken;
    Serial.println("Using access token.");
  }

  http.begin(url);
  
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    Serial.println("Data received.");

    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, payload);

    bg = doc["bgnow"]["sgvs"][0]["mgdl"].as<int>();
    delta = doc["delta"]["mgdl"].as<float>();
    trend = doc["bgnow"]["sgvs"][0]["direction"].as<String>();
    timestamp = doc["bgnow"]["sgvs"][0]["mgdl"].as<unsigned long>();
    
    if (timestamp != lastTimestamp) {
      if (lastTimestamp > 0) {
        lastUpdate = millis();
      }
      lastTimestamp = timestamp;
    } else {
      lastUpdate = 0; // the same data after 5 minutes, need to resync?
    }
  } else {
    Serial.println("Cannot read latest data.");
  }
  http.end();
}

void updateDisplay() {
  spr.fillSprite(TFT_BLACK);
  
  // Update dynamic values
  updateBGValue();
  updateDelta();
  updateTimestamp();

  // Trend arrow(s)
  int16_t arrowAngle = getTrendArrowRotation();
  if (arrowAngle >= 0) {
    trd.pushRotated(&spr, arrowAngle);

    if (isDoubleTrendArrow()) {
      int32_t pivX = spr.getPivotX();
      int32_t pivY = spr.getPivotY();
      spr.setPivot(pivX + 10, pivY);
      trd.pushRotated(&spr, arrowAngle, TFT_BLACK);
      spr.setPivot(pivX, pivY);
    }
  }

  // Push buffer to screen
  spr.pushSprite(0, 0);
}

String getBGValue(float rawBG) {
  if (mmol) {
    return String(rawBG/18, 1);
  } else {
    return String(rawBG, 0);
  }
}

void updateBGValue() {
  uint8_t td = spr.getTextDatum();
  spr.setTextDatum(TC_DATUM);
  spr.drawString(getBGValue(bg), 120, 15, FONT8);
  spr.setTextDatum(td);
}

void updateDelta() {
  String prefix;
  if (delta > 0) {
    prefix = "+";
  }

  uint8_t td = spr.getTextDatum();
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(TFT_LIGHTGREY);
  spr.setFreeFont(FSS24);
  spr.drawString(prefix + getBGValue(delta), 120, 110, GFXFF);
  spr.setTextDatum(td);
  spr.setTextColor(TFT_WHITE);
}

void updateTimestamp() {
  String minutesDisp;

  if (lastUpdate == 0) {
    minutesDisp = "?";
  } else {
    float minFloat = (millis() - lastUpdate) / 60000;
    minutesDisp = String(minFloat, 0);
  }

  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(TFT_LIGHTGREY);
  spr.setFreeFont(FSS18);
  spr.drawString(minutesDisp, 250, 100, GFXFF);
  spr.setFreeFont(FSS12);
  spr.drawString("min. ago", 250, 135, GFXFF);
  spr.setTextColor(TFT_WHITE);
}

void createTrendArrow() {
  trd.createSprite(50, 50);
  trd.setPivot(25, 25);
  trd.drawLine(24, 1, 24, 50, TFT_WHITE);
  trd.drawLine(25, 1, 25, 50, TFT_WHITE);
  trd.drawLine(26, 1, 26, 50, TFT_WHITE);
  
  trd.drawLine(24, 0, 19, 20, TFT_WHITE);
  trd.drawLine(25, 0, 20, 20, TFT_WHITE);
  trd.drawLine(26, 0, 21, 20, TFT_WHITE);
  
  trd.drawLine(24, 0, 29, 20, TFT_WHITE);
  trd.drawLine(25, 0, 30, 20, TFT_WHITE);
  trd.drawLine(26, 0, 31, 20, TFT_WHITE);
}

int16_t getTrendArrowRotation() {
  if (trend.equals("DoubleUp")) return 0;
  if (trend.equals("SingleUp")) return 0;
  if (trend.equals("FortyFiveUp")) return 45;
  if (trend.equals("Flat")) return 90;
  if (trend.equals("FortyFiveDown")) return 135;
  if (trend.equals("SingleDown")) return 180;
  if (trend.equals("DoubleDown")) return 180;
  return -1;
}

bool isDoubleTrendArrow() {
  return trend.startsWith("Double");
}

void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
  if (useLed) {
    Serial.println("Setting LED to (" + String(r) + ", " + String(g) + ", " + String(b) + ")");
    neopixelWrite(PIN_NEOPIXEL, g, r, b);
  }
}

void updateLED() {
  if (bg < lowUrgent || bg > highUrgent) {
    setLEDColor(ledIntensity, 0, 0);
  } else if ((bg >= lowUrgent && bg <= lowWarning) || (bg >= highWarning && bg <= highUrgent)) {
    setLEDColor(ledIntensity, ledIntensity, 0);
  } else {
    setLEDColor(0, ledIntensity, 0);
  }
}