
/*
 * PENJEMUR PINTAR ESP32-S3 + HIVE MQ CLOUD
 *
 * Sensor:
 * - DHT22 : GPIO 4
 * - FC-37 : GPIO 5
 *
 * Motor driver:
 * - ENA/PWM : GPIO 15
 * - IN1     : GPIO 16
 * - IN2     : GPIO 17
 *
 * Alur data:
 * ESP32-S3 -> HiveMQ Cloud -> Node.js -> Supabase
 *
 * Library:
 * - PubSubClient
 * - DHT sensor library (Adafruit)
 * - Adafruit Unified Sensor
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHT.h>

// ================ PIN ESP32-S3 ================

#define PIN_DHT 4
#define DHTTYPE DHT22

#define PIN_FC37 5

#define ENA 15
#define IN1 16
#define IN2 17

// ================ WIFI ================

const char* ssid = "Punyasaya";
const char* password = "Tanyasaya";

// ================ HIVEMQ CLOUD ================

const char* mqtt_server =
  "d8e105acbc644d4bb8aeca9ecbc2337e.s1.eu.hivemq.cloud";

const int mqtt_port = 8883;

const char* mqtt_user = "Kay";
const char* mqtt_password = "0987654321";

const char* client_id = "ESP32-001";
const char* device_id = "ESP32-001";

const char* topic_sensor =
  "iot/TRSE2026/ESP32-001/sensor";

const char* topic_status =
  "iot/TRSE2026/ESP32-001/status";

// ================ OBJEK SENSOR/MQTT ================

DHT dht(PIN_DHT, DHTTYPE);

WiFiClientSecure secureClient;
PubSubClient client(secureClient);

// ================ KONFIGURASI SISTEM ================

const int KECEPATAN_MOTOR = 200;  // PWM 0-255
const unsigned long WAKTU_TARIK = 3000;

const unsigned long INTERVAL_DHT = 2000;
const unsigned long INTERVAL_MQTT = 5000;
const unsigned long INTERVAL_RECONNECT = 5000;

// FC-37 umumnya LOW saat basah.
// Ubah menjadi HIGH jika modulmu bekerja sebaliknya.
const int KONDISI_HUJAN = LOW;

// ================ STATUS SISTEM ================

bool hujanSebelumnya = false;
bool jemuranSudahDitarik = false;
bool motorBerjalan = false;

bool dataDHTValid = false;

float suhu = NAN;
float kelembapan = NAN;

unsigned long waktuMulaiMotor = 0;
unsigned long waktuBacaDHT = 0;
unsigned long waktuPublish = 0;
unsigned long waktuReconnect = 0;

// ================ KONEKSI WIFI ================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Menghubungkan ke WiFi...");
}

// ================ MOTOR ================

void motorBerhenti() {
  ledcWrite(ENA, 0);

  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);

  motorBerjalan = false;
}

void tarikJemuran() {
  Serial.println("HUJAN TERDETEKSI!");
  Serial.println("Menarik jemuran ke dalam...");

  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);

  ledcWrite(ENA, KECEPATAN_MOTOR);

  waktuMulaiMotor = millis();
  motorBerjalan = true;
  jemuranSudahDitarik = true;
}

// ================ STATUS MQTT ================

void publishStatus(const char* status) {
  char payload[100];

  snprintf(
    payload,
    sizeof(payload),
    "{\"device_id\":\"%s\",\"status\":\"%s\"}",
    device_id,
    status
  );

  if (client.connected()) {
    client.publish(topic_status, payload, true);
  }
}

// ================ RECONNECT MQTT ================

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (client.connected()) {
    return;
  }

  unsigned long sekarang = millis();

  if (sekarang - waktuReconnect < INTERVAL_RECONNECT) {
    return;
  }

  waktuReconnect = sekarang;

  Serial.print("Menghubungkan ke HiveMQ...");

  char willMsg[100];

  snprintf(
    willMsg,
    sizeof(willMsg),
    "{\"device_id\":\"%s\",\"status\":\"offline\"}",
    device_id
  );

  if (client.connect(
        client_id,
        mqtt_user,
        mqtt_password,
        topic_status,
        1,
        true,
        willMsg
      )) {

    Serial.println(" berhasil!");

    publishStatus("online");

  } else {
    Serial.print(" gagal. Kode: ");
    Serial.println(client.state());
  }
}

// ================ BACA DHT22 ================

void bacaDHT() {
  unsigned long sekarang = millis();

  if (sekarang - waktuBacaDHT < INTERVAL_DHT) {
    return;
  }

  waktuBacaDHT = sekarang;

  float suhuBaru = dht.readTemperature();
  float kelembapanBaru = dht.readHumidity();

  if (isnan(suhuBaru) || isnan(kelembapanBaru)) {
    dataDHTValid = false;
    Serial.println("Gagal membaca DHT22!");
    return;
  }

  suhu = suhuBaru;
  kelembapan = kelembapanBaru;
  dataDHTValid = true;

  Serial.print("Suhu       : ");
  Serial.print(suhu, 2);
  Serial.println(" C");

  Serial.print("Kelembapan : ");
  Serial.print(kelembapan, 2);
  Serial.println(" %");
}

// ================ BACA SENSOR HUJAN ================

void bacaHujan() {
  int nilaiFC37 = digitalRead(PIN_FC37);
  bool hujan = (nilaiFC37 == KONDISI_HUJAN);

  if (hujan && !hujanSebelumnya) {
    Serial.println("Sensor FC-37 mendeteksi air.");
  }

  if (!hujan && hujanSebelumnya) {
    Serial.println("Kondisi kembali kering.");
    Serial.println("Sistem siap untuk hujan berikutnya.");

    jemuranSudahDitarik = false;
  }

  hujanSebelumnya = hujan;

  if (hujan && !jemuranSudahDitarik) {
    tarikJemuran();
  }
}

// ================ KONTROL DURASI MOTOR ================

void kontrolMotor() {
  unsigned long sekarang = millis();

  if (motorBerjalan &&
      sekarang - waktuMulaiMotor >= WAKTU_TARIK) {

    motorBerhenti();

    Serial.println("Penarikan selesai.");
    Serial.println("Motor berhenti.");
  }
}

// ================ KIRIM DATA KE HIVEMQ ================

void publishSensor() {
  unsigned long sekarang = millis();

  if (sekarang - waktuPublish < INTERVAL_MQTT) {
    return;
  }

  waktuPublish = sekarang;

  // Jangan mengirim data sensor DHT yang tidak valid.
  if (!dataDHTValid) {
    Serial.println("Data DHT belum valid; publish dilewati.");
    return;
  }

  if (!client.connected()) {
    Serial.println("MQTT terputus; data tidak dikirim.");
    return;
  }

  bool hujan =
    (digitalRead(PIN_FC37) == KONDISI_HUJAN);

  char payload[220];

  snprintf(
    payload,
    sizeof(payload),
    "{\"device_id\":\"%s\","
    "\"temperature\":%.2f,"
    "\"humidity\":%.2f,"
    "\"rain\":%s,"
    "\"motor_status\":\"%s\"}",
    device_id,
    suhu,
    kelembapan,
    hujan ? "true" : "false",
    motorBerjalan ? "running" : "stopped"
  );

  bool berhasil = client.publish(
    topic_sensor,
    payload
  );

  if (berhasil) {
    Serial.println("Data berhasil dikirim ke HiveMQ:");
    Serial.println(payload);
  } else {
    Serial.println("Gagal mengirim data MQTT.");
  }
}

// ================ SETUP ================

void setup() {
  Serial.begin(115200);

  dht.begin();

  pinMode(PIN_FC37, INPUT);

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);

  // ESP32 Arduino Core 3.x:
  // ledcAttach(pin, frekuensi, resolusi_bit)
  ledcAttach(ENA, 1000, 8);

  motorBerhenti();

  // Untuk praktikum. Lihat catatan keamanan TLS.
  secureClient.setInsecure();

  client.setServer(mqtt_server, mqtt_port);
  client.setBufferSize(256);

  connectWiFi();

  Serial.println();
  Serial.println("================================");
  Serial.println("     PENJEMUR PINTAR ESP32-S3");
  Serial.println("================================");
  Serial.println("Sistem mulai dijalankan.");
}

// ================ LOOP ================

void loop() {
  // 1. Jaga koneksi WiFi tanpa menunggu dalam while.
  if (WiFi.status() == WL_CONNECTED) {
    reconnectMQTT();
    client.loop();
  }

  // 2. Jalankan otomasi lokal.
  bacaHujan();
  kontrolMotor();

  // 3. Baca DHT22 setiap 2 detik.
  bacaDHT();

  // 4. Kirim data sensor setiap 5 detik.
  publishSensor();
}
