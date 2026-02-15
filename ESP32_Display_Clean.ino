#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>

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

// Innen-Sensor (DHT11 am Display)
#define DHTPIN  27
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

const char* TOPIC_INSIDE_STATE = "innen/sensor/state";
uint32_t lastInsidePub = 0;


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
// PAGE 2 Data (Wetter)
// =====================
struct Page2Data {
  float outT = NAN;
  int   outH = -1;

  float netT = NAN;
  String netCond = "unknown";

  struct FcItem {
    String d = "--";     // "Mo"
    int hi = 0;
    int lo = 0;
    String cond = "unknown";
  } fc[5];
} p2;

// Caches Page2 (nur neu zeichnen wenn geändert)
float  c_inT = NAN; int c_inH = -1;
float  c_outT = NAN; int c_outH = -1;
float  c_netT = NAN; String c_netCond = "";
String c_fcSig = "";
// Für flackerfreie Anzeige: wir vergleichen gerundete Werte
int c_inTi  = 9999;
int c_outTi = 9999;
int c_netTi = 9999;

// Live Innenwerte (DHT) nur selten aktualisieren (gegen Flackern)
float liveInT = NAN;
int   liveInH = -1;
uint32_t lastDhtRead = 0;


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
void publishInsideIfDue() {
  const uint32_t interval = 60000; // 60 Sekunden
  if (millis() - lastInsidePub < interval) return;
  lastInsidePub = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    Serial.println("[DHT] read failed (inside)");
    return;
  }

  StaticJsonDocument<128> j;
  j["temperature"] = t;
  j["humidity"] = h;

  char out[128];
  size_t n = serializeJson(j, out, sizeof(out));

  bool ok = client.publish(TOPIC_INSIDE_STATE, (const uint8_t*)out, n, false);

  Serial.printf("[MQTT] publish inside %s -> %s\n", ok ? "OK" : "FAIL", out);
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
  if (o.containsKey("page2") && o["page2"].is<JsonObject>()) {
    parsePage2(o["page2"].as<JsonObject>());
  }
}

void parsePage2(JsonObject p){
  // outside
  if (p.containsKey("outside") && p["outside"].is<JsonObject>()) {
    JsonObject o = p["outside"].as<JsonObject>();
    if (o.containsKey("t")) p2.outT = o["t"].as<float>();
    if (o.containsKey("h")) p2.outH = o["h"].as<int>();
  }

  // net
  if (p.containsKey("net") && p["net"].is<JsonObject>()) {
    JsonObject n = p["net"].as<JsonObject>();
    if (n.containsKey("t") && !n["t"].isNull()) p2.netT = n["t"].as<float>();
    if (n.containsKey("cond")) p2.netCond = safe(String((const char*)n["cond"]), "unknown");
  }

  // forecast (5 Tage)
  if (p.containsKey("fc") && p["fc"].is<JsonArray>()) {
    JsonArray a = p["fc"].as<JsonArray>();
    for (int i=0;i<5;i++){
      if (i < (int)a.size() && a[i].is<JsonObject>()) {
        JsonObject it = a[i].as<JsonObject>();
        if (it.containsKey("d"))    p2.fc[i].d = safe(String((const char*)it["d"]), "--");
        if (it.containsKey("hi"))   p2.fc[i].hi = it["hi"].as<int>();
        if (it.containsKey("lo"))   p2.fc[i].lo = it["lo"].as<int>();
        if (it.containsKey("cond")) p2.fc[i].cond = safe(String((const char*)it["cond"]), "unknown");
      } else {
        p2.fc[i] = Page2Data::FcItem(); // reset
      }
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length){
  Serial.println("=== MQTT MESSAGE RECEIVED ===");
  Serial.printf("Topic: %s\n", topic);
  Serial.printf("Length: %u\n", length);
  Serial.print("Payload: ");
for (unsigned int i = 0; i < length; i++) {
  Serial.print((char)payload[i]);
}
Serial.println();

  String t(topic);
  String msg; msg.reserve(length+1);
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];

  StaticJsonDocument<8192> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
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

  c_inT = NAN; c_inH = -1;
  c_outT = NAN; c_outH = -1;
  c_netT = NAN; c_netCond = "";
  c_fcSig = "";
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

  // ---- Geburtstagsbox unten (Heute + Nächster) UNTEREINANDER
if (today != c_bdayToday || nextN != c_bdayNextName || nextD != c_bdayNextDate) {
  int x=10, y=210, w=220, h=90;
  restoreBg(x, y, w, h);
  boxRound(x, y, w, h, 12, COL_CYAN);

  // Labels links
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE);

  tft.drawString("Heute Geburtstag", x+12, y+8);
  tft.drawString("Naechster Geburtstag", x+12, y+48);

  // Werte (Namen) mit Platz
  tft.setTextColor(COL_CYAN);

  // Heute Name (groesser)
  tft.setTextFont(4);
  tft.drawString(today, x+12, y+22);

  // Naechster Name (kleiner, damit es passt)
  tft.setTextFont(2);
  tft.drawString(nextN, x+12, y+64);

  // Naechstes Datum rechts unten
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_WHITE);
  tft.drawString(nextD, x+w-12, y+64);

  c_bdayToday = today;
  c_bdayNextName = nextN;
  c_bdayNextDate = nextD;
  }
}

// ===========================
// PAGE 2 - NICE WEATHER ICONS
// ===========================

void drawCloud(int cx, int cy, int w, int h, uint16_t fill, uint16_t outline) {
  int x = cx - w/2;
  int y = cy - h/2;

  int baseH = (h * 45) / 100;      // ~0.45h
  int baseY = y + h - baseH;

  // Outline (optional)
  if (outline != fill) {
    tft.fillRoundRect(x, baseY, w, baseH, baseH/2, outline);
    tft.fillCircle(x + (w*30)/100, y + (h*55)/100, (h*30)/100 + 2, outline);
    tft.fillCircle(x + (w*48)/100, y + (h*40)/100, (h*36)/100 + 2, outline);
    tft.fillCircle(x + (w*68)/100, y + (h*55)/100, (h*30)/100 + 2, outline);
  }

  // Fill
  tft.fillRoundRect(x+1, baseY+1, w-2, baseH-2, (baseH/2)-1, fill);
  tft.fillCircle(x + (w*30)/100, y + (h*55)/100, (h*30)/100, fill);
  tft.fillCircle(x + (w*48)/100, y + (h*40)/100, (h*36)/100, fill);
  tft.fillCircle(x + (w*68)/100, y + (h*55)/100, (h*30)/100, fill);
}

void drawMiniIcon(int cx, int cy, const String& cond){
  uint16_t cloudFill = tft.color565(220,230,240);
  uint16_t cloudOut  = tft.color565(170,185,200);

  if (cond == "sunny") {
    // kleine Sonne
    uint16_t s = tft.color565(255, 210, 80);
    tft.fillCircle(cx, cy, 6, s);
    for (int a = 0; a < 360; a += 60) {
      float rad = a * 0.0174533f;
      int x1 = cx + (int)(cos(rad)*9);
      int y1 = cy + (int)(sin(rad)*9);
      int x2 = cx + (int)(cos(rad)*12);
      int y2 = cy + (int)(sin(rad)*12);
      tft.drawLine(x1,y1,x2,y2,s);
    }
  }
  else if (cond == "cloudy") {
    drawCloud(cx, cy, 24, 18, cloudFill, cloudOut);
  }
  else if (cond == "rain") {
    drawCloud(cx, cy-2, 22, 16, cloudFill, cloudOut);
    uint16_t r = tft.color565(120, 170, 230);
    for (int i = -8; i <= 8; i += 6) {
      tft.drawLine(cx+i, cy+10, cx+i-2, cy+16, r);
    }
  }
  else if (cond == "snow") {
    drawCloud(cx, cy-2, 22, 16, cloudFill, cloudOut);
    uint16_t sn = tft.color565(230, 245, 255);
    tft.fillCircle(cx-6, cy+12, 1, sn);
    tft.fillCircle(cx,   cy+15, 1, sn);
    tft.fillCircle(cx+6, cy+12, 1, sn);
  }
  else {
    drawCloud(cx, cy, 24, 18, cloudFill, cloudOut);
  }
}

void drawMainCondIcon(const String& cond, int cx, int cy) {
  uint16_t cloudFill = tft.color565(220,230,240);
  uint16_t cloudOut  = tft.color565(150,170,190);

  if (cond == "sunny") {
    uint16_t s = tft.color565(255, 210, 80);
    tft.fillCircle(cx-20, cy-18, 14, s);
    for (int a = 0; a < 360; a += 30) {
      float rad = a * 0.0174533f;
      int x1 = (cx-20) + (int)(cos(rad)*18);
      int y1 = (cy-18) + (int)(sin(rad)*18);
      int x2 = (cx-20) + (int)(cos(rad)*24);
      int y2 = (cy-18) + (int)(sin(rad)*24);
      tft.drawLine(x1,y1,x2,y2,s);
    }
    drawCloud(cx+6, cy-2, 76, 50, cloudFill, cloudOut);
  }
  else if (cond == "cloudy") {
    drawCloud(cx, cy-2, 76, 50, cloudFill, cloudOut);
  }
  else if (cond == "rain") {
    drawCloud(cx, cy-6, 74, 48, cloudFill, cloudOut);
    uint16_t r = tft.color565(120, 170, 230);
    for (int i = -24; i <= 24; i += 10) {
      tft.drawLine(cx+i, cy+22, cx+i-3, cy+38, r);
    }
  }
  else if (cond == "snow") {
    drawCloud(cx, cy-6, 74, 48, cloudFill, cloudOut);
    uint16_t sn = tft.color565(230, 245, 255);
    for (int i = -18; i <= 18; i += 9) {
      tft.fillCircle(cx+i, cy+28, 2, sn);
    }
  }
  else {
    drawCloud(cx, cy-2, 76, 50, cloudFill, cloudOut);
  }
}



void overlayPage2(){

  // DHT nur alle 10s lesen (DHT11 schwankt sonst ständig)
  if (millis() - lastDhtRead > 10000) {
    lastDhtRead = millis();

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      liveInT = t;
      liveInH = (int)round(h);
    }
  }

  float inT = liveInT;
  int   inH = liveInH;

  float outT = p2.outT;
  int   outH = p2.outH;

  float netT = p2.netT;
  String netCond = p2.netCond;

  // ===== TOP BEREICH =====
  bool needTop = false;

  int inTi  = (!isnan(inT))  ? (int)round(inT)  : 9999;
  int outTi = (!isnan(outT)) ? (int)round(outT) : 9999;

  if (inTi != c_inTi)   needTop = true;
  if (inH  != c_inH)    needTop = true;
  if (outTi != c_outTi) needTop = true;
  if (outH  != c_outH)  needTop = true;

  if (needTop){
    restoreBg(0, 0, 240, 100);

    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE);

    tft.drawString("Innen", 10, 10);
    tft.drawLine(10, 28, 100, 28, TFT_DARKGREY);

    tft.setTextFont(4);
    if (!isnan(inT))
      tft.drawString(String((int)round(inT)) + "°C", 10, 35);

    tft.setTextFont(2);
    if (inH >= 0)
      tft.drawString(String(inH) + "%", 10, 65);

    tft.setTextDatum(TR_DATUM);
    tft.setTextFont(2);
    tft.drawString("Aussen", 230, 10);
    tft.drawLine(140, 28, 230, 28, TFT_DARKGREY);

    tft.setTextFont(4);
    if (!isnan(outT))
      tft.drawString(String((int)round(outT)) + "°C", 230, 35);

    tft.setTextFont(2);
    if (outH >= 0)
      tft.drawString(String(outH) + "%", 230, 65);

    c_inTi  = inTi;
    c_inH   = inH;
    c_outTi = outTi;
    c_outH  = outH;
  }

  // ===== MITTE =====
  bool needMid = false;

  if (!isnan(netT) && (isnan(c_netT) || fabs(netT-c_netT) > 0.1)) needMid = true;
  if (netCond != c_netCond) needMid = true;

  if (needMid){
  restoreBg(0, 100, 240, 110);

  int cx = 120;
  int cy = 115;

  // ===== Wetter Icon =====
  drawMainCondIcon(netCond, cx, cy);


  // ===== Temperatur =====
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(7);
  tft.setTextColor(TFT_WHITE);

  if (!isnan(netT))
    tft.drawString(String((int)round(netT)) + "°C", 120, 162);

  // ===== Text =====
  tft.setTextFont(4);

  String label =
    (netCond=="sunny") ? "Sonnig" :
    (netCond=="cloudy") ? "Wolkig" :
    (netCond=="rain") ? "Regen" :
    (netCond=="snow") ? "Schnee" :
    "-";

  tft.drawString(label, 120, 205);

  c_netT = netT;
  c_netCond = netCond;
}

// ===== FORECAST =====
  bool needFc = false;

  String newSig = "";
  for (int i=0;i<5;i++){
    newSig += p2.fc[i].d;
    newSig += String(p2.fc[i].hi);
    newSig += String(p2.fc[i].lo);
  }

  if (newSig != c_fcSig) needFc = true;

  if (needFc){
    restoreBg(0, 230, 240, 90);

    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE);

    for (int i=0;i<5;i++){
  int col = 24 + i*48;

  /// Icon tiefer, damit es den Wochentag nicht verdeckt
  drawMiniIcon(col, 252, p2.fc[i].cond);

  // Wochentag danach zeichnen -> bleibt immer sichtbar
  tft.setTextColor(TFT_WHITE);
  tft.drawString(p2.fc[i].d, col, 232);

  // High Temperatur (etwas tiefer)
  tft.setTextColor(TFT_WHITE);
  tft.drawString(String(p2.fc[i].hi) + "°", col, 268);

  // Low Temperatur (etwas tiefer)
  tft.setTextColor(COL_CYAN);
  tft.drawString(String(p2.fc[i].lo) + "°", col, 286);
}

    c_fcSig = newSig;
  }
}


// =====================
// Setup / Loop
// =====================
void setup() {
  Serial.begin(115200);
  delay(200);
  dht.begin();
  Serial.println("[DHT] begin ok (GPIO27, DHT11)");


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
  client.setBufferSize(4096);

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
  publishInsideIfDue();


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
    if (currentPage==PAGE2) overlayPage2();
    // PAGE2 / PAGE3 Overlays kommen als nächste Steps
  }
}



