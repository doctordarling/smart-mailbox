/*
 * ============================================
 *   Briefkasten-Wächter v2.2 - LoRa Sender
 *   Board: LILYGO T-Beam v1.2 (AXP2101)
 * ============================================
 * 
 * Funktion:
 *   - Deep Sleep bis Briefkastenklappe geöffnet wird
 *   - Reed-Switch (NC) an GPIO25 löst Wakeup aus
 *   - Sendet LoRa-Nachricht an Gateway in der Wohnung
 *   - Gateway macht den ntfy.sh Push (WLAN braucht der T-Beam nicht mehr!)
 * 
 * Änderungen ggü. v2.1:
 *   - OLED-Display per I2C-Befehl in Schlafmodus (~15mA gespart)
 *     Auf T-Beam v1.2 hängt OLED auf DCDC1 zusammen mit dem ESP32,
 *     kann also nicht per PMU abgeschaltet werden. Stattdessen per
 *     SSD1306-Kommando 0xAE (Display off) + 0x8D 0x10 (Charge Pump off).
 *   - Lade-LED ausschalten (~5-10mA gespart)
 *
 * Änderungen v2.0 → v2.1:
 *   - GPS-Modul (ALDO3) wird deaktiviert: ~30-50mA gespart
 *   - Weitere ungenutzte Spannungsschienen aus (ALDO4, BLDO1, BLDO2)
 *   - LoRa-Power (ALDO2) wird vor Deep Sleep abgeschaltet
 * 
 * Hardware:
 *   - Reed-Switch: blau(COM)→GND, schwarz(NC)→GPIO25
 *   - LoRa: SX1276 auf T-Beam (868 MHz)
 *   - PMU: AXP2101 (Akku-Management)
 *   - OLED: SSD1306 auf I2C-Adresse 0x3C
 * 
 * Libraries (Arduino IDE → Sketch → Bibliothek einbinden → Bibliotheken verwalten):
 *   - RadioLib       (Suche: "RadioLib")
 *   - XPowersLib     (Suche: "XPowersLib")
 */

#include <RadioLib.h>
#include <XPowersLib.h>
#include <SPI.h>
#include <Wire.h>
#include "driver/rtc_io.h"

// ==========================================
//  Pin-Belegung T-Beam v1.2
// ==========================================

// LoRa SX1276
#define LORA_SCK   5
#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_CS    18
#define LORA_RST   23
#define LORA_DIO0  26
#define LORA_DIO1  33

// Reed-Switch (NC = Normally Closed)
#define REED_PIN   25

// I2C für AXP2101
#define I2C_SDA    21
#define I2C_SCL    22

// ==========================================
//  LoRa Parameter (MÜSSEN identisch zum Gateway sein!)
// ==========================================
#define LORA_FREQ       868.0   // MHz
#define LORA_BW         125.0   // kHz
#define LORA_SF         12      // Spreading Factor (max Reichweite)
#define LORA_CR         5       // Coding Rate 4/5
#define LORA_SYNC       0x12    // Sync Word
#define LORA_POWER      17      // dBm (max für SX1276 mit PA_BOOST)
#define LORA_PREAMBLE   8       // Preamble Length

// ==========================================
//  RTC Memory (überlebt Deep Sleep)
// ==========================================
RTC_DATA_ATTR int openCount = 0;

// ==========================================
//  Globale Objekte
// ==========================================
XPowersAXP2101 pmu;
SX1276 radio = new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1);

// ==========================================
//  Setup (läuft nach jedem Wakeup)
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(100);

  // I2C früh initialisieren (für OLED-Sleep, auch bei Fehlauslösung)
  Wire.begin(I2C_SDA, I2C_SCL);

  openCount++;
  Serial.println();
  Serial.println("========================================");
  Serial.println("  Briefkasten-Wächter v2.2 (LoRa)");
  Serial.printf("  Öffnung #%d\n", openCount);
  Serial.println("========================================");

  // --- Reed-Switch Entprellung ---
  if (!debounceReed()) {
    Serial.println("Fehlauslösung erkannt → zurück in Deep Sleep");
    goToSleep();
    return;
  }
  Serial.println("Klappe geöffnet bestätigt!");

  // --- PMU initialisieren ---
  int battPercent = -1;
  if (initPMU()) {
    battPercent = pmu.getBatteryPercent();
    Serial.printf("Akku: %d%%\n", battPercent);
  } else {
    Serial.println("PMU Fehler - weiter ohne Akku-Info");
  }

  // --- LoRa initialisieren ---
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

  Serial.println("LoRa initialisieren...");
  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_POWER, LORA_PREAMBLE);

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("LoRa FEHLER! Code: %d\n", state);
    Serial.println("Mögliche Ursachen:");
    Serial.println("  - SPI Verbindung prüfen");
    Serial.println("  - PMU: LoRa nicht mit Strom versorgt?");
    goToSleep();
    return;
  }
  Serial.println("LoRa bereit!");

  // --- Nachricht zusammenbauen ---
  // Format: MAIL|öffnungsnummer|akkuprozent
  // Beispiel: MAIL|5|82
  String msg = "MAIL|" + String(openCount) + "|" + String(battPercent);

  // --- Senden ---
  Serial.printf("Sende LoRa: \"%s\"\n", msg.c_str());

  state = radio.transmit(msg);

  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(">>> Erfolgreich gesendet! <<<");
  } else if (state == RADIOLIB_ERR_TX_TIMEOUT) {
    Serial.println("Sendefehler: Timeout");
  } else {
    Serial.printf("Sendefehler: Code %d\n", state);
  }

  // --- Schlafen ---
  goToSleep();
}

// ==========================================
//  Loop (wird nie erreicht)
// ==========================================
void loop() {
  // T-Beam geht nach setup() direkt in Deep Sleep
}

// ==========================================
//  Reed-Switch Entprellung
// ==========================================
bool debounceReed() {
  pinMode(REED_PIN, INPUT_PULLUP);
  int lowCount = 0;

  // 5 Messungen über 150ms
  for (int i = 0; i < 5; i++) {
    if (digitalRead(REED_PIN) == LOW) {
      lowCount++;
    }
    delay(30);
  }

  Serial.printf("Entprellung: %d/5 LOW\n", lowCount);
  return (lowCount >= 3);  // Mindestens 3 von 5 LOW = Klappe offen
}

// ==========================================
//  PMU (AXP2101) initialisieren
// ==========================================
bool initPMU() {
  if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
    return false;
  }

  // LoRa-Modul mit Strom versorgen (ALDO2 = 3.3V)
  pmu.setALDO2Voltage(3300);
  pmu.enableALDO2();
  delay(50);  // Kurz warten bis Spannung stabil

  // STROMSPAREN: GPS und andere ungenutzte Verbraucher abschalten
  // ALDO3 = GPS-Modul (NEO-6M zieht ~30-50mA, riesiger Akkufresser!)
  // ALDO4, BLDO1, BLDO2 = sonstige Peripherie, nicht benötigt
  pmu.disableALDO3();
  pmu.disableALDO4();
  pmu.disableBLDO1();
  pmu.disableBLDO2();

  // Lade-LED ausschalten (zieht im "always on"-Default 5-10mA)
  pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);

  return true;
}

// ==========================================
//  OLED in Schlafmodus versetzen
//  SSD1306 hängt auf DCDC1 (zusammen mit ESP32),
//  kann nicht per PMU abgeschaltet werden.
//  Aber: per I2C-Befehl kann der Chip selbst in Sleep gehen.
// ==========================================
void sleepOLED() {
  const uint8_t OLED_ADDR = 0x3C;  // Standard-Adresse des SSD1306 auf T-Beam

  // Display OFF
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00);  // Co=0, D/C=0 (Befehlsmodus)
  Wire.write(0xAE);  // Display OFF
  Wire.endTransmission();

  // Charge Pump deaktivieren (spart nochmal ein paar mA)
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00);
  Wire.write(0x8D);  // Charge Pump Setting
  Wire.write(0x10);  // Charge Pump OFF
  Wire.endTransmission();
}

// ==========================================
//  Deep Sleep konfigurieren und starten
// ==========================================
void goToSleep() {
  // OLED-Chip in Schlafmodus (spart ~15mA)
  sleepOLED();

  // LoRa in Sleep-Modus (Strom sparen)
  radio.sleep();

  // LoRa-Spannung komplett aus (im Sleep brauchen wir kein LoRa)
  // Wird beim nächsten Wakeup in initPMU() wieder eingeschaltet.
  pmu.disableALDO2();

  // Reed-Switch als Wakeup-Quelle
  // NC-Switch: Klappe zu (Magnet dran) = NC öffnet = HIGH (Pullup)
  //            Klappe auf (Magnet weg) = NC schließt = LOW (GND)
  // → Wakeup auf LOW = Klappe wird geöffnet
  rtc_gpio_init((gpio_num_t)REED_PIN);
  rtc_gpio_set_direction((gpio_num_t)REED_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)REED_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)REED_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)REED_PIN, LOW);

  Serial.println("Gute Nacht! Warte auf nächste Klappe...");
  Serial.flush();
  delay(100);

  esp_deep_sleep_start();
}
