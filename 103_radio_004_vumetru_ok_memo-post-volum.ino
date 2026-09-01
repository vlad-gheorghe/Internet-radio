/* -------------------------------------------------
Copyright (c)
Arduino project by Tech Talkies YouTube Channel.
https://www.youtube.com/@techtalkies1
+ VU Metru Cinematic + Genuri Extinse + MEMORIE NVS
-------------------------------------------------*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Audio.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Preferences.h>      // <-- Libraria pentru memorie interna
#include "secrets.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#define I2S_DOUT 26
#define I2S_BCLK 25
#define I2S_LRCK 22

#define TFT_SCK 18
#define TFT_MOSI 23
#define TFT_DC 4
#define TFT_RST 2
#define TFT_CS 5

#define ENC_CLK 32
#define ENC_DT 33
#define ENC_SW 27

#define RB_HOST "http://de1.api.radio-browser.info"
#define RB_RESULT_LIMIT 40
#define VOL_MAX 21
#define INITIAL_VOLUME 18

// ── Lista de țări ────────────────────────────────
struct CountryEntry { const char* code; const char* name; };
static const CountryEntry COUNTRIES[] = {
  { "all", "All" }, { "RO", "Romania" }, { "US", "USA" }, { "IN", "India" },
  { "GB", "UK" }, { "DE", "Germany" }, { "FR", "France" }, { "JP", "Japan" },
  { "CA", "Canada" }, { "AU", "Australia" }, { "IT", "Italy" }, { "ES", "Spain" },
  { "BR", "Brazil" }, { "MX", "Mexico" }, { "NL", "Netherlands" }, { "SE", "Sweden" },
  { "NO", "Norway" }, { "ZA", "South Africa" }, { "SG", "Singapore" }, { "AE", "UAE" }
};
static const int COUNTRY_COUNT = sizeof(COUNTRIES) / sizeof(COUNTRIES[0]);

// ── Lista genurilor (Extinsa) ─────────────────────────
struct GenreEntry { const char* tag; const char* label; };
static const GenreEntry GENRES[] = {
  { "all", "All" }, 
  { "music", "Music" }, 
  { "news", "News" },
  { "pop", "Pop" }, 
  { "rock", "Rock" }, 
  { "metal", "Metal" }, 
  { "blues", "blues" }, 
  //{ "dance", "Dance" },
  //{ "electronic", "Electronic" }, 
  { "hiphop", "Hip-Hop" }, 
  { "jazz", "Jazz" }, 
  { "classical", "Classical" }, 
  { "country", "Country" },
  { "chillout", "Chillout" },
  { "retro", "Retro" },
  { "80s", "80s Hits" },
  { "90s", "90s Hits" },
  { "folk", "Folk" },
  { "ambient", "Ambient" }
};
static const int GENRE_COUNT = sizeof(GENRES) / sizeof(GENRES[0]);

Audio audio;
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(160, 128);
QueueHandle_t encQueue;
Preferences prefs; // <-- Obiectul care gestioneaza hard-disk-ul virtual

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
String streamTitle = "Loading...";
int previewStation = 0;

String previewTag = "all";
String previewCountry = "RO";
String searchTag = "all";
String selectedGenre = "all";
String selectedCountry = "RO";

int bitrateCap = 96;
bool muted = false;
int currentVol = INITIAL_VOLUME;
int lastVol = INITIAL_VOLUME;
uint32_t browseLastAction = 0;
volatile uint32_t holdStartMs = 0;
volatile bool buttonHolding = false;

bool fetchStations(String tag) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  String url = String(RB_HOST) + "/json/stations/search?limit=15&hidebroken=true&order=clickcount&reverse=true";
  if (tag != "all") url += "&tag=" + tag;
  if (selectedCountry != "all") url += "&countrycode=" + selectedCountry;
  
  http.setTimeout(3500);
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }

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
  streamTitle = stations[i].name;
  audio.connecttohost(stations[i].url.c_str());
  uiDirty = true;
  
  // Salvam ultimul post in memorie!
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

#define DISP canvas

void chip(int x, int y, String txt, uint16_t col) {
  int w = txt.length() * 6 + 10;
  bool activeEdit = (uiMode == MODE_EDIT && ((focusIndex == 1 && x < 60) || (focusIndex == 2 && x > 60)));
  if (activeEdit) {
    DISP.fillRoundRect(x, y, w, 14, 3, col);
    DISP.setTextColor(ILI9341_BLACK);
  } else {
    DISP.drawRoundRect(x, y, w, 14, 3, col);
    DISP.setTextColor(col);
  }
  DISP.setCursor(x + 5, y + 4);
  DISP.print(txt);
}

void drawUI() {
  DISP.fillScreen(0x0000);

  uint16_t topBg = tft.color565(50, 0, 60);
  uint16_t cardBg = tft.color565(32, 73, 93);
  uint16_t botBg = ILI9341_BLACK;
  uint16_t border = ILI9341_WHITE;
  uint16_t accent = ILI9341_GREEN;
  uint16_t warn = ILI9341_YELLOW;

  DISP.fillRoundRect(2, 2, 156, 16, 3, topBg);
  DISP.drawRoundRect(2, 2, 156, 16, 3, border);

  DISP.fillRoundRect(2, 22, 156, 80, 5, cardBg);
  DISP.drawRoundRect(2, 22, 156, 80, 5, border);

  DISP.fillRoundRect(2, 106, 156, 20, 4, botBg);
  DISP.drawRoundRect(2, 106, 156, 20, 4, (uiMode == MODE_NORMAL) ? border : accent);

  DISP.setCursor(8, 6);
  DISP.setTextColor(accent);
  DISP.print("Nivel volum");

  uint16_t volCol = (uiMode == MODE_NORMAL) ? accent : 0x39E7;
  for (int i = 0; i < 12; i++) {
    uint16_t c = (i < audio.getVolume() * 12 / 21) ? volCol : 0x2104;
    DISP.fillRoundRect(90 + i * 5, 6, 4, 6, 1, c);
  }

  uint16_t npCol = (focusIndex == F_NOWPLAYING && uiMode != MODE_NORMAL) ? warn : ILI9341_WHITE;
  DISP.setTextColor(npCol);
  DISP.setCursor((160 - 11 * 6) / 2, 30);
  DISP.print("Now Playing");

  String np = (uiMode == MODE_EDIT && focusIndex == F_NOWPLAYING)
                ? (String("< ") + stations[previewStation].name.substring(0, 18) + " >")
                : streamTitle.substring(0, 24);
  int npX = (160 - (int)np.length() * 6) / 2;

  if (uiMode == MODE_EDIT && focusIndex == F_NOWPLAYING) {
    DISP.fillRoundRect(npX - 4, 44, np.length() * 6 + 8, 16, 3, warn);
    DISP.setTextColor(ILI9341_BLACK);
    DISP.setCursor(npX, 48);
    DISP.print(np);
    
    String countTxt = String(previewStation + 1) + "/" + String(stationCount);
    DISP.setTextColor(warn);
    DISP.setCursor((160 - (int)countTxt.length() * 6) / 2, 66);
    DISP.print(countTxt);
  } else {
    DISP.setTextColor(ILI9341_WHITE);
    DISP.setCursor(npX, 48);
    DISP.print(np);
  }

  String cCode = (uiMode == MODE_EDIT && focusIndex == F_COUNTRY)
                   ? String(previewCountry)
                   : (selectedCountry == "all" ? stations[currentStation].country : String(selectedCountry));
  chip(4, 80, countryName(cCode), focusIndex == F_COUNTRY ? warn : accent);

  String gLabel;
  if (uiMode == MODE_EDIT && focusIndex == F_TYPE) {
    gLabel = previewTag;
    for (int i = 0; i < GENRE_COUNT; i++)
      if (previewTag == GENRES[i].tag) { gLabel = GENRES[i].label; break; }
  } else {
    gLabel = typeName();
  }
  chip(92, 80, gLabel, focusIndex == F_TYPE ? warn : ILI9341_CYAN);

  DISP.drawCircle(149, 116, 8, ILI9341_WHITE);
  if (buttonHolding) {
    int prog = min(8, (int)((millis() - holdStartMs) / 87));
    for (int i = 0; i < prog; i++) DISP.fillCircle(149, 116, i, accent);
  }

  DISP.setCursor(8, 112);
  DISP.setTextColor(ILI9341_WHITE);
  if (uiMode == MODE_NORMAL) DISP.print("Menu Volume");
  else if (uiMode == MODE_BROWSE) DISP.print("Rotate Select");
  else DISP.print("Rotate Change");

  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 128);
  uiDirty = false;
}

// ── DESENARE VU METRU CINEMATIC ──
void drawVU() {
  int bars = 0;
  static int lastBars = 0;
  
  if (audio.isRunning() && audio.getVolume() > 0) {
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

  int startX = 35;
  int startY = 66; 
  uint16_t emptyColor = 0x2104; 

  for (int i = 0; i < 15; i++) {
    uint16_t color = emptyColor; 
    if (i < bars) {
      if (i < 9) color = ILI9341_GREEN;
      else if (i < 12) color = ILI9341_CYAN;
      else color = ILI9341_BLUE;
    }
    DISP.fillRect(startX + i * 6, startY, 4, 6, color);
  }
  
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 128);
}

void applyChange() {
  // Salvam in memorie noile criterii de cautare
  prefs.putString("country", selectedCountry);
  prefs.putString("genre", selectedGenre);
  prefs.putInt("station", 0); // La schimbarea filtrelor pornim de la postul 1
  
  fetchStations(selectedGenre);
  playStation(0);
}

void handleEvent(uint8_t ev) {
  browseLastAction = millis();

  if (uiMode == MODE_NORMAL) {
    if (ev == EV_CW) {
      currentVol = min(VOL_MAX, audio.getVolume() + 1);
      audio.setVolume(currentVol);
      prefs.putInt("vol", currentVol); // Salvam volumul
    }
    else if (ev == EV_CCW) {
      currentVol = max(0, audio.getVolume() - 1);
      audio.setVolume(currentVol);
      prefs.putInt("vol", currentVol); // Salvam volumul
    }
    else if (ev == EV_PRESS) {
      muted = !muted;
      if (muted) {
        lastVol = audio.getVolume();
        audio.setVolume(0);
      } else {
        audio.setVolume(lastVol);
        prefs.putInt("vol", lastVol);
      }
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

void setup() {
  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  SPI.setFrequency(20000000);
  tft.begin();
  tft.setRotation(3);
  
  // FIX Oglindire
  tft.sendCommand(0x36, (const uint8_t *)"\x68", 1); 

  // ── CITIREA MEMORIEI LA PORNIRE ──
  prefs.begin("radio", false); 
  currentVol = prefs.getInt("vol", INITIAL_VOLUME);
  selectedCountry = prefs.getString("country", "RO");
  selectedGenre = prefs.getString("genre", "all");
  int savedStation = prefs.getInt("station", 0);
  
  // Sincronizam interfata cu datele descarcate din memorie
  previewCountry = selectedCountry;
  previewTag = selectedGenre;
  searchTag = selectedGenre;
  lastVol = currentVol;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  
  // Aplicam volumul memorat
  audio.setVolume(currentVol);

  encQueue = xQueueCreate(16, 1);
  xTaskCreatePinnedToCore(taskRotary, "rotary", 4096, nullptr, 1, nullptr, 1);

  // Incarcam posturile folosind Tara si Genul memorate
  fetchStations(selectedGenre);
  
  // Daca baza de date s-a miscorat peste noapte si statia noastra nu mai exista, o resetam la 0
  if (savedStation >= stationCount) savedStation = 0; 
  playStation(savedStation);
}

void loop() {
  audio.loop();
  
  uint8_t ev;
  while (xQueueReceive(encQueue, &ev, 0) == pdTRUE) handleEvent(ev);

  if ((uiMode == MODE_BROWSE || uiMode == MODE_EDIT) && millis() - browseLastAction > 10000) {
    uiMode = MODE_NORMAL;
    uiDirty = true;
  }

  if (uiDirty) drawUI();
  
  static uint32_t lastVU = 0;
  if (millis() - lastVU > 60) {
    lastVU = millis();
    if (uiMode == MODE_NORMAL) {
      drawVU();
    }
  }
}

void audio_showstreamtitle(const char* info) {
  streamTitle = String(info);
  uiDirty = true;
}