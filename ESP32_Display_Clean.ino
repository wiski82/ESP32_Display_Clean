#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>

TFT_eSPI tft;

#define SD_CS 5
const char* RAW_FILE = "/page1.raw";

static const int WIDTH  = 240;
static const int HEIGHT = 320;

void setup() {
  Serial.begin(115200);

  // Backlight einschalten
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  tft.setTextFont(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, 10);
  tft.println("RAW TEST...");

  // SD starten
  if (!SD.begin(SD_CS)) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 40);
    tft.println("SD FAIL");
    Serial.println("SD FAIL");
    return;
  }

  // Datei öffnen
  File file = SD.open(RAW_FILE, FILE_READ);
  if (!file) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 40);
    tft.println("FILE FAIL");
    Serial.println("FILE FAIL");
    return;
  }

  Serial.print("File size: ");
  Serial.println(file.size());

  if (file.size() != WIDTH * HEIGHT * 2) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, 40);
    tft.println("SIZE WARN");
  }

  static uint16_t lineBuffer[WIDTH];

  for (int y = 0; y < HEIGHT; y++) {
    int bytesRead = file.read((uint8_t*)lineBuffer, WIDTH * 2);
    if (bytesRead != WIDTH * 2) {
      Serial.println("Read error!");
      break;
    }
    tft.pushImage(0, y, WIDTH, 1, lineBuffer);
  }

  file.close();
  Serial.println("RAW DRAW DONE");
}

void loop() {}

