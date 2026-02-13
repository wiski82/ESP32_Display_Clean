#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>

TFT_eSPI tft;

#define SD_CS 5

const char* pages[] = {
  "/page1.raw",
  "/page2.raw",
  "/page3.raw"
};

static const int WIDTH  = 240;
static const int HEIGHT = 320;

int currentPage = 0;
unsigned long lastSwitch = 0;
const unsigned long interval = 5000; // 5 Sekunden

void drawRaw(const char* filename) {
  File file = SD.open(filename, FILE_READ);
  if (!file) {
    Serial.println("FILE FAIL");
    return;
  }

  static uint16_t lineBuffer[WIDTH];

  for (int y = 0; y < HEIGHT; y++) {
    int bytesRead = file.read((uint8_t*)lineBuffer, WIDTH * 2);
    if (bytesRead != WIDTH * 2) break;
    tft.pushImage(0, y, WIDTH, 1, lineBuffer);
  }

  file.close();
}

void setup() {
  Serial.begin(115200);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  if (!SD.begin(SD_CS)) {
    Serial.println("SD FAIL");
    return;
  }

  drawRaw(pages[currentPage]);
  lastSwitch = millis();
}

void loop() {
  if (millis() - lastSwitch > interval) {
    currentPage++;
    if (currentPage > 2) currentPage = 0;

    drawRaw(pages[currentPage]);
    lastSwitch = millis();

    Serial.print("Page: ");
    Serial.println(currentPage + 1);
  }
}


