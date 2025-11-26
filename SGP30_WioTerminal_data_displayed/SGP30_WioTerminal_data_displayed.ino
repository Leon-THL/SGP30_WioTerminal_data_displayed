/*
  SGP30_WioTerminal_flash_fixed2_raw.ino
  Same as the working flash-based sketch but now reads and displays raw H2 / Ethanol.
  If your Adafruit_SGP30 version uses different APIs, see the notes below.
*/

#include <Wire.h>
#include <Adafruit_SGP30.h>
#include <TFT_eSPI.h>
#include <Adafruit_SHT31.h>
#include <FlashStorage_SAMD.h>

#define GRAPH_LEN 200
#define SAMPLE_INTERVAL_MS 1000UL
#define BASELINE_SAVE_INTERVAL_MS (10UL * 60UL * 1000UL) // 10 minutes
#define FLASH_MAGIC 0x53475030UL // 'SGP0'

// ----- Flash storage structure -----
struct FlashData {
  uint32_t magic;
  uint16_t baseline_eCO2;
  uint16_t baseline_TVOC;
  uint8_t manualRH;
  uint8_t flags; // bit0: humidityModeManual
};
FlashStorage(flashdata, FlashData);
// -----------------------------------

Adafruit_SGP30 sgp;
TFT_eSPI tft = TFT_eSPI();
Adafruit_SHT31 sht31 = Adafruit_SHT31();

uint16_t eCO2 = 0, TVOC = 0;
uint16_t rawH2 = 0, rawEthanol = 0;

uint16_t eCO2Hist[GRAPH_LEN];
uint16_t tvocHist[GRAPH_LEN];
uint16_t histIndex = 0;

unsigned long lastSampleMillis = 0;
unsigned long lastBaselineSaveMillis = 0;

uint8_t manualRH = 50;
bool humidityModeManual = false;

void loadSettingsFromFlash();
void saveSettingsToFlash();
void saveBaselineToFlash();
bool loadBaselineFromFlash_and_apply();
void applyHumidityCompensation(float tempC, float relHumPercent);
float computeAbsoluteHumidity_gm3(float tempC, float relHumPercent);
void drawUIHeader();
void drawValues();
void drawGraphs();
void addHistory(uint16_t eco2v, uint16_t tvocv);
void processSerialCommands();

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) ; // short wait for Serial (USB)

  Serial.println("SGP30 Wio Terminal - using Flash storage (raw values enabled)");

  Wire.begin();

  // TFT init
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  loadSettingsFromFlash();

  if (!sgp.begin()) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 20);
    tft.setTextSize(2);
    tft.println("SGP30 not found!");
    Serial.println("SGP30 not found! Halting.");
    while (1) delay(1000);
  }

  if (!sgp.IAQinit()) {
    Serial.println("IAQinit failed");
  }

  if (loadBaselineFromFlash_and_apply()) {
    Serial.println("Baseline loaded from flash and applied.");
  } else {
    Serial.println("No valid baseline in flash.");
  }

  bool shtOk = false;
  if (sht31.begin(0x44)) {
    shtOk = true;
    Serial.println("SHT31 found at 0x44.");
  } else if (sht31.begin(0x45)) {
    shtOk = true;
    Serial.println("SHT31 found at 0x45.");
  } else {
    Serial.println("SHT31 not found. Manual RH available via serial.");
  }
  if (shtOk && !humidityModeManual) humidityModeManual = false;

  tft.fillScreen(TFT_BLACK);
  drawUIHeader();
  drawGraphs();

  lastSampleMillis = millis();
  lastBaselineSaveMillis = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - lastSampleMillis >= SAMPLE_INTERVAL_MS) {
    lastSampleMillis = now;

    // Primary IAQ reading (eCO2 / TVOC)
    if (sgp.IAQmeasure()) {
      eCO2 = sgp.eCO2;
      TVOC = sgp.TVOC;
    } else {
      Serial.println("IAQmeasure failed");
    }

    // === RAW H2 / Ethanol reading ===
    // Most Adafruit_SGP30 variants provide IAQmeasureRaw() that returns true and populates
    // sgp.rawH2 and sgp.rawEthanol — use that if available.
    // If your library uses a different API, see the notes below to change the code.
    if (sgp.IAQmeasureRaw()) {
      // read the public members populated by IAQmeasureRaw()
      rawH2 = sgp.rawH2;
      rawEthanol = sgp.rawEthanol;
    } else {
      // If IAQmeasureRaw() is not present in your version, use one of the fallbacks:
      // 1) If the library exposes: bool measureRaw(uint16_t *h2, uint16_t *eth)
      //    uncomment the line below and comment out the IAQmeasureRaw() call above.
      // if (sgp.measureRaw(&rawH2, &rawEthanol)) { /* values now in rawH2/rawEthanol */ }
      //
      // 2) If the library exposes: bool measureRaw(uint16_t &h2, uint16_t &eth)
      //    uncomment and adapt the line below:
      // if (sgp.measureRaw(rawH2, rawEthanol)) { /* values now in rawH2/rawEthanol */ }
      //
      // If neither exists, rawH2/rawEthanol will stay at their previous values (default 0).
    }

    addHistory(eCO2, TVOC);
    drawValues();
    drawGraphs();

    // humidity compensation
    if (humidityModeManual) {
      applyHumidityCompensation(25.0f, (float)manualRH); // assume 25°C if no temp sensor
    } else {
      float t = sht31.readTemperature();
      float rh = sht31.readHumidity();
      if (!isnan(t) && !isnan(rh)) {
        applyHumidityCompensation(t, rh);
      } else {
        applyHumidityCompensation(25.0f, (float)manualRH);
      }
    }

    if (now - lastBaselineSaveMillis >= BASELINE_SAVE_INTERVAL_MS) {
      saveBaselineToFlash();
      lastBaselineSaveMillis = now;
    }
  }

  processSerialCommands();

  delay(5);
}

// ---------- Flash storage functions ----------
void loadSettingsFromFlash() {
  FlashData d;
  flashdata.read(d);
  if (d.magic == FLASH_MAGIC) {
    manualRH = d.manualRH;
    humidityModeManual = (d.flags & 0x01) != 0;
    Serial.print("Flash settings loaded: manualRH=");
    Serial.print(manualRH);
    Serial.print(" mode=");
    Serial.println(humidityModeManual ? "manual" : "auto");
  } else {
    manualRH = 50;
    humidityModeManual = false;
    Serial.println("No flash settings; using defaults.");
  }
}

void saveSettingsToFlash() {
  FlashData d;
  flashdata.read(d);
  if (d.magic != FLASH_MAGIC) {
    d.magic = FLASH_MAGIC;
    d.baseline_eCO2 = 0;
    d.baseline_TVOC = 0;
  }
  d.manualRH = manualRH;
  d.flags = humidityModeManual ? 0x01 : 0x00;
  flashdata.write(d);
  Serial.println("Settings saved to flash.");
}

void saveBaselineToFlash() {
  uint16_t basE = 0, basT = 0;
  if (sgp.getIAQBaseline(&basE, &basT)) {
    FlashData d;
    flashdata.read(d);
    d.magic = FLASH_MAGIC;
    d.baseline_eCO2 = basE;
    d.baseline_TVOC = basT;
    d.manualRH = manualRH;
    d.flags = humidityModeManual ? 0x01 : 0x00;
    flashdata.write(d);
    Serial.print("Saved baseline to flash: eCO2=");
    Serial.print(basE);
    Serial.print(" TVOC=");
    Serial.println(basT);
  } else {
    Serial.println("Failed to read baseline from SGP30; not saving.");
  }
}

bool loadBaselineFromFlash_and_apply() {
  FlashData d;
  flashdata.read(d);
  if (d.magic != FLASH_MAGIC) return false;
  if (d.baseline_eCO2 == 0 && d.baseline_TVOC == 0) return false;
  if (sgp.setIAQBaseline(d.baseline_eCO2, d.baseline_TVOC)) {
    Serial.print("Applied baseline from flash: eCO2=");
    Serial.print(d.baseline_eCO2);
    Serial.print(" TVOC=");
    Serial.println(d.baseline_TVOC);
    return true;
  } else {
    Serial.println("SGP30 rejected baseline (setIAQBaseline returned false).");
    return false;
  }
}

// ---------- Humidity compensation ----------
void applyHumidityCompensation(float tempC, float relHumPercent) {
  float ah = computeAbsoluteHumidity_gm3(tempC, relHumPercent);
  uint32_t ah_ticks = (uint32_t)(ah * 1000.0f + 0.5f); // typical scaling for Adafruit_SGP30
  if (!sgp.setHumidity(ah_ticks)) {
    // ignore if not supported
  }
}

float computeAbsoluteHumidity_gm3(float tempC, float relHumPercent) {
  float T = tempC;
  float RH = relHumPercent;
  float svp = 6.112f * expf((17.62f * T) / (243.12f + T)); // hPa
  float ah = 216.7f * (svp * (RH / 100.0f)) / (T + 273.15f); // g/m^3
  return ah;
}

// ---------- Display helpers ----------
void drawUIHeader() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, 4);
  tft.print("SGP30 IAQ");
  tft.setTextSize(1);
  tft.setCursor(8, 28);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print("eCO2 (ppm)  TVOC (ppb)  RawH2  RawEth");
}

void drawValues() {
  tft.fillRect(0, 40, 320, 84, TFT_BLACK);

  tft.setTextSize(4);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(8, 44);
  if (eCO2 == 0) tft.print("---");
  else tft.print(eCO2);
  tft.setTextSize(2);
  tft.setCursor(8 + 4 * 24, 58);
  tft.print("ppm");

  tft.setTextSize(3);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(160, 44);
  if (TVOC == 0) tft.print("---");
  else tft.print(TVOC);
  tft.setTextSize(2);
  tft.setCursor(160 + 3 * 18, 58);
  tft.print("ppb");

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(8, 92);
  tft.printf("Raw H2: %4u  Eth: %4u", rawH2, rawEthanol);

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(8, 110);
  if (humidityModeManual) {
    tft.print("Humidity: MANUAL ");
    tft.printf("%u%%", (unsigned)manualRH);
  } else {
    tft.print("Humidity: AUTO (SHT31)");
  }

  tft.setTextSize(1);
  tft.setCursor(8, 125);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.print("Serial cmd: 'h <RH>' set manual RH, 'm' toggle mode, 'save' baseline");
}

void drawGraphs() {
  const int gx = 8, gy = 140, gw = 304, gh = 88;
  tft.fillRect(gx - 1, gy - 1, gw + 2, gh + 2, TFT_BLACK);
  tft.drawRect(gx, gy, gw, gh, TFT_DARKGREY);
  for (int i = 1; i <= 3; i++) {
    int yy = gy + i * gh / 4;
    tft.drawFastHLine(gx + 1, yy, gw - 2, TFT_DARKGREY);
  }

  const float eco2Max = 2000.0f;
  const float tvocMax = 1000.0f;
  int w = gw - 8;
  if (w <= 0) return;
  int maxDraw = w;

  int prevX = -1, prevY = -1;
  for (int s = 0; s < maxDraw; s++) {
    int i = (histIndex + GRAPH_LEN - s - 1) % GRAPH_LEN;
    uint16_t v = eCO2Hist[i];
    if (v == 0 && s > histIndex) break;
    float frac = min(v / eco2Max, 1.0f);
    int y = gy + gh - 4 - (int)(frac * (gh - 8));
    int x = gx + gw - 4 - s;
    if (prevX >= 0) tft.drawLine(prevX, prevY, x, y, TFT_GREEN);
    prevX = x; prevY = y;
  }

  prevX = prevY = -1;
  for (int s = 0; s < maxDraw; s++) {
    int i = (histIndex + GRAPH_LEN - s - 1) % GRAPH_LEN;
    uint16_t v = tvocHist[i];
    if (v == 0 && s > histIndex) break;
    float frac = min(v / tvocMax, 1.0f);
    int y = gy + gh - 4 - (int)(frac * (gh - 8));
    int x = gx + gw - 4 - s;
    if (prevX >= 0) tft.drawLine(prevX, prevY, x, y, TFT_YELLOW);
    prevX = x; prevY = y;
  }

  tft.setTextSize(1);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(gx + 4, gy + 4);
  tft.print("eCO2");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(gx + 56, gy + 4);
  tft.print("TVOC");
}

void addHistory(uint16_t eco2v, uint16_t tvocv) {
  eCO2Hist[histIndex] = eco2v;
  tvocHist[histIndex] = tvocv;
  histIndex = (histIndex + 1) % GRAPH_LEN;
}

String serialLine = "";

void processSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (serialLine.length() > 0) {
        String cmd = serialLine;
        cmd.trim();
        cmd.toLowerCase();
        if (cmd.startsWith("h ")) {
          int val = cmd.substring(2).toInt();
          if (val < 0) val = 0;
          if (val > 100) val = 100;
          manualRH = (uint8_t)val;
          humidityModeManual = true;
          saveSettingsToFlash();
          Serial.print("Manual RH set to ");
          Serial.print(manualRH);
          Serial.println("% and saved to flash. Mode = MANUAL.");
          drawValues();
        } else if (cmd == "m") {
          humidityModeManual = !humidityModeManual;
          saveSettingsToFlash();
          Serial.print("Humidity mode toggled. Now ");
          Serial.println(humidityModeManual ? "MANUAL" : "AUTO");
          drawValues();
        } else if (cmd == "save") {
          saveBaselineToFlash();
          Serial.println("Forced baseline save.");
        } else if (cmd == "help") {
          Serial.println("Commands:");
          Serial.println("  h <RH>  -> set manual RH percent (0-100) and switch to manual mode");
          Serial.println("  m       -> toggle humidity mode (auto/manual)");
          Serial.println("  save    -> force save IAQ baseline to flash");
          Serial.println("  help    -> this help");
        } else {
          Serial.print("Unknown cmd: ");
          Serial.println(cmd);
          Serial.println("Type 'help' for commands.");
        }
      }
      serialLine = "";
    } else {
      serialLine += c;
      if (serialLine.length() > 64) serialLine = serialLine.substring(serialLine.length() - 64);
    }
  }
}