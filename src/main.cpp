#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

#define TRIG_PIN    5
#define ECHO_PIN    18
#define LED_MERAH   2
#define LED_KUNING  4
#define LED_HIJAU   16
#define BUZZER_PIN  15

#define JARAK_BAHAYA  30
#define JARAK_SIAGA   60
#define MAX_HISTORY   20

// ── Konfigurasi WiFi ───────────────────────────────────────
const char* ssid     = "Wokwi-GUEST";
const char* password = "";

// ── Konfigurasi Telegram ───────────────────────────────────
const char* botToken = "8889124546:AAGO2dQGqGvHhcSTWQMt2csq1CbUwzn_hlQ";
const char* chatID   = "-1003932862329"; 

// ── NTP ────────────────────────────────────────────────────
const char* ntpServer      = "pool.ntp.org";
const long  gmtOffset      = 25200; // UTC+7 WIB
const int   daylightOffset = 0;

// ── Variabel sensor & Waktu non-blocking ───────────────────
long  duration;
float  distance;
String statusBanjir = "NORMAL";
String prevStatus   = "NORMAL";

unsigned long prevSensorMillis = 0;
const long sensorInterval = 1000; // Baca sensor setiap 1 detik

unsigned long prevBuzzerMillis = 0;
bool buzzerState = false;

// ── Struktur histori ───────────────────────────────────────
struct FloodEvent {
  String tanggal;
  String waktuMulai;
  String waktuSelesai;
  String level;
  float  jarakMulai;
  float  jarakMaks;
  bool   selesai;
  String amanWaktu;
  String amanTanggal;
};

FloodEvent history[MAX_HISTORY];
int historyCount    = 0;
int currentEventIdx = -1;

WebServer server(80);

// ── Helper waktu ───────────────────────────────────────────
String getNamaHari(int wday) {
  const char* hari[] = {"Minggu","Senin","Selasa","Rabu","Kamis","Jumat","Sabtu"};
  return String(hari[wday]);
}
String getNamaBulan(int mon) {
  const char* bulan[] = {"Jan","Feb","Mar","Apr","Mei","Jun","Jul","Agu","Sep","Okt","Nov","Des"};
  return String(bulan[mon]);
}
String pad2(int n) {
  return (n < 10 ? "0" : "") + String(n);
}
String getWaktuStr() {
  struct tm ti;
  if (!getLocalTime(&ti)) return "--:--:--";
  return pad2(ti.tm_hour) + ":" + pad2(ti.tm_min) + ":" + pad2(ti.tm_sec);
}
String getTanggalStr() {
  struct tm ti;
  if (!getLocalTime(&ti)) return "Tanggal tidak tersedia";
  return getNamaHari(ti.tm_wday) + ", " + String(ti.tm_mday) + " " +
         getNamaBulan(ti.tm_mon) + " " + String(1900 + ti.tm_year);
}

// ── Escape karakter khusus JSON ────────────────────────────
String escapeJson(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "\\r");
  return s;
}

// ── Kirim notifikasi Telegram (POST + JSON) ────────────────
void sendTelegram(String pesan) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Telegram] WiFi tidak terhubung, skip kirim.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); 

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(botToken) + "/sendMessage";

  if (!http.begin(client, url)) {
    Serial.println("[Telegram] Gagal membuka koneksi.");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  String body = "{";
  body += "\"chat_id\":\"" + String(chatID) + "\",";
  body += "\"text\":\"" + escapeJson(pesan) + "\",";
  body += "\"parse_mode\":\"\"";
  body += "}";

  int httpCode = http.POST(body);
  Serial.print("[Telegram] HTTP Code: ");
  Serial.println(httpCode);

  if (httpCode > 0) {
    String resp = http.getString();
    if (httpCode == 200) {
      Serial.println("[Telegram] Pesan berhasil terkirim!");
    } else {
      Serial.println("[Telegram] Pesan gagal — cek token & chat_id.");
    }
  } else {
    Serial.println("[Telegram] Error koneksi: " + String(http.errorToString(httpCode)));
  }
  http.end();
}

// ── Manajemen histori ──────────────────────────────────────
void mulaiEvent(String level, float jarak) {
  if (historyCount < MAX_HISTORY) {
    int idx = historyCount;
    history[idx].tanggal      = getTanggalStr();
    history[idx].waktuMulai   = getWaktuStr();
    history[idx].waktuSelesai = "";
    history[idx].level        = level;
    history[idx].jarakMulai   = jarak;
    history[idx].jarakMaks    = jarak;
    history[idx].selesai      = false;
    history[idx].amanWaktu    = "";
    history[idx].amanTanggal  = "";
    currentEventIdx = idx;
    historyCount++;
  } else {
    for (int i = 0; i < MAX_HISTORY - 1; i++) history[i] = history[i + 1];
    int idx = MAX_HISTORY - 1;
    history[idx].tanggal      = getTanggalStr();
    history[idx].waktuMulai   = getWaktuStr();
    history[idx].waktuSelesai = "";
    history[idx].level        = level;
    history[idx].jarakMulai   = jarak;
    history[idx].jarakMaks    = jarak;
    history[idx].selesai      = false;
    history[idx].amanWaktu    = "";
    history[idx].amanTanggal  = "";
    currentEventIdx = idx;
  }
}

void updateEventMaks(float jarak) {
  if (currentEventIdx >= 0 && !history[currentEventIdx].selesai) {
    if (jarak < history[currentEventIdx].jarakMaks)
      history[currentEventIdx].jarakMaks = jarak;
  }
}

void selesaiEvent() {
  if (currentEventIdx >= 0 && !history[currentEventIdx].selesai) {
    history[currentEventIdx].waktuSelesai = getWaktuStr();
    history[currentEventIdx].amanWaktu    = getWaktuStr();
    history[currentEventIdx].amanTanggal  = getTanggalStr();
    history[currentEventIdx].selesai      = true;
    currentEventIdx = -1;
  }
}

// ── JSON histori untuk /history ────────────────────────────
void handleHistory() {
  String json = "[";
  for (int i = historyCount - 1; i >= 0; i--) {
    if (i < historyCount - 1) json += ",";
    json += "{";
    json += "\"tanggal\":\"" + history[i].tanggal + "\",";
    json += "\"waktuMulai\":\"" + history[i].waktuMulai + "\",";
    json += "\"waktuSelesai\":\"" + (history[i].selesai ? history[i].waktuSelesai : "Berlangsung") + "\",";
    json += "\"level\":\"" + history[i].level + "\",";
    json += "\"jarakMulai\":" + String(history[i].jarakMulai, 1) + ",";
    json += "\"jarakMaks\":" + String(history[i].jarakMaks, 1) + ",";
    json += "\"selesai\":" + String(history[i].selesai ? "true" : "false") + ",";
    json += "\"amanWaktu\":\"" + history[i].amanWaktu + "\",";
    json += "\"amanTanggal\":\"" + history[i].amanTanggal + "\"";
    json += "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// HTML Dashboard dimasukkan melalui PROGMEM eksternal dari baris asli Anda
const char index_html[] PROGMEM = R"rawliteral(
)rawliteral"; 
// Catatan: masukkan kembali kode HTML lengkap Anda di bagian PROGMEM ini saat kompilasi.

void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleData() {
  String json = "{\"jarak\":" + String(distance, 1) + ",\"status\":\"" + statusBanjir + "\"}";
  server.send(200, "application/json", json);
}

// ── Setup ──────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN,   OUTPUT);
  pinMode(ECHO_PIN,   INPUT);
  pinMode(LED_MERAH,  OUTPUT);
  pinMode(LED_KUNING, OUTPUT);
  pinMode(LED_HIJAU,  OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  digitalWrite(LED_MERAH,  LOW);
  digitalWrite(LED_KUNING, LOW);
  digitalWrite(LED_HIJAU,  LOW);

  // Koneksi WiFi
  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan WiFi");
  int wifiRetry = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetry < 20) {
    delay(500);
    Serial.print(".");
    wifiRetry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi terhubung! IP Address: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nGagal konek WiFi. Restart...");
    ESP.restart();
  }

  // Sinkronisasi NTP
  configTime(gmtOffset, daylightOffset, ntpServer);
  struct tm ti;
  int ntpRetry = 0;
  while (!getLocalTime(&ti) && ntpRetry < 10) {
    delay(500);
    Serial.print(".");
    ntpRetry++;
  }
  if (ntpRetry < 10) {
    Serial.println("\nWaktu tersinkron: " + getTanggalStr() + " " + getWaktuStr());
  }

  // Web server routes
  server.on("/",        handleRoot);
  server.on("/data",    handleData);
  server.on("/history", handleHistory);
  server.begin();
  Serial.println("Web server aktif.");
}

// ── Loop ───────────────────────────────────────────────────
void loop() {
  // Selalu jalankan client web server di setiap iterasi tanpa interupsi
  server.handleClient();

  unsigned long currentMillis = millis();

  // 1. Pembacaan Sensor secara Periodik (Setiap 1 Detik)
  if (currentMillis - prevSensorMillis >= sensorInterval) {
    prevSensorMillis = currentMillis;

    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    duration = pulseIn(ECHO_PIN, HIGH, 30000); 
    distance = (duration * 0.034f) / 2.0f;

    if (duration != 0 && distance <= 400) {
      Serial.print("Jarak Air: ");
      Serial.print(distance);
      Serial.print(" cm | Status: ");

      String newStatus;
      if (distance < JARAK_BAHAYA) {
        newStatus = "BAHAYA";
      } else if (distance < JARAK_SIAGA) {
        newStatus = "SIAGA";
      } else {
        newStatus = "NORMAL";
      }
      Serial.println(newStatus);

      // Logika Transisi Status & Notifikasi Telegram
      if (newStatus != "NORMAL" && prevStatus == "NORMAL") {
        mulaiEvent(newStatus, distance);
        sendTelegram("⚠️ PERINGATAN BANJIR!\nStatus: " + newStatus + "\nJarak: " + String(distance, 1) + " cm\nJam: " + getWaktuStr());
      }
      if (newStatus == "BAHAYA" && prevStatus == "SIAGA") {
        selesaiEvent(); mulaiEvent("BAHAYA", distance);
        sendTelegram("🚨 STATUS NAIK: BAHAYA!\nJarak: " + String(distance, 1) + " cm");
      }
      if (newStatus == "SIAGA" && prevStatus == "BAHAYA") {
        selesaiEvent(); mulaiEvent("SIAGA", distance);
        sendTelegram("⚠️ STATUS TURUN: SIAGA\nJarak: " + String(distance, 1) + " cm");
      }
      if (newStatus != "NORMAL") {
        updateEventMaks(distance);
      }
      if (newStatus == "NORMAL" && prevStatus != "NORMAL") {
        selesaiEvent();
        sendTelegram("✅ KONDISI KEMBALI AMAN\nJarak: " + String(distance, 1) + " cm");
      }

      prevStatus   = newStatus;
      statusBanjir = newStatus;
    }
  }

  // 2. Kontrol LED Statis & Kedipan Buzzer (Non-Blocking)
  if (statusBanjir == "BAHAYA") {
    digitalWrite(LED_MERAH, HIGH);
    digitalWrite(LED_KUNING, LOW);
    digitalWrite(LED_HIJAU, LOW);

    // Pola Bahaya: Bunyi 500ms, Mati 500ms
    if (currentMillis - prevBuzzerMillis >= 500) {
      prevBuzzerMillis = currentMillis;
      buzzerState = !buzzerState;
      if (buzzerState) tone(BUZZER_PIN, 2000);
      else noTone(BUZZER_PIN);
    }
  } 
  else if (statusBanjir == "SIAGA") {
    digitalWrite(LED_MERAH, LOW);
    digitalWrite(LED_KUNING, HIGH);
    digitalWrite(LED_HIJAU, LOW);

    // Pola Siaga: Bunyi 200ms, Mati 800ms
    unsigned long interval = buzzerState ? 200 : 800;
    if (currentMillis - prevBuzzerMillis >= interval) {
      prevBuzzerMillis = currentMillis;
      buzzerState = !buzzerState;
      if (buzzerState) tone(BUZZER_PIN, 1000);
      else noTone(BUZZER_PIN);
    }
  } 
  else { // NORMAL
    digitalWrite(LED_MERAH, LOW);
    digitalWrite(LED_KUNING, LOW);
    digitalWrite(LED_HIJAU, HIGH);
    noTone(BUZZER_PIN);
  }
}