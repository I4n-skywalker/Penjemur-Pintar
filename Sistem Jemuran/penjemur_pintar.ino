/*
 * PENJEMUR PINTAR ESP32 - versi IoT (UTS)
 * Sensor  : DHT22 (suhu, kelembapan), FC-37 (air/hujan)
 * Aktuator: Motor DC via driver (ENA/IN1/IN2)
 * Cloud   : MQTT (HiveMQ Cloud, TLS 8883) -> bridge Node.js -> Supabase & Blynk
 *
 * Library (Library Manager): DHT sensor library, Adafruit Unified Sensor,
 *                            PubSubClient, ArduinoJson (v7)
 * Board   : ESP32 Arduino Core 3.x (memakai ledcAttach)
 *
 * Tanpa delay() / while() penunggu -> tidak ada blocking pada loop utama.
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include "secrets.h"

// ================= PIN ESP32 =================
#define PIN_DHT   4
#define DHTTYPE   DHT22
#define PIN_FC37  5
#define ENA       15
#define IN1       16
#define IN2       17

// ================= KONFIGURASI =================
const int  KECEPATAN_MOTOR   = 200;      // PWM 0-255
const unsigned long WAKTU_TARIK     = 3000;   // ms motor berputar
const unsigned long INTERVAL_DHT    = 2000;   // ms
const unsigned long INTERVAL_KIRIM  = 5000;   // ms kirim berkala ke cloud
const unsigned long INTERVAL_RECONNECT = 5000; // ms jeda percobaan sambung ulang
const int  KONDISI_HUJAN = LOW;  // FC-37 umumnya LOW saat basah; ubah ke HIGH jika terbalik

// ================= TOPIK MQTT =================
const char* TOPIK_TELEMETRY = "penjemur/" DEVICE_ID "/telemetry";
const char* TOPIK_STATUS    = "penjemur/" DEVICE_ID "/status";
const char* TOPIK_COMMAND   = "penjemur/" DEVICE_ID "/command";

// ================= OBJEK =================
DHT dht(PIN_DHT, DHTTYPE);
WiFiClientSecure netClient;
PubSubClient mqtt(netClient);

// ================= STATUS SISTEM =================
enum Perintah { CMD_NONE, CMD_TARIK, CMD_KELUAR, CMD_STOP };
volatile Perintah perintahMasuk = CMD_NONE;   // diisi terimaDataCloud()

float suhu = NAN, kelembapan = NAN;
bool  hujan = false, hujanSebelumnya = false;
bool  jemuranSudahDitarik = false;
bool  motorBerjalan = false;
const char* arahMotor = "-";     // TARIK / KELUAR / -
const char* sumberMotor = "-";   // AUTO / MANUAL / -
bool  kirimSegera = false;       // paksa kirim saat ada perubahan status

unsigned long waktuMulaiMotor = 0, waktuBacaDHT = 0, waktuKirim = 0;
unsigned long waktuWiFiCoba = 0, waktuMqttCoba = 0;

// =====================================================
// BLOK 1: PEMBACAAN SENSOR
// =====================================================
void bacaSensor(unsigned long sekarang) {
  // FC-37 dibaca setiap loop (cepat, tidak blocking)
  hujan = (digitalRead(PIN_FC37) == KONDISI_HUJAN);

  if (hujan != hujanSebelumnya) {
    Serial.println(hujan ? "Sensor FC-37 mendeteksi air."
                         : "Sensor FC-37: kondisi kering. Siap mendeteksi hujan berikutnya.");
    if (!hujan) jemuranSudahDitarik = false;
    hujanSebelumnya = hujan;
    kirimSegera = true;
  }

  // DHT22 dibaca berkala
  if (sekarang - waktuBacaDHT >= INTERVAL_DHT) {
    waktuBacaDHT = sekarang;
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (isnan(h) || isnan(t)) {
      Serial.println("Gagal membaca sensor DHT22!");
      suhu = NAN; kelembapan = NAN;
    } else {
      suhu = t; kelembapan = h;
      Serial.printf("Suhu %.1f C | Kelembapan %.1f %% | Hujan: %s | Motor: %s\n",
                    suhu, kelembapan, hujan ? "YA" : "TIDAK", motorBerjalan ? "ON" : "OFF");
    }
  }
}

// =====================================================
// BLOK 2: PERINTAH KE AKTUATOR (MOTOR)
// =====================================================
void motorBerhenti() {
  ledcWrite(ENA, 0);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  motorBerjalan = false;
  arahMotor = "-";
  sumberMotor = "-";
}

void motorMulai(bool tarik, const char* sumber) {
  digitalWrite(IN1, tarik ? HIGH : LOW);
  digitalWrite(IN2, tarik ? LOW : HIGH);
  ledcWrite(ENA, KECEPATAN_MOTOR);
  waktuMulaiMotor = millis();
  motorBerjalan = true;
  arahMotor = tarik ? "TARIK" : "KELUAR";
  sumberMotor = sumber;
  Serial.printf("Motor jalan: %s (%s)\n", arahMotor, sumberMotor);
}

void perintahAktuator(unsigned long sekarang) {
  // a) Perintah manual dari cloud (Blynk -> bridge -> MQTT)
  Perintah cmd = perintahMasuk;
  perintahMasuk = CMD_NONE;
  if (cmd == CMD_TARIK)       motorMulai(true,  "MANUAL");
  else if (cmd == CMD_KELUAR) motorMulai(false, "MANUAL");
  else if (cmd == CMD_STOP && motorBerjalan) { motorBerhenti(); Serial.println("Motor dihentikan manual."); }
  if (cmd != CMD_NONE) kirimSegera = true;

  // b) Otomatis: hujan -> tarik jemuran (sekali per kejadian hujan)
  if (hujan && !jemuranSudahDitarik) {
    Serial.println("HUJAN TERDETEKSI! Menarik jemuran ke dalam...");
    jemuranSudahDitarik = true;
    motorMulai(true, "AUTO");
    kirimSegera = true;
  }

  // c) Hentikan motor setelah durasi tercapai
  if (motorBerjalan && sekarang - waktuMulaiMotor >= WAKTU_TARIK) {
    motorBerhenti();
    Serial.println("Penarikan selesai. Motor berhenti.");
    kirimSegera = true;
  }
}

// =====================================================
// BLOK 3: PENGIRIMAN DATA KE CLOUD (MQTT publish)
// =====================================================
void kirimDataCloud(unsigned long sekarang) {
  if (!mqtt.connected()) return;
  if (!kirimSegera && sekarang - waktuKirim < INTERVAL_KIRIM) return;
  waktuKirim = sekarang;
  kirimSegera = false;

  JsonDocument doc;
  if (isnan(suhu)) doc["suhu"] = nullptr;       else doc["suhu"] = serialized(String(suhu, 1));
  if (isnan(kelembapan)) doc["kelembapan"] = nullptr; else doc["kelembapan"] = serialized(String(kelembapan, 1));
  doc["hujan"]  = hujan ? 1 : 0;
  doc["motor"]  = motorBerjalan ? 1 : 0;
  doc["arah"]   = arahMotor;
  doc["sumber"] = sumberMotor;
  doc["rssi"]   = WiFi.RSSI();

  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(TOPIK_TELEMETRY, (const uint8_t*)buf, n, false);
}

// =====================================================
// BLOK 4: PENERIMAAN DATA DARI CLOUD (MQTT callback)
// =====================================================
void terimaDataCloud(char* topik, byte* payload, unsigned int len) {
  String isi;
  for (unsigned int i = 0; i < len; i++) isi += (char)payload[i];
  isi.trim(); isi.toUpperCase();
  Serial.printf("Perintah diterima [%s]: %s\n", topik, isi.c_str());

  if (isi == "TARIK")       perintahMasuk = CMD_TARIK;
  else if (isi == "KELUAR") perintahMasuk = CMD_KELUAR;
  else if (isi == "STOP")   perintahMasuk = CMD_STOP;
}

// =====================================================
// KONEKSI (non-blocking: hanya mencoba tiap INTERVAL_RECONNECT)
// =====================================================
void jagaKoneksi(unsigned long sekarang) {
  if (WiFi.status() != WL_CONNECTED) {
    if (sekarang - waktuWiFiCoba >= INTERVAL_RECONNECT) {
      waktuWiFiCoba = sekarang;
      Serial.println("WiFi terputus, mencoba menyambung ulang...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    return;
  }

  if (!mqtt.connected()) {
    if (sekarang - waktuMqttCoba >= INTERVAL_RECONNECT) {
      waktuMqttCoba = sekarang;
      String cid = String("esp32-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      // Last Will: broker menandai perangkat offline bila koneksi putus mendadak
      if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS, TOPIK_STATUS, 1, true, "offline")) {
        Serial.println("MQTT terhubung.");
        mqtt.publish(TOPIK_STATUS, "online", true);
        mqtt.subscribe(TOPIK_COMMAND, 1);
        kirimSegera = true;
      } else {
        Serial.printf("MQTT gagal, rc=%d\n", mqtt.state());
      }
    }
  } else {
    mqtt.loop();
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(PIN_FC37, INPUT);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  ledcAttach(ENA, 1000, 8);   // PWM ESP32 Arduino Core 3.x
  motorBerhenti();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  netClient.setInsecure();    // TLS tanpa verifikasi sertifikat (prototipe). Produksi: setCACert(ISRG Root X1)
  netClient.setTimeout(5);    // detik, agar connect tidak menahan loop terlalu lama
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(terimaDataCloud);
  mqtt.setSocketTimeout(5);
  mqtt.setBufferSize(512);

  Serial.println("\n===== PENJEMUR PINTAR ESP32 =====\nSistem siap dijalankan.");
}

// ================= LOOP =================
void loop() {
  unsigned long sekarang = millis();
  jagaKoneksi(sekarang);
  bacaSensor(sekarang);
  perintahAktuator(sekarang);
  kirimDataCloud(sekarang);
}
