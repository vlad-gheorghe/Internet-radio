/* -------------------------------------------------
Copyright (c)
Arduino project by Tech Talkies YouTube Channel.
https://www.youtube.com/@techtalkies1
+ MULTITHREADING PERFECT: Audio(Core 1) / UI(Core 0)
-------------------------------------------------*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Audio.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <time.h>             
#include "secrets.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#define I2S_DOUT 26
#define I2S_BCLK 25
#define I2S_LRCK 22

#define TFT_SCK 18
#define TFT_MOSI 23
#define TFT_CS 5
#define TFT_DC 4
#define TFT_RST 2

#define ENC_CLK 32
#define ENC_DT 33
#define ENC_SW 27

#define RB_HOST "http://de1.api.radio-browser.info"
#define RB_RESULT_LIMIT 40
#define VOL_MAX 21
#define INITIAL_VOLUME 18

volatile bool eofReconnect = false;
volatile bool reqNewStation = false;
char reqUrl[256] = "";

U8G2_ST7565_JLX12864_F_4W_HW_SPI u8g2(U8G2_R0, /* cs=*/ TFT_CS, /* dc=*/ TFT_DC, /* reset=*/ TFT_RST);

struct CountryEntry { const char* code; const char* name; };
static const CountryEntry COUNTRIES[] = {
  { "all", "All" }, { "RO", "Romania" }, { "US", "USA" }, { "IN", "India" },
  { "GB", "UK" }, { "DE", "Germany" }, { "FR", "France" }, { "JP", "Japan" },
  { "CA", "Canada" }, { "AU", "Australia" }, { "IT", "Italy" }, { "ES", "Spain" },
  { "BR", "Brazil" }, { "MX", "Mexico" }, { "NL", "Netherlands" }, { "SE", "Sweden" },
  { "NO", "Norway" }, { "ZA", "South Africa" }, { "SG", "Singapore" }, { "AE", "UAE" }
};
static const int COUNTRY_COUNT = sizeof(COUNTRIES) / sizeof(COUNTRIES[0]);

struct GenreEntry { const char* tag; const char* label; };
static const GenreEntry GENRES[] = {
  { "all", "All" }, { "music", "Music" }, { "news", "News" },
  { "pop", "Pop" }, { "rock", "Rock" }, { "metal", "Metal" }, 
  { "blues", "Blues" }, { "hiphop", "Hip-Hop" }, { "jazz", "Jazz" }, 
  { "classical", "Classical" }, { "country", "Country" }, { "chillout", "Chillout" },
  { "retro", "Retro" }, { "80s", "80s Hits" }, { "90s", "90s Hits" },
  { "folk", "Folk" }, { "ambient", "Ambient" }
};
static const int GENRE_COUNT = sizeof(GENRES) / sizeof(GENRES[0]);

Audio audio;
QueueHandle_t encQueue;
Preferences prefs;

// ── PROTECTII DE MEMORIE ──
SemaphoreHandle_t titleMutex;
char streamTitle[256] = "Loading...";

enum EncEvent { EV_CW, EV_CCW, EV_PRESS, EV_LONG };
enum UiMode { MODE_NORMAL, MODE_BROWSE, MODE_EDIT };
enum FocusItem { F_NOWPLAYING, F_COUNTRY, F_TYPE };

struct Station {
  String name;
  String url;
  int bitrate;
  String country;
};

Station stations[RB_RESULT_LIMIT];
int stationCount = 0;
int currentStation = 0;
int focusIndex = 0;
UiMode uiMode = MODE_NORMAL;
bool uiDirty = true;
int previewStation = 0;

String previewTag = "all";
String previewCountry = "RO";
String searchTag = "all";
String selectedGenre = "all";
String selectedCountry = "RO";

bool muted = false;
int currentVol = INITIAL_VOLUME;
int lastVol = INITIAL_VOLUME;
uint32_t browseLastAction = 0;
volatile uint32_t holdStartMs = 0;
volatile bool buttonHolding = false;

int currentVuBars = 0; 
int textScrollX = 0;     
int scrollWait = 30;     

bool fetchStations(String tag) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  String url = String(RB_HOST) + "/json/stations/search?limit=15&hidebroken=true&order=clickcount&reverse=true";
  if (tag != "all") url += "&tag=" + tag;
  if (selectedCountry != "all") url += "&countrycode=" + selectedCountry;
  
  http.setTimeout(3500);
  http.begin(url);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  DynamicJsonDocument doc(16384); 
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    stationCount = 1;
    stations[0] = { "Eroare Memorie", "http://live2.radiozu.ro:8020/radiozu.mp3", 128, "RO" };
    return true; 
  }

  stationCount = 0;
  for (JsonObject s : doc.as<JsonArray>()) {
    String u = s["url_resolved"] | "";
    if (!u.startsWith("http")) continue; 
    
    stations[stationCount].name = s["name"].as<String>();
    stations[stationCount].url = u;
    stations[stationCount].bitrate = s["bitrate"] | 0;
    stations[stationCount].country = s["countrycode"].as<String>();
    stationCount++;
    if (stationCount >= RB_RESULT_LIMIT) break;
  }
  
  if (stationCount == 0) {
    stationCount = 1;
    stations[0] = { "Radio ZU (Backup)", "http://live2.radiozu.ro:8020/radiozu.mp3", 128, "RO" };
  }
  return stationCount > 0;
}

void playStation(int i) {
  if (i < 0 || i >= stationCount) return;
  currentStation = i;
  
  if (xSemaphoreTake(titleMutex, portMAX_DELAY)) {
    strncpy(streamTitle, stations[i].name.c_str(), sizeof(streamTitle) - 1);
    streamTitle[sizeof(streamTitle) - 1] = '\0';
    xSemaphoreGive(titleMutex);
  }
  
  textScrollX = 0;
  scrollWait = 30; 
  
  // Copiem URL-ul in siguranta si semnalizam cererea pentru bucla audio
  strncpy(reqUrl, stations[i].url.c_str(), sizeof(reqUrl) - 1);
  reqUrl[sizeof(reqUrl) - 1] = '\0';
  reqNewStation = true; 
  
  uiDirty = true;
  prefs.putInt("station", currentStation);
}

String countryName(String c) {
  c.toUpperCase();
  for (int i = 0; i < COUNTRY_COUNT; i++) {
    if (c == COUNTRIES[i].code) return String(COUNTRIES[i].name);
  }
  return c;
}

String typeName() {
  for (int i = 0; i < GENRE_COUNT; i++)
    if (selectedGenre == GENRES[i].tag) return String(GENRES[i].label);
  return String(selectedGenre);
}

void chip(int x, int y, String txt, bool isFocused, bool isEditing) {
  u8g2.setFont(u8g2_font_5x8_tf);
  int w = u8g2.getStrWidth(txt.c_str()) + 6;
  
  if (isEditing) {
    u8g2.drawBox(x, y, w, 11);
    u8g2.setDrawColor(0); 
    u8g2.setCursor(x + 3, y + 8);
    u8g2.print(txt);
    u8g2.setDrawColor(1); 
  } else {
    if (isFocused) {
      u8g2.drawRFrame(x, y, w, 11, 2); 
    }
    u8g2.setCursor(x + 3, y + 8);
    u8g2.print(txt);
  }
}

void drawUI() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1); 

  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.setCursor(0, 7);
  u8g2.print("Volum");

  if (buttonHolding) {
    int prog = min(12, (int)((millis() - holdStartMs) / 58));
    u8g2.drawBox(116, 2, prog, 4);
  }

  for (int i = 0; i < 12; i++) {
    int bx = 70 + i * 4;
    if (i < currentVol * 12 / 21) {
      u8g2.drawBox(bx, 1, 3, 6);
    } else {
      u8g2.drawFrame(bx, 1, 3, 6);
    }
  }
  u8g2.drawLine(0, 10, 128, 10); 

  u8g2.setFont(u8g2_font_6x10_tf);
  
  if (uiMode == MODE_EDIT && focusIndex == F_NOWPLAYING) {
    String np = "< " + stations[previewStation].name.substring(0, 16) + " >";
    int npX = (128 - u8g2.getStrWidth(np.c_str())) / 2;
    u8g2.drawBox(npX - 2, 13, u8g2.getStrWidth(np.c_str()) + 4, 12);
    u8g2.setDrawColor(0); 
    u8g2.setCursor(npX, 23);
    u8g2.print(np);
    u8g2.setDrawColor(1);
  } else {
    if (focusIndex == F_NOWPLAYING && uiMode != MODE_NORMAL) {
      u8g2.drawRFrame(0, 12, 128, 14, 2); 
    }
    
    int textWidth = 0;
    if (xSemaphoreTake(titleMutex, portMAX_DELAY)) {
      textWidth = u8g2.getStrWidth(streamTitle);
      xSemaphoreGive(titleMutex);
    }

    if (textWidth <= 128) {
      int npX = (128 - textWidth) / 2;
      u8g2.setCursor(npX, 23);
    } else {
      u8g2.setCursor(textScrollX, 23);
    }
    
    if (xSemaphoreTake(titleMutex, portMAX_DELAY)) {
      u8g2.print(streamTitle);
      xSemaphoreGive(titleMutex);
    }
  }

  String cCode = (uiMode == MODE_EDIT && focusIndex == F_COUNTRY) ? String(previewCountry) : (selectedCountry == "all" ? stations[currentStation].country : String(selectedCountry));
  chip(2, 30, countryName(cCode), (focusIndex == F_COUNTRY && uiMode != MODE_NORMAL), (uiMode == MODE_EDIT && focusIndex == F_COUNTRY));

  String gLabel;
  if (uiMode == MODE_EDIT && focusIndex == F_TYPE) {
    gLabel = previewTag;
    for (int i = 0; i < GENRE_COUNT; i++) if (previewTag == GENRES[i].tag) { gLabel = GENRES[i].label; break; }
  } else {
    gLabel = typeName();
  }
  
  int gW = u8g2.getStrWidth(gLabel.c_str()) + 6;
  chip(126 - gW, 30, gLabel, (focusIndex == F_TYPE && uiMode != MODE_NORMAL), (uiMode == MODE_EDIT && focusIndex == F_TYPE));

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) { 
    char timeStr[6];
    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    
    u8g2.setFont(u8g2_font_logisoso16_tf); 
    int tW = u8g2.getStrWidth(timeStr);
    int tX = ((128 - tW) / 2) + 5;
    
    u8g2.setDrawColor(0);
    u8g2.drawBox(tX - 2, 28, tW + 4, 17); 
    u8g2.setDrawColor(1);
    
    u8g2.setCursor(tX, 43); 
    u8g2.print(timeStr);
  }

  int startX = 19; 
  int startY = 48; 
  for (int i = 0; i < 15; i++) {
    if (i < currentVuBars) {
      u8g2.drawBox(startX + i * 6, startY, 4, 12); 
    } else {
      u8g2.drawFrame(startX + i * 6, startY, 4, 12); 
    }
  }

  u8g2.sendBuffer(); 
  uiDirty = false;
}

void updateAnimations() {
  int bars = 0;
  static int lastBars = 0;
  
  if (audio.isRunning() && currentVol > 0) {
    int chance = random(100);
    if (chance > 85) bars = random(12, 16);       
    else if (chance > 45) bars = random(7, 13);   
    else bars = random(2, 8);                     
  } else {
    bars = 0; 
  }

  if (bars < lastBars) bars = lastBars - 1; 
  else if (bars > lastBars + 4) bars = lastBars + 3; 
  
  if (bars < 0) bars = 0;
  if (bars > 15) bars = 15;
  lastBars = bars;
  
  if (currentVuBars != bars) {
    currentVuBars = bars;
    if (uiMode == MODE_NORMAL) uiDirty = true; 
  }

  if (uiMode == MODE_NORMAL) {
    u8g2.setFont(u8g2_font_6x10_tf); 
    int textWidth = 0;
    if (xSemaphoreTake(titleMutex, portMAX_DELAY)) {
      textWidth = u8g2.getStrWidth(streamTitle);
      xSemaphoreGive(titleMutex);
    }
    
    if (textWidth > 128) {
      if (scrollWait > 0) {
        scrollWait--; 
      } else {
        textScrollX -= 1; 
        if (textScrollX < -textWidth) {
          textScrollX = 128;
        }
        uiDirty = true; 
      }
    } else {
      textScrollX = 0; 
    }
  }
}

void applyChange() {
  prefs.putString("country", selectedCountry);
  prefs.putString("genre", selectedGenre);
  prefs.putInt("station", 0); 
  
  fetchStations(selectedGenre);
  playStation(0);
}

void handleEvent(uint8_t ev) {
  browseLastAction = millis();

  if (uiMode == MODE_NORMAL) {
    if (ev == EV_CW) {
      currentVol = min(VOL_MAX, currentVol + 1);
      audio.setVolume(currentVol);
      prefs.putInt("vol", currentVol); 
    }
    else if (ev == EV_CCW) {
      currentVol = max(0, currentVol - 1);
      audio.setVolume(currentVol);
      prefs.putInt("vol", currentVol); 
    }
    else if (ev == EV_PRESS) {
      muted = !muted;
      if (muted) {
        lastVol = currentVol;
        audio.setVolume(0);
      } else {
        currentVol = lastVol;
        audio.setVolume(currentVol);
      }
      prefs.putInt("vol", currentVol);
    } else if (ev == EV_LONG) {
      uiMode = MODE_BROWSE;
      previewStation = currentStation;
      previewTag = selectedGenre;
      previewCountry = selectedCountry;
    }
  } else if (uiMode == MODE_BROWSE) {
    if (ev == EV_CW) focusIndex = (focusIndex + 1) % 3;
    else if (ev == EV_CCW) focusIndex = (focusIndex + 2) % 3;
    else if (ev == EV_PRESS) uiMode = MODE_EDIT;
    else if (ev == EV_LONG) uiMode = MODE_NORMAL;
  } else if (uiMode == MODE_EDIT) {
    if (focusIndex == F_NOWPLAYING) {
      if (ev == EV_CW && stationCount > 0)
        previewStation = (previewStation + 1) % stationCount;
      else if (ev == EV_CCW && stationCount > 0)
        previewStation = (previewStation - 1 + stationCount) % stationCount;
      else if (ev == EV_PRESS) {
        playStation(previewStation);
        uiMode = MODE_BROWSE;
      } else if (ev == EV_LONG) uiMode = MODE_BROWSE;
    } else if (focusIndex == F_COUNTRY) {
      if (ev == EV_CW || ev == EV_CCW) {
        int ci = 0;
        for (int i = 0; i < COUNTRY_COUNT; i++) {
          if (previewCountry == COUNTRIES[i].code) { ci = i; break; }
        }
        int dir = (ev == EV_CW) ? 1 : -1;
        ci = (ci + dir + COUNTRY_COUNT) % COUNTRY_COUNT;
        previewCountry = COUNTRIES[ci].code;
      } else if (ev == EV_PRESS) {
        selectedCountry = previewCountry;
        applyChange();
        uiMode = MODE_BROWSE;
      } else if (ev == EV_LONG) uiMode = MODE_BROWSE;
    } else if (focusIndex == F_TYPE) {
      if (ev == EV_CW || ev == EV_CCW) {
        int gi = 0;
        for (int i = 0; i < GENRE_COUNT; i++) {
          if (previewTag == GENRES[i].tag) { gi = i; break; }
        }
        int dir = (ev == EV_CW) ? 1 : -1;
        gi = (gi + dir + GENRE_COUNT) % GENRE_COUNT;
        previewTag = GENRES[gi].tag;
      } else if (ev == EV_PRESS) {
        selectedGenre = previewTag;
        searchTag = selectedGenre;
        applyChange();
        uiMode = MODE_BROWSE;
      } else if (ev == EV_LONG) uiMode = MODE_BROWSE;
    }
  }
  uiDirty = true;
}

void taskRotary(void* p) {
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  int lastClk = digitalRead(ENC_CLK);
  bool lastBtn = HIGH;
  uint32_t pressAt = 0;

  for (;;) {
    int clk = digitalRead(ENC_CLK);
    if (clk != lastClk && clk == LOW) {
      uint8_t e = digitalRead(ENC_DT) ? EV_CW : EV_CCW;
      xQueueSend(encQueue, &e, 0);
    }
    lastClk = clk;

    bool btn = digitalRead(ENC_SW);
    if (btn == LOW && lastBtn == HIGH) {
      pressAt = millis();
      holdStartMs = pressAt;
      buttonHolding = true;
    }
    if (btn == HIGH && lastBtn == LOW) {
      buttonHolding = false;
      uint8_t e = (millis() - pressAt > 700) ? EV_LONG : EV_PRESS;
      xQueueSend(encQueue, &e, 0);
    }
    lastBtn = btn;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ── NUCLEUL GRAFIC (Core 0) ──
void taskUI(void* p) {
  for (;;) {
    if ((uiMode == MODE_BROWSE || uiMode == MODE_EDIT) && millis() - browseLastAction > 10000) {
      uiMode = MODE_NORMAL;
      uiDirty = true;
    }

    static uint32_t lastAnim = 0;
    if (millis() - lastAnim > 60) {
      lastAnim = millis();
      updateAnimations();
    }

    static uint32_t lastTimeCheck = 0;
    if (millis() - lastTimeCheck > 1000) {
      lastTimeCheck = millis();
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 0)) { 
        static int lastMin = -1;
        if (timeinfo.tm_min != lastMin) {
          lastMin = timeinfo.tm_min;
          uiDirty = true; 
        }
      }
    }

    if (uiDirty) {
      drawUI();
    }
    
    // Pauza foarte scurta ce mentine nucleul 0 sanatos pentru Wi-Fi
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  u8g2.begin();
  u8g2.setBusClock(8000000); // Acceleram desenarea
  u8g2.setContrast(130); 
  
  titleMutex = xSemaphoreCreateMutex(); 
  
  prefs.begin("radio", false); 
  currentVol = prefs.getInt("vol", INITIAL_VOLUME);
  selectedCountry = prefs.getString("country", "RO");
  selectedGenre = prefs.getString("genre", "all");
  int savedStation = prefs.getInt("station", 0);
  
  previewCountry = selectedCountry;
  previewTag = selectedGenre;
  searchTag = selectedGenre;
  lastVol = currentVol;
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false); // Anulam restrictiile de baterie 
  
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(10, 30, "Connecting Wi-Fi...");
  u8g2.sendBuffer();
  
  while (WiFi.status() != WL_CONNECTED) delay(300);

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
  tzset();

  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audio.setVolume(currentVol);
  // ADAUGĂ ACEASTĂ LINIE: Mărește memoria tampon internă pentru a rezista la micro-latențe
 // audio.setBufsize(64 * 1024, 0); // Alocă un buffer de rețea generos

  encQueue = xQueueCreate(16, 1);
  
  // LANSAM ENCODERUL SI ECRANUL PE CORE 0
  xTaskCreatePinnedToCore(taskRotary, "rotary", 4096, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(taskUI, "ui", 10000, nullptr, 1, nullptr, 0);

  fetchStations(selectedGenre);
  if (savedStation >= stationCount) savedStation = 0; 
  playStation(savedStation);
}

// ── NUCLEUL AUDIO (Core 1 - Procesorul ruleaza DOAR asta) ──
void loop() {
  // Procesam cererea de schimbare post in mod asincron
  if (reqNewStation) {
    reqNewStation = false;
    audio.connecttohost(reqUrl);
  }

  audio.loop();

  if (eofReconnect) {
    eofReconnect = false;
    playStation(currentStation); 
  }

  // --- AUDIO WATCHDOG (Auto-Recovery la blocaj total) ---
 /* static uint32_t audioStuckTimer = 0;
  if (!muted && currentVol > 0) {
    if (!audio.isRunning()) {
      if (audioStuckTimer == 0) {
        audioStuckTimer = millis(); // Pornim cronometrul cand se opreste sunetul
      } else if (millis() - audioStuckTimer > 200) { // Daca sta blocat peste 5 secunde
        audioStuckTimer = 0;
        playStation(currentStation); // Fortam reconectarea automata
      }
    } else {
      audioStuckTimer = 0; // Totul functioneaza normal, resetam cronometrul
    }
    
  }
  // -----------------------------------------------------
*/
  uint8_t ev;
  while (xQueueReceive(encQueue, &ev, 0) == pdTRUE) {
    handleEvent(ev);
  }
}

// ── FUNCTII DE CALLBACK AUDIO ──
void audio_showstreamtitle(const char* info) {
  if (xSemaphoreTake(titleMutex, portMAX_DELAY)) {
    strncpy(streamTitle, info, sizeof(streamTitle) - 1);
    streamTitle[sizeof(streamTitle) - 1] = '\0';
    xSemaphoreGive(titleMutex);
  }
  textScrollX = 0;
  scrollWait = 30;
  uiDirty = true;
}

void audio_eof_stream(const char* info) {
  eofReconnect = true; // Doar ridicăm steagul, evitând blocarea stivei TCP
}
