/*
 * ============================================
 *   Briefkasten Gateway v1.0
 *   Board: Heltec WiFi LoRa 32 V3 (ESP32-S3, SX1262)
 * ============================================
 * 
 * Funktion:
 *   - Läuft dauerhaft an USB-Strom (kein Sleep)
 *   - Empfängt LoRa-Nachrichten vom T-Beam im Briefkasten
 *   - Sendet Push-Nachricht über ntfy.sh
 *   - Anti-Spam: max 1 Push pro 30 Sekunden
 * 
 * Hardware:
 *   - Heltec WiFi LoRa 32 V3 (oder kompatibler Clone)
 *   - ESP32-S3 + SX1262 LoRa + OLED 0.96"
 *   - LoRa-Antenne MUSS angeschlossen sein!
 * 
 * Libraries:
 *   - RadioLib (Suche: "RadioLib" von Jan Gromeš)
 */

#include <RadioLib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>

// ==========================================
//  KONFIGURATION - HIER ANPASSEN!
// ==========================================
const char* WIFI_SSID     = "DEIN_WLAN_NAME";
const char* WIFI_PASSWORD = "DEIN_WLAN_PASSWORT";  // <-- HIER EINTRAGEN

const char* NTFY_TOPIC  = "DEIN_GEHEIMER_TOPIC_NAME";  // <-- selbst ausdenken, z.B. langer Zufallsstring
const char* NTFY_SERVER = "https://ntfy.sh/";

// ==========================================
//  Pin-Belegung Heltec WiFi LoRa 32 V3
// ==========================================

// LoRa SX1262
#define LORA_SCK    9
#define LORA_MISO   11
#define LORA_MOSI   10
#define LORA_CS     8
#define LORA_RST    12
#define LORA_DIO1   14
#define LORA_BUSY   13

// ==========================================
//  LoRa Parameter (IDENTISCH zum T-Beam!)
// ==========================================
#define LORA_FREQ       868.0   // MHz
#define LORA_BW         125.0   // kHz
#define LORA_SF         12      // Spreading Factor
#define LORA_CR         5       // Coding Rate 4/5
#define LORA_SYNC       0x12    // Sync Word
#define LORA_POWER      17      // dBm
#define LORA_PREAMBLE   8       // Preamble Length

// Anti-Spam: Mindestens 30 Sekunden zwischen Pushes
#define MIN_PUSH_INTERVAL_MS 30000

// ==========================================
//  Globale Objekte
// ==========================================
SPIClass loraSPI(FSPI);
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, loraSPI);

unsigned long lastPushTime = 0;
int totalReceived = 0;
int totalPushed = 0;

// ==========================================
//  Setup
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("========================================");
  Serial.println("  Briefkasten Gateway v1.0");
  Serial.println("  Heltec LoRa 32 V3 + SX1262");
  Serial.println("========================================");

  // --- WiFi verbinden ---
  connectWiFi();

  // --- LoRa initialisieren ---
  loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

  Serial.println("LoRa initialisieren (SX1262)...");
  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_POWER, LORA_PREAMBLE);

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("LoRa FEHLER! Code: %d\n", state);
    Serial.println("Prüfe:");
    Serial.println("  - Antenne angeschlossen?");
    Serial.println("  - SPI Pins korrekt?");
    while (true) {
      delay(1000);  // Hängenbleiben bei Fehler
    }
  }

  Serial.println("LoRa bereit!");
  Serial.println();
  Serial.println("Warte auf Nachrichten vom Briefkasten...");
  Serial.println("========================================");
}

// ==========================================
//  Loop - dauerhaft LoRa empfangen
// ==========================================
void loop() {
  String msg;
  int state = radio.receive(msg);

  if (state == RADIOLIB_ERR_NONE) {
    // --- Nachricht empfangen! ---
    totalReceived++;
    float rssi = radio.getRSSI();
    float snr = radio.getSNR();

    Serial.println();
    Serial.printf(">>> LoRa empfangen #%d <<<\n", totalReceived);
    Serial.printf("  Inhalt: \"%s\"\n", msg.c_str());
    Serial.printf("  RSSI:   %.1f dBm\n", rssi);
    Serial.printf("  SNR:    %.1f dB\n", snr);

    // Nur MAIL-Nachrichten verarbeiten
    if (msg.startsWith("MAIL|")) {
      handleMailMessage(msg, rssi, snr);
    } else {
      Serial.println("  Unbekanntes Format, ignoriert.");
    }

  } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
    // Kein Paket empfangen, normal — einfach weiter warten
    // (kein Print, sonst wird die Konsole geflutet)

  } else {
    // Anderer Fehler
    Serial.printf("LoRa Empfangsfehler: %d\n", state);
  }

  // WiFi-Verbindung prüfen (alle paar Sekunden)
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 30000) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi verloren, verbinde neu...");
      connectWiFi();
    }
    lastWifiCheck = millis();
  }
}

// ==========================================
//  MAIL-Nachricht verarbeiten
// ==========================================
void handleMailMessage(String msg, float rssi, float snr) {
  // Anti-Spam Prüfung
  if (lastPushTime > 0 && (millis() - lastPushTime) < MIN_PUSH_INTERVAL_MS) {
    unsigned long remaining = (MIN_PUSH_INTERVAL_MS - (millis() - lastPushTime)) / 1000;
    Serial.printf("  Anti-Spam: noch %lu Sek. warten, ignoriert.\n", remaining);
    return;
  }

  // Format parsen: MAIL|öffnungsnr|akkuprozent
  int sep1 = msg.indexOf('|');
  int sep2 = msg.indexOf('|', sep1 + 1);

  String count = "?";
  String battery = "?";

  if (sep1 > 0 && sep2 > sep1) {
    count = msg.substring(sep1 + 1, sep2);
    battery = msg.substring(sep2 + 1);
  }

  // Push-Nachricht zusammenbauen
  String pushMsg = "📬 Post da!\n\n";
  pushMsg += "🔋 T-Beam Akku: " + battery + "%\n";
  pushMsg += "🔢 Öffnung Nr. " + count + "\n";
  pushMsg += "📡 LoRa: " + String(rssi, 0) + " dBm / SNR " + String(snr, 1) + " dB";

  Serial.println("  Push senden...");

  if (sendPush(pushMsg)) {
    totalPushed++;
    lastPushTime = millis();
    Serial.printf("  Push gesendet! (gesamt: %d)\n", totalPushed);
  } else {
    Serial.println("  Push FEHLGESCHLAGEN!");
  }
}

// ==========================================
//  Push via ntfy.sh senden
// ==========================================
bool sendPush(String message) {
  // WiFi prüfen
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("  WiFi nicht verbunden, versuche...");
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      return false;
    }
  }

  HTTPClient http;
  String url = String(NTFY_SERVER) + NTFY_TOPIC;
  http.begin(url);
  http.addHeader("Title", "Briefkasten");
  http.addHeader("Tags", "mailbox_with_mail");
  http.addHeader("Priority", "default");

  // Bis zu 3 Versuche
  for (int i = 1; i <= 3; i++) {
    int httpCode = http.POST(message);
    Serial.printf("  HTTP Versuch %d: %d\n", i, httpCode);
    if (httpCode == 200) {
      http.end();
      return true;
    }
    delay(1000);
  }

  http.end();
  return false;
}

// ==========================================
//  WiFi verbinden
// ==========================================
void connectWiFi() {
  Serial.printf("WiFi verbinden mit '%s'... ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(250);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("OK! IP: %s, Signal: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("FEHLGESCHLAGEN!");
  }
}
