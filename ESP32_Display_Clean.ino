#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>

// =====================
// WLAN / MQTT
// =====================
const char* ssid     = "FRITZ!Box 7530 IF";
const char* password = "79147601316378641473";

const char* mqtt_server = "192.168.178.185";
const int   mqtt_port   = 1883;
const char* mqtt_user   = "";
const char* mqtt_pass   = "";

// Topics
const char* TOPIC_UI = "display/ui";

// =====================
// CYD Pins
// =====================
#define TFT_BL 21
#define SD_CS  5

// =====================
// RAW Backgrounds
// =====================
static const int W = 240;
static const int H = 320;

const char* BG[3] = {
  "/page1.raw",
  "/page2.raw",
  "/page3.raw"
};

// =====================
// Page Switching
// =====================
enum Page : uint8_t { PAGE1=0, PAGE2=1, PAGE3=2 };
Page currentPage = PAGE1;

static const uint32_t PAGE_SWITCH_MS  = 30000; // 30s
static const uint32_t OVERLAY_TICK_MS = 300;   // overlay refresh check

uint32_t lastPageSwitch  = 0;
uint32_t lastOverlayTick = 0;

// =====================
// TFT / SD / NET
// =====================
TFT_eSPI tft;
WiFiClient espClient;
PubSubClient client(espClient);

// =====================
// UI Data
// =====================
struct UIData {
  String time = "--:--";
  String weekday = "-";
  String date = "-";
  String bdayToday = "-";
  String bdayNextName = "-";
  String bdayNextDate = "-";
} ui;

// caches (nur neu zeichnen wenn geändert)
String c_time, c_weekday, c_date, c_bdayToday, c_bdayNextName, c_bdayNextDate;

// =====================
// Helpers
// =====================
static inline bool isBad(const String& s){
  String v=s; v.trim(); v.toLowerCase();
  return v.length()==0 || v=="unknown" || v=="unavailable" || v=="null" || v=="none";
}
static inline String safe(const String& s, const char* fb="-"){
  return isBad(s) ? String(fb) : s;
}

// Cyan-Farbe wie bisher
uint16_t COL_CYAN;

// Doppelrahmen
void boxRound(int x,int y,int w,int h,int r, uint16_t col){
  tft.drawRoundRect(x,y,w,h,r,col);
  tft.drawRoundRect(x+1,y+1,w-2,h-2,r-1,col);
}

// =====================
// SD RAW draw
// =====================
bool drawRaw(const char* path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  static uint16_t line[W];

  for (int y=0; y<H; y++) {
    int need = W*2;
    int got  = f.read((uint8_t*)line, need);
    if (got != need) { f.close(); return false; }
    tft.pushImage(0, y, W, 1, line);
    if ((y & 0x0F)==0) yield();
  }
  f.close();
  return true;
}

bool drawRawRegion(const char* path, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return true;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return true;

  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  static uint16_t line[W];

  for (int yy=0; yy<h; yy++) {
    uint32_t off = ((uint32_t)(y + yy) * (uint32_t)W + (uint32_t)x) * 2u;
    if (!f.seek(off)) { f.close(); return false; }
    int need = w * 2;
    int got  = f.read((uint8_t*)line, need);
    if (got != need) { f.close(); return false; }
    tft.pushImage(x, y + yy, w, 1, line);
    if ((yy & 0x0F)==0) yield();
  }
  f.close();
  return true;
}

const char* bgPathForPage(Page p){
  return BG[(int)p];
}

inline void restoreBg(int x,int y,int w,int h){
  if (!drawRawRegion(bgPathForPage(currentPage), x, y, w, h)) {
    tft.fillRect(x, y, w, h, TFT_BLACK);
  }
}

// =====================
// MQTT parse
// =====================
void parseUI(JsonDocument& doc){
  JsonObject o = doc.as<JsonObject>();

  if (o.containsKey("time"))    ui.time    = safe(String((const char*)o["time"]), "--:--");
  if (o.containsKey("weekday")) ui.weekday = safe(String((const char*)o["weekday"]), "-");
  if (o.containsKey("date"))    ui.date    = safe(String((const char*)o["date"]), "-");

  if (o.containsKey("birthday_today")) {
    ui.bdayToday = safe(String((const char*)o["birthday_today"]), "-");
  }

  if (o.containsKey("birthday_next") && o["birthday_next"].is<JsonObject>()) {
    JsonObject n = o["birthday_next"].as<JsonObject>();
    if (n.containsKey("name")) ui.bdayNextName = safe(String((const char*)n["name"]), "-");
    if (n.containsKey("date")) ui.bdayNextDate = safe(String((const char*)n["date"]), "-");
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length){
  String t(topic);
  String msg; msg.reserve(length+1);
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Serial.printf("[JSON] FAIL topic=%s err=%s\n", topic, err.c_str());
    return;
  }

  if (t == TOPIC_UI) parseUI(doc);
}

// =====================
// WiFi / MQTT connect
// =====================
void setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  Serial.print("[WiFi] connecting");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-start < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  Serial.printf("[WiFi] status=%d ip=%s rssi=%d\n",
    (int)WiFi.status(),
    WiFi.localIP().toString().c_str(),
    (int)WiFi.RSSI());
}

void mqttReconnect(){
  while (!client.connected()) {
    String cid = "ESP32_Display_" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.printf("[MQTT] connect as %s ...\n", cid.c_str());

    if (client.connect(cid.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("[MQTT] connected");
      client.subscribe(TOPIC_UI);
      Serial.printf("[MQTT] subscribed: %s\n", TOPIC_UI);
    } else {
      Serial.printf("[MQTT] fail rc=%d\n", (int)client.state());
      delay(1200);
    }
  }
}

// =====================
// Pages
// =====================
void resetCaches(){
  c_time=""; c_weekday=""; c_date="";
  c_bdayToday=""; c_bdayNextName=""; c_bdayNextDate="";
}

void showPage(Page p){
  currentPage = p;
  drawRaw(bgPathForPage(p));
  resetCaches();
}

// =====================
// PAGE 1 overlay (Uhr/Datum/Geburtstage)
// =====================
void overlayPage1(){
  String time    = safe(ui.time, "--:--");
  String weekday = safe(ui.weekday, "-");
  String date    = safe(ui.date, "-");
  String today   = safe(ui.bdayToday, "-");
  String nextN   = safe(ui.bdayNextName, "-");
  String nextD   = safe(ui.bdayNextDate, "-");

  // ---- Uhrzeit Box oben
  if (time != c_time) {
    // Bereich
    restoreBg(16, 6, 208, 56);
    boxRound(16, 6, 208, 56, 12, COL_CYAN);

    // Zeit zentriert
    tft.setTextDatum(MC_DATUM);
    // Font 7 ist schön groß, fallback falls zu breit
    int wTry = tft.textWidth(time, 7);
    int fontUse = (wTry <= 190) ? 7 : 4;

    tft.setTextFont(fontUse);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(time, 120, 34);

    c_time = time;
  }

  // ---- Wochentag + Datum (mittig)
  if (weekday != c_weekday || date != c_date) {
    restoreBg(0, 70, 240, 90);

    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(weekday, 120, 98);

    tft.setTextFont(2);
    tft.setTextColor(COL_CYAN);
    tft.drawString(date, 120, 126);

    c_weekday = weekday;
    c_date = date;
  }

  // ---- Geburtstagsbox unten (Heute + Nächster)
  if (today != c_bdayToday || nextN != c_bdayNextName || nextD != c_bdayNextDate) {
    int x=10, y=232, w=220, h=78;
    restoreBg(x, y, w, h);
    boxRound(x, y, w, h, 12, COL_CYAN);

    // Links: Heute
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("Geburtstag heute", x+10, y+8);

    tft.setTextColor(COL_CYAN);
    tft.drawString(today, x+10, y+30);

    // Rechts: Nächster
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("Naechster", x+w-10, y+8);

    tft.setTextColor(COL_CYAN);
    tft.drawString(nextN, x+w-10, y+30);

    tft.setTextColor(TFT_WHITE);
    tft.drawString(nextD, x+w-10, y+50);

    c_bdayToday = today;
    c_bdayNextName = nextN;
    c_bdayNextDate = nextD;
  }
}

// =====================
// Setup / Loop
// =====================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  COL_CYAN = tft.color565(0,255,255);

  SPI.begin();
  if (!SD.begin(SD_CS)) {
    Serial.println("[SD] begin FAIL");
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("SD FAIL", 120, 160);
    // trotzdem weiter, damit WLAN/MQTT sichtbar debuggt werden kann
  } else {
    Serial.println("[SD] OK");
  }

  setup_wifi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(1024);

  mqttReconnect();

  showPage(PAGE1);
  lastPageSwitch  = millis();
  lastOverlayTick = millis();
}

void loop() {
  // WiFi/MQTT am Leben halten
  if (WiFi.status() != WL_CONNECTED) setup_wifi();
  if (!client.connected()) mqttReconnect();
  client.loop();

  // Page switch
  if (millis() - lastPageSwitch > PAGE_SWITCH_MS) {
    lastPageSwitch = millis();
    Page next = (currentPage==PAGE1) ? PAGE2 : (currentPage==PAGE2 ? PAGE3 : PAGE1);
    showPage(next);
  }

  // Overlay tick
  if (millis() - lastOverlayTick > OVERLAY_TICK_MS) {
    lastOverlayTick = millis();
    if (currentPage==PAGE1) overlayPage1();
    // PAGE2 / PAGE3 Overlays kommen als nächste Steps
  }
}



