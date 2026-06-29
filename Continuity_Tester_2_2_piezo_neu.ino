#include <U8g2lib.h>
#include <Wire.h>
#include <EEPROM.h>

// ─── Hardware ────────────────────────────────────────────────────────────────
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, 16, 5, 4);

#define PIN_SELECT  12
#define PIN_ENTER   13
#define PIN_BUZZER  14

// ─── EEPROM ───────────────────────────────────────────────────────────────────
#define EEPROM_SIZE     68
#define EEPROM_MAGIC    0xAB
#define ADDR_MAGIC      0
#define ADDR_SOUND      1
#define ADDR_LIMIT1     2
#define ADDR_LIMIT2     6
#define ADDR_LIMIT3     10
#define ADDR_CAL_FLAG   14
#define ADDR_RAWERROR   15
#define ADDR_CAL_START  17   // 15 floats × 4 Bytes = 60 Bytes

// ─── Lookup-Tabelle (Default) ─────────────────────────────────────────────────
const int LUT_SIZE = 15;
float lut_raw[LUT_SIZE] = {   3,   32,   48,   93,  134,  169,  232,  333,  412,  498,  592,  659,  739,  817,  890 };
float lut_rx[LUT_SIZE]  = { 0.0,  0.3,  0.5,  1.0,  1.5,  2.0,  3.0,  5.0,  7.0, 10.0, 15.0, 20.0, 30.0, 50.0, 100.0 };

// Default-Werte für Reset
const float LUT_RAW_DEFAULT[LUT_SIZE] = {   3,   32,   48,   93,  134,  169,  232,  333,  412,  498,  592,  659,  739,  817,  890 };
const int   RAWERROR_DEFAULT = 42;

const int   QUICK_IDX[]  = { 0, 3, 9, 12, 14 };
const float QUICK_RX[]   = { 0.0, 1.0, 10.0, 30.0, 100.0 };
const int   QUICK_SIZE   = 5;

// ─── Globale Variablen ────────────────────────────────────────────────────────
bool  soundOn  = true;
float limit1   = 1.0;
float limit2   = 10.0;
float limit3   = 50.0;
int   rawerror = RAWERROR_DEFAULT;

char  ausgabe[24]        = "";
float cal_buffer[LUT_SIZE];   // Puffer während Kalibrierung

// ─── Töne (passiver Piezo-Schallwandler PS1240P02BT) ─────────────────────────
// Der PS1240P02BT ist ein PASSIVER Wandler — er braucht ein Wechselsignal (tone()).
// Seine Resonanzfrequenz liegt bei ca. 4 kHz; wir nutzen eine feste Frequenz
// und unterscheiden die Limit-Stufen durch Pulsmuster (An-/Aus-Zeiten).
//
// Limit 1 (≤ limit1): Dauerton 4000 Hz         → Durchgang / sehr niedrig
// Limit 2 (≤ limit2): Kurze Pulse  100ms/100ms → mittlerer Widerstand
// Limit 3 (≤ limit3): Mittlere Pulse 100ms/300ms
// > Limit 3:          Langsame Pulse  100ms/700ms

#define BUZZER_FREQ  4000   // nahe Resonanzfrequenz des PS1240P02BT

void beep(int dauer_ms) {
  tone(PIN_BUZZER, BUZZER_FREQ, dauer_ms);
  delay(dauer_ms);
  noTone(PIN_BUZZER);
}

void beepUI()  { beep(100); }
void beepUI2() { beep(100); delay(100); beep(100); }

// ─── EEPROM ───────────────────────────────────────────────────────────────────
void eepromLoad() {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(ADDR_MAGIC) != EEPROM_MAGIC) return;
  soundOn = EEPROM.read(ADDR_SOUND);
  EEPROM.get(ADDR_LIMIT1,   limit1);
  EEPROM.get(ADDR_LIMIT2,   limit2);
  EEPROM.get(ADDR_LIMIT3,   limit3);
  EEPROM.get(ADDR_RAWERROR, rawerror);
  if (EEPROM.read(ADDR_CAL_FLAG) == 1) {
    for (int i = 0; i < LUT_SIZE; i++) {
      EEPROM.get(ADDR_CAL_START + i * 4, lut_raw[i]);
    }
  }
}

void eepromSave() {
  EEPROM.write(ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.write(ADDR_SOUND, soundOn ? 1 : 0);
  EEPROM.put(ADDR_LIMIT1,   limit1);
  EEPROM.put(ADDR_LIMIT2,   limit2);
  EEPROM.put(ADDR_LIMIT3,   limit3);
  EEPROM.put(ADDR_RAWERROR, rawerror);
  EEPROM.commit();
}

// Wird nur aufgerufen wenn Kalibrierung vollständig abgeschlossen
void eepromSaveCal() {
  // Erst jetzt cal_buffer → lut_raw übernehmen
  for (int i = 0; i < LUT_SIZE; i++) {
    lut_raw[i] = cal_buffer[i];
  }
  EEPROM.write(ADDR_MAGIC,    EEPROM_MAGIC);
  EEPROM.write(ADDR_CAL_FLAG, 1);
  EEPROM.put(ADDR_RAWERROR, rawerror);
  for (int i = 0; i < LUT_SIZE; i++) {
    EEPROM.put(ADDR_CAL_START + i * 4, lut_raw[i]);
  }
  EEPROM.commit();
}

void eepromReset() {
  // Alle Werte auf Default zurücksetzen
  soundOn  = true;
  limit1   = 1.0;
  limit2   = 10.0;
  limit3   = 50.0;
  rawerror = RAWERROR_DEFAULT;
  for (int i = 0; i < LUT_SIZE; i++) {
    lut_raw[i] = LUT_RAW_DEFAULT[i];
  }
  EEPROM.write(ADDR_MAGIC,    EEPROM_MAGIC);
  EEPROM.write(ADDR_SOUND,    1);
  EEPROM.write(ADDR_CAL_FLAG, 0);
  EEPROM.put(ADDR_LIMIT1,   limit1);
  EEPROM.put(ADDR_LIMIT2,   limit2);
  EEPROM.put(ADDR_LIMIT3,   limit3);
  EEPROM.put(ADDR_RAWERROR, rawerror);
  EEPROM.commit();
}

// ─── Display ──────────────────────────────────────────────────────────────────
void dispText(const char* z1, const char* z2 = nullptr) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  if (z1) u8g2.drawStr(0, 12, z1);
  if (z2) u8g2.drawStr(0, 28, z2);
  u8g2.sendBuffer();
}

void dispGroß(const char* text, int x_offset = 0) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_logisoso24_tf);
  int breite = u8g2.getUTF8Width(text);
  int x = (128 - breite) / 2 + x_offset;
  u8g2.drawUTF8(x, 28, text);
  u8g2.sendBuffer();
}

void dispMenu(const char** items, int anzahl, int sel) {
  char z1[24], z2[24];
  int idx2 = (sel + 1) % anzahl;
  snprintf(z1, sizeof(z1), "> %s", items[sel]);
  snprintf(z2, sizeof(z2), "  %s", items[idx2]);
  dispText(z1, z2);
}

// ─── Tasten ───────────────────────────────────────────────────────────────────
bool selectPressed() { return digitalRead(PIN_SELECT) == LOW; }
bool enterPressed()  { return digitalRead(PIN_ENTER)  == LOW; }

void waitRelease() {
  while (selectPressed() || enterPressed()) delay(10);
  delay(50);
}

bool abbruchGedrueckt() {
  if (selectPressed() && enterPressed()) {
    int t = 0;
    while (selectPressed() && enterPressed()) {
      delay(100); t += 100;
      if (t >= 1000) {
        beepUI();
        dispText("Abbruch", "");
        delay(1000);
        waitRelease();
        return true;
      }
    }
  }
  return false;
}

int enterHaltezeit(int schwelle_ms = 0, const char* anzeigeText = nullptr) {
  int t = 0;
  bool signalGegeben = false;
  while (enterPressed()) {
    delay(100); t += 100;
    if (!signalGegeben && schwelle_ms > 0 && t >= schwelle_ms) {
      beepUI();
      if (anzeigeText) dispText(anzeigeText, "");
      signalGegeben = true;
    }
  }
  return t;
}

// ─── Lookup ───────────────────────────────────────────────────────────────────
float lookup_rx(int raw) {
  if (raw <= lut_raw[0])            return 0.0;
  if (raw >= lut_raw[LUT_SIZE - 1]) return 9999.0;
  for (int i = 1; i < LUT_SIZE; i++) {
    if (raw <= lut_raw[i]) {
      float divisor = lut_raw[i] - lut_raw[i-1];
      if (divisor == 0) return lut_rx[i-1];  // Schutz vor nan
      float t = (raw - lut_raw[i-1]) / divisor;
      return lut_rx[i-1] + t * (lut_rx[i] - lut_rx[i-1]);
    }
  }
  return 9999.0;
}

// ─── Kalibrierung ─────────────────────────────────────────────────────────────
bool calEinzelMessung(int idx, float r_soll) {
  char z1[24];
  if (r_soll == 0.0)
    snprintf(z1, sizeof(z1), "Kurzschluss anlegen");
  else
    snprintf(z1, sizeof(z1), "%.1f Ohm anlegen", r_soll);
  dispText(z1, "ENTER druecken");

  while (true) {
    if (abbruchGedrueckt()) return false;
    if (enterPressed()) {
      waitRelease();
      int raw = analogRead(A0) - rawerror;
      if (raw < 0) raw = 0;
      cal_buffer[idx] = raw;    // in Puffer, nicht direkt in lut_raw
      char z2[24];
      snprintf(z2, sizeof(z2), "RAW=%d OK", (int)cal_buffer[idx]);
      dispText(z1, z2);
      beepUI();
      delay(1000);
      return true;
    }
    delay(20);
  }
}

void interpoliereQuick() {
  for (int seg = 0; seg < QUICK_SIZE - 1; seg++) {
    int i0 = QUICK_IDX[seg];
    int i1 = QUICK_IDX[seg + 1];
    float raw0 = cal_buffer[i0];
    float raw1 = cal_buffer[i1];
    for (int i = i0 + 1; i < i1; i++) {
      float t = (lut_rx[i] - lut_rx[i0]) / (lut_rx[i1] - lut_rx[i0]);
      cal_buffer[i] = raw0 + t * (raw1 - raw0);
    }
  }
}

bool menuCalQuick() {
  dispText("Schnellkal. 5 Pkte", "ENTER zum Start");
  waitRelease();
  while (!enterPressed()) {
    if (abbruchGedrueckt()) return false;
    delay(20);
  }
  waitRelease();

  dispText("Klemmen kurzschl.", "dann ENTER");
  while (true) {
    if (abbruchGedrueckt()) return false;
    if (enterPressed()) {
      rawerror = analogRead(A0);
      waitRelease();
      beepUI();
      break;
    }
    delay(20);
  }

  for (int i = 0; i < QUICK_SIZE; i++) {
    if (!calEinzelMessung(QUICK_IDX[i], QUICK_RX[i])) return false;
  }
  interpoliereQuick();
  eepromSaveCal();
  dispText("Kalibrierung OK", "Werte gespeichert");
  beepUI2();
  delay(2000);
  return true;
}

bool menuCalFull() {
  dispText("Vollkal. 15 Pkte", "ENTER zum Start");
  waitRelease();
  while (!enterPressed()) {
    if (abbruchGedrueckt()) return false;
    delay(20);
  }
  waitRelease();

  dispText("Klemmen kurzschl.", "dann ENTER");
  while (true) {
    if (abbruchGedrueckt()) return false;
    if (enterPressed()) {
      rawerror = analogRead(A0);
      waitRelease();
      beepUI();
      break;
    }
    delay(20);
  }

  for (int i = 0; i < LUT_SIZE; i++) {
    if (!calEinzelMessung(i, lut_rx[i])) return false;
  }
  eepromSaveCal();
  dispText("Kalibrierung OK", "Werte gespeichert");
  beepUI2();
  delay(2000);
  return true;
}

void menuCalibration() {
  const char* items[] = { "Quick (5 Punkte)", "Full  (15 Punkte)" };
  const int anzahl = 2;
  int sel = 0;

  while (true) {
    dispMenu(items, anzahl, sel);
    if (selectPressed()) { sel = (sel + 1) % anzahl; waitRelease(); delay(150); }
    if (enterPressed()) {
      waitRelease();
      dispText("ENTER 3s halten", "zum Starten");
      // Warten bis ENTER neu gedrückt wird
      while (!enterPressed()) delay(20);
      int t = enterHaltezeit(3000, "Calibration...");
      if (t >= 3000) {
        waitRelease();
        if (sel == 0) menuCalQuick();
        else          menuCalFull();
        return;
      } else {
        waitRelease();
        dispText("Abgebrochen", "");
        delay(1000);
      }
    }
    delay(50);
  }
}

// ─── Reset Default ────────────────────────────────────────────────────────────
void menuReset() {
  dispText("ENTER 3s halten", "fuer Reset");
  // Warten bis ENTER neu gedrückt wird
  while (!enterPressed()) delay(20);
  int t = enterHaltezeit(3000, "Reset...");
  if (t >= 3000) {
    waitRelease();
    eepromReset();
    dispText("Reset OK", "Default-Werte aktiv");
    beepUI2();
    delay(2000);
  } else {
    waitRelease();
    dispText("Abgebrochen", "");
    delay(1000);
  }
}

// ─── Limits ───────────────────────────────────────────────────────────────────
void setEinLimit(int nr, float* limit) {
  char z1[24];
  snprintf(z1, sizeof(z1), "Limit %d anlegen", nr);
  dispText(z1, "dann ENTER");
  waitRelease();
  while (!enterPressed()) delay(20);
  int raw = analogRead(A0) - rawerror;
  if (raw < 0) raw = 0;
  float r = lookup_rx(raw);
  *limit = r;
  char z2[24];
  snprintf(z2, sizeof(z2), "= %.1f Ohm", r);
  dispText(z1, z2);
  beepUI();
  waitRelease();
  delay(1500);
}

void menuLimits() {
  const char* items[] = { "Limit 1", "Limit 2", "Limit 3" };
  const int anzahl = 3;
  int sel = 0;
  while (true) {
    dispMenu(items, anzahl, sel);
    if (selectPressed()) { sel = (sel + 1) % anzahl; waitRelease(); delay(150); }
    if (enterPressed()) {
      waitRelease();
      if      (sel == 0) setEinLimit(1, &limit1);
      else if (sel == 1) setEinLimit(2, &limit2);
      else               setEinLimit(3, &limit3);
      eepromSave();
      return;
    }
    delay(50);
  }
}

// ─── Sound ────────────────────────────────────────────────────────────────────
void menuSound() {
  const char* items[] = { "ON", "OFF" };
  const int anzahl = 2;
  int sel = soundOn ? 0 : 1;
  while (true) {
    dispMenu(items, anzahl, sel);
    if (selectPressed()) { sel = (sel + 1) % anzahl; waitRelease(); }
    if (enterPressed()) {
      soundOn = (sel == 0);
      eepromSave();
      beepUI();
      waitRelease();
      return;
    }
    delay(50);
  }
}

// ─── Hauptmenü ────────────────────────────────────────────────────────────────
void hauptmenu() {
  const char* items[] = { "Sound ON/OFF", "Set Limits", "Calibration", "Reset Default" };
  const int anzahl = 4;
  int sel = 0;
  while (true) {
    dispMenu(items, anzahl, sel);
    if (selectPressed()) { sel = (sel + 1) % anzahl; waitRelease(); delay(150); }
    if (enterPressed()) {
      waitRelease();
      if      (sel == 0) menuSound();
      else if (sel == 1) menuLimits();
      else if (sel == 2) menuCalibration();
      else               menuReset();
      return;
    }
    delay(50);
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_SELECT, INPUT_PULLUP);
  pinMode(PIN_ENTER,  INPUT_PULLUP);
  Serial.begin(9600);
  u8g2.begin();
  // EINMALIG — nach dem Flashen wieder entfernen!
  // EEPROM.begin(EEPROM_SIZE);
  // EEPROM.write(ADDR_MAGIC,    0x00);
  // EEPROM.write(ADDR_CAL_FLAG, 0x00);
  // EEPROM.commit();
  eepromLoad();
  // cal_buffer initiallisieren
  for (int i = 0; i < LUT_SIZE; i++) {
    cal_buffer[i] = lut_raw[i];
  }
  // Debug: lut_raw[] und rawerror ausgeben
  Serial.print("rawerror: "); Serial.println(rawerror);
  for (int i = 0; i < LUT_SIZE; i++) {
    Serial.print("lut_raw["); Serial.print(i);
    Serial.print("] = "); Serial.println(lut_raw[i]);
  }
  dispText("Continuity Tester", "art-of-electronics");
  delay(2000);
}

// ─── Hauptschleife ────────────────────────────────────────────────────────────
void loop() {
  if (enterPressed()) {
    int t = enterHaltezeit(5000, "Menue");
    if (t >= 5000) {
      waitRelease();
      hauptmenu();
      return;
    }
  }

  int adc_roh = analogRead(A0) - rawerror;
  if (adc_roh < 0) adc_roh = 0;
  float r_x = lookup_rx(adc_roh);

  Serial.print("RAW: "); Serial.print(adc_roh);
  Serial.print("  Rx: "); Serial.println(r_x, 1);

  if (r_x >= 9999.0) {
    dispGroß("Overflow");
  } else {
    char zahl[12] = "";
    if (r_x < 10.0) dtostrf(r_x, 4, 1, zahl);
    else             dtostrf(r_x, 4, 0, zahl);
    snprintf(ausgabe, sizeof(ausgabe), "%s Ohm", zahl);
    dispGroß(ausgabe, -8);
  }

  if (soundOn && r_x < 9999.0) {
    // ── Passiver Piezo PS1240P02BT: tone() + Pulsmuster je Widerstandsbereich ─
    // Limit 1 (≤ limit1): Dauerton                  → Durchgang / sehr niedrig
    // Limit 2 (≤ limit2): Kurze Pause  100ms/100ms  → mittlerer Widerstand
    // Limit 3 (≤ limit3): Mittlere Pause 100ms/300ms
    // > Limit 3:          Langsame Pause  100ms/700ms
    unsigned long start = millis();
    while (millis() - start < 3000) {
      int raw2 = analogRead(A0) - rawerror;
      if (raw2 < 0) raw2 = 0;
      float r2 = lookup_rx(raw2);
      if (r2 >= 9999.0) {
        noTone(PIN_BUZZER);
        break;
      }
      if (r2 <= limit1) {
        // Dauerton: tone() läuft durch, nur kurz neu getriggert
        tone(PIN_BUZZER, BUZZER_FREQ);
        delay(100);
      } else if (r2 <= limit2) {
        // Kurze Pulse: 100ms an / 100ms aus
        tone(PIN_BUZZER, BUZZER_FREQ);
        delay(100);
        noTone(PIN_BUZZER);
        delay(100);
      } else if (r2 <= limit3) {
        // Mittlere Pulse: 100ms an / 300ms aus
        tone(PIN_BUZZER, BUZZER_FREQ);
        delay(100);
        noTone(PIN_BUZZER);
        delay(300);
      } else {
        // Langsame Pulse: 100ms an / 700ms aus
        tone(PIN_BUZZER, BUZZER_FREQ);
        delay(100);
        noTone(PIN_BUZZER);
        delay(700);
      }
    }
    noTone(PIN_BUZZER);
  }

  delay(200);
}
