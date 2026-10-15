
require("dotenv").config();

const mqtt = require("mqtt");
const { createClient } = require("@supabase/supabase-js");

// ================ KONFIGURASI ================

const {
  MQTT_HOST,
  MQTT_PORT = "8883",
  MQTT_USERNAME,
  MQTT_PASSWORD,
  TOPIC_SENSOR,
  TOPIC_STATUS,
  DEVICE_ID,
  SUPABASE_URL,
  SUPABASE_SECRET_KEY,
  BLYNK_TOKEN,
  BLYNK_BASE_URL = "https://blynk.cloud",
} = process.env;

const required = {
  MQTT_HOST,
  MQTT_USERNAME,
  MQTT_PASSWORD,
  TOPIC_SENSOR,
  TOPIC_STATUS,
  DEVICE_ID,
  SUPABASE_URL,
  SUPABASE_SECRET_KEY,
  BLYNK_TOKEN,
};

for (const [key, value] of Object.entries(required)) {
  if (!value) {
    throw new Error(`Konfigurasi ${key} belum diisi pada .env`);
  }
}

// ================ SUPABASE ================

const supabase = createClient(
  SUPABASE_URL,
  SUPABASE_SECRET_KEY
);

// ================ STATUS SISTEM ================

let deviceStatus = "OFFLINE";

// ================ BLYNK QUEUE ================
//
// V0 = Status perangkat: 1 online, 0 offline
// V1 = Status motor:     1 menyala, 0 berhenti
// V2 = Temperatur:       nilai numerik °C
// V3 = Humidity:         nilai numerik %
// V4 = Air/hujan:        1 terdeteksi, 0 tidak terdeteksi

const BLYNK_UPDATE_INTERVAL = 1000;
const BLYNK_TIMEOUT = 5000;
const BLYNK_RETRY_DELAY = 5000;

const sleep = (ms) =>
  new Promise((resolve) => setTimeout(resolve, ms));

let latestBlynkValues = null;
let blynkWorkerRunning = false;
let lastBlynkUpdate = 0;

// Memasukkan data terbaru ke antrean.
// Data baru menggantikan nilai pin yang belum dikirim.

function queueBlynkUpdate(values) {
  latestBlynkValues = {
    ...(latestBlynkValues || {}),
    ...values,
  };

  if (!blynkWorkerRunning) {
    void processBlynkQueue();
  }
}

// Mengirim data Blynk satu request dalam satu waktu.

async function processBlynkQueue() {
  if (blynkWorkerRunning) return;

  blynkWorkerRunning = true;

  try {
    while (latestBlynkValues !== null) {
      const waitTime =
        BLYNK_UPDATE_INTERVAL -
        (Date.now() - lastBlynkUpdate);

      if (waitTime > 0) {
        await sleep(waitTime);
      }

      // Ambil data terbaru untuk dikirim.
      const values = latestBlynkValues;
      latestBlynkValues = null;

      const params = new URLSearchParams({
        token: BLYNK_TOKEN,
      });

      for (const [key, value] of Object.entries(values)) {
        params.set(key, String(value));
      }

      const url =
        `${BLYNK_BASE_URL}/external/api/batch/update?${params}`;

      try {
        const response = await fetch(url, {
          method: "GET",
          signal: AbortSignal.timeout(BLYNK_TIMEOUT),
        });

        const result = await response.text();

        if (
          !response.ok ||
          result.toLowerCase().includes("error")
        ) {
          throw new Error(
            `HTTP ${response.status}: ${result}`
          );
        }

        lastBlynkUpdate = Date.now();

        console.log(
          "[Blynk] Update berhasil:",
          values
        );
      } catch (error) {
        console.error(
          "[Blynk] Update gagal:",
          error.message
        );

        // Jika gagal, pertahankan nilai yang gagal dikirim.
        // Nilai yang lebih baru tetap diprioritaskan.
        latestBlynkValues = {
          ...values,
          ...(latestBlynkValues || {}),
        };

        // Hindari percobaan ulang terlalu cepat.
        await sleep(BLYNK_RETRY_DELAY);

        // Jika perangkat dimatikan saat menunggu,
        // proses akan selesai pada iterasi berikutnya
        // setelah antrean dikosongkan oleh shutdown.
      }
    }
  } finally {
    blynkWorkerRunning = false;

    // Tangani data yang masuk menjelang worker berhenti.
    if (latestBlynkValues !== null) {
      void processBlynkQueue();
    }
  }
}

// ================ STATUS PERANGKAT ================

function updateDeviceStatus(status) {
  deviceStatus = status;

  queueBlynkUpdate({
    V0: status === "ONLINE" ? 1 : 0,
  });
}

// ================ DASHBOARD SENSOR ================

function updateSensorDashboard(data) {
  queueBlynkUpdate({
    V0: deviceStatus === "ONLINE" ? 1 : 0,
    V1: data.motor_status === "running" ? 1 : 0,
    V2: data.temperature,
    V3: data.humidity,
    V4: data.rain ? 1 : 0,
  });
}

// ================ SIMPAN KE SUPABASE ================

async function saveToSupabase(data) {
  const row = {
    device_id: data.device_id,
    temperature: data.temperature,
    humidity: data.humidity,
    rain: data.rain,
    motor_status: data.motor_status,
    device_status: deviceStatus,
  };

  const { error } = await supabase
    .from("sensor_logs")
    .insert(row);

  if (error) {
    console.error(
      "[Supabase] Gagal menyimpan:",
      error.message,
      error.code || ""
    );
    return;
  }

  console.log("[Supabase] Data berhasil disimpan.");
}

// ================ VALIDASI SENSOR ================

function validateSensorData(data) {
  return (
    data &&
    data.device_id === DEVICE_ID &&
    typeof data.temperature === "number" &&
    Number.isFinite(data.temperature) &&
    typeof data.humidity === "number" &&
    Number.isFinite(data.humidity) &&
    data.humidity >= 0 &&
    data.humidity <= 100 &&
    typeof data.rain === "boolean" &&
    ["running", "stopped"].includes(data.motor_status)
  );
}

// ================ MQTT HIVEMQ ================

const mqttUrl =
  `mqtts://${MQTT_HOST}:${MQTT_PORT}`;

const client = mqtt.connect(mqttUrl, {
  username: MQTT_USERNAME,
  password: MQTT_PASSWORD,
  clientId: `NodeJS-${DEVICE_ID}-${process.pid}`,
  clean: true,
  reconnectPeriod: 5000,
  connectTimeout: 30000,
});

client.on("connect", () => {
  console.log("[MQTT] Terhubung ke HiveMQ Cloud.");

  client.subscribe(
    [TOPIC_SENSOR, TOPIC_STATUS],
    { qos: 0 },
    (error) => {
      if (error) {
        console.error(
          "[MQTT] Gagal subscribe:",
          error.message
        );
      } else {
        console.log(
          "[MQTT] Subscribe sensor dan status berhasil."
        );
      }
    }
  );
});

client.on("message", (topic, message) => {
  let data;

  try {
    data = JSON.parse(message.toString());
  } catch {
    console.error("[MQTT] Payload bukan JSON valid.");
    return;
  }

  // -------- STATUS PERANGKAT --------

  if (topic === TOPIC_STATUS) {
    if (
      data.device_id !== DEVICE_ID ||
      !["online", "offline"].includes(data.status)
    ) {
      console.warn("[MQTT] Pesan status tidak valid.");
      return;
    }

    const status = data.status.toUpperCase();

    console.log(`[Device] Status: ${status}`);

    // Hanya memasukkan update ke antrean.
    // Tidak menunggu request HTTP Blynk.
    updateDeviceStatus(status);

    return;
  }

  // -------- DATA SENSOR --------

  if (topic !== TOPIC_SENSOR) return;

  if (!validateSensorData(data)) {
    console.warn(
      "[MQTT] Data sensor tidak valid; diabaikan.",
      data
    );
    return;
  }

  console.log("\n===== PENJEMUR PINTAR =====");
  console.log("Device      :", data.device_id);
  console.log("Temperatur  :", data.temperature, "°C");
  console.log("Humidity    :", data.humidity, "%");
  console.log(
    "Air/hujan   :",
    data.rain ? "TERDETEKSI" : "TIDAK TERDETEKSI"
  );
  console.log("Motor       :", data.motor_status);
  console.log("Status alat :", deviceStatus);
  console.log("============================\n");

  // Supabase berjalan independen.
  // Tidak menunggu Blynk selesai.
  void saveToSupabase(data).catch((error) => {
    console.error(
      "[Supabase] Kesalahan:",
      error.message
    );
  });

  // Blynk hanya memasukkan nilai terbaru ke antrean.
  updateSensorDashboard(data);
});

// ================ PENANGANAN KONEKSI ================

client.on("reconnect", () => {
  console.log("[MQTT] Mencoba menghubungkan ulang...");
});

client.on("offline", () => {
  console.log("[MQTT] Koneksi HiveMQ offline.");
});

client.on("error", (error) => {
  console.error("[MQTT] Kesalahan:", error.message);
});

process.on("SIGINT", () => {
  console.log("\nMenutup koneksi MQTT...");

  client.end(false, {}, () => {
    console.log("[MQTT] Koneksi ditutup.");
    process.exit(0);
  });
});
