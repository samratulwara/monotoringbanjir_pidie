#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

#define TRIG_PIN 5
#define ECHO_PIN 18
#define LED_MERAH 2
#define LED_KUNING 4
#define LED_HIJAU 16
#define BUZZER_PIN 15

#define JARAK_BAHAYA 6
#define JARAK_SIAGA 12
#define MAX_HISTORY 20

// ── Konfigurasi WiFi ───────────────────────────────────────
const char *ssid = "OPPO RENO 6 5G";   
const char *password = "27072005";

// ── Konfigurasi Telegram ───────────────────────────────────
const char *botToken = "8889124546:AAGO2dQGqGvHhcSTWQMt2csq1CbUwzn_hlQ";
const char *chatID = "-1003932862329";

// ── NTP ────────────────────────────────────────────────────
const char *ntpServer = "pool.ntp.org";
const long gmtOffset = 25200; // UTC+7 WIB
const int daylightOffset = 0;

// ── Variabel sensor & Waktu non-blocking ───────────────────
long duration;
float distance;
String statusBanjir = "NORMAL";
String prevStatus = "NORMAL";

unsigned long prevSensorMillis = 0;
const long sensorInterval = 1000;

unsigned long prevBuzzerMillis = 0;
bool buzzerState = false;

// ── Struktur histori ───────────────────────────────────────
struct FloodEvent
{
  String tanggal;
  String waktuMulai;
  String waktuSelesai;
  String level;
  float jarakMulai;
  float jarakMaks;
  bool selesai;
  String amanWaktu;
  String amanTanggal;
};

FloodEvent history[MAX_HISTORY];
int historyCount = 0;
int currentEventIdx = -1;

WebServer server(80);

// ── Helper waktu ───────────────────────────────────────────
String getNamaHari(int wday)
{
  const char *hari[] = {"Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu"};
  return String(hari[wday]);
}
String getNamaBulan(int mon)
{
  const char *bulan[] = {"Jan", "Feb", "Mar", "Apr", "Mei", "Jun", "Jul", "Agu", "Sep", "Okt", "Nov", "Des"};
  return String(bulan[mon]);
}
String pad2(int n)
{
  return (n < 10 ? "0" : "") + String(n);
}
String getWaktuStr()
{
  struct tm ti;
  if (!getLocalTime(&ti))
    return "--:--:--";
  return pad2(ti.tm_hour) + ":" + pad2(ti.tm_min) + ":" + pad2(ti.tm_sec);
}
String getTanggalStr()
{
  struct tm ti;
  if (!getLocalTime(&ti))
    return "Tanggal tidak tersedia";
  return getNamaHari(ti.tm_wday) + ", " + String(ti.tm_mday) + " " +
         getNamaBulan(ti.tm_mon) + " " + String(1900 + ti.tm_year);
}

// ── Escape karakter khusus JSON ────────────────────────────
String escapeJson(String s)
{
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "\\r");
  return s;
}

// ── Kirim notifikasi Telegram ──────────────────────────────
void sendTelegram(String pesan)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[Telegram] WiFi tidak terhubung, skip kirim.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(botToken) + "/sendMessage";

  if (!http.begin(client, url))
  {
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

  if (httpCode > 0)
  {
    if (httpCode == 200)
      Serial.println("[Telegram] Pesan berhasil terkirim!");
    else
      Serial.println("[Telegram] Pesan gagal — cek token & chat_id.");
  }
  else
  {
    Serial.println("[Telegram] Error koneksi: " + String(http.errorToString(httpCode)));
  }
  http.end();
}

// ── Manajemen histori ──────────────────────────────────────
void mulaiEvent(String level, float jarak)
{
  if (historyCount < MAX_HISTORY)
  {
    int idx = historyCount;
    history[idx].tanggal = getTanggalStr();
    history[idx].waktuMulai = getWaktuStr();
    history[idx].waktuSelesai = "";
    history[idx].level = level;
    history[idx].jarakMulai = jarak;
    history[idx].jarakMaks = jarak;
    history[idx].selesai = false;
    history[idx].amanWaktu = "";
    history[idx].amanTanggal = "";
    currentEventIdx = idx;
    historyCount++;
  }
  else
  {
    for (int i = 0; i < MAX_HISTORY - 1; i++)
      history[i] = history[i + 1];
    int idx = MAX_HISTORY - 1;
    history[idx].tanggal = getTanggalStr();
    history[idx].waktuMulai = getWaktuStr();
    history[idx].waktuSelesai = "";
    history[idx].level = level;
    history[idx].jarakMulai = jarak;
    history[idx].jarakMaks = jarak;
    history[idx].selesai = false;
    history[idx].amanWaktu = "";
    history[idx].amanTanggal = "";
    currentEventIdx = idx;
  }
}

void updateEventMaks(float jarak)
{
  if (currentEventIdx >= 0 && !history[currentEventIdx].selesai)
  {
    if (jarak < history[currentEventIdx].jarakMaks)
      history[currentEventIdx].jarakMaks = jarak;
  }
}

void selesaiEvent()
{
  if (currentEventIdx >= 0 && !history[currentEventIdx].selesai)
  {
    history[currentEventIdx].waktuSelesai = getWaktuStr();
    history[currentEventIdx].amanWaktu = getWaktuStr();
    history[currentEventIdx].amanTanggal = getTanggalStr();
    history[currentEventIdx].selesai = true;
    currentEventIdx = -1;
  }
}

// ── JSON histori ───────────────────────────────────────────
void handleHistory()
{
  String json = "[";
  for (int i = historyCount - 1; i >= 0; i--)
  {
    if (i < historyCount - 1)
      json += ",";
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

// ── HTML Dashboard ─────────────────────────────────────────
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Monitor Banjir Pidie</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; font-family: Arial, sans-serif; }
body { background: #e8f4fd; color: #1a3a5c; padding: 12px; }

.topbar {
  display: flex; justify-content: space-between; align-items: center;
  background: #1a73e8; border-radius: 8px; padding: 10px 16px; margin-bottom: 12px;
}
.topbar-title { font-size: 15px; font-weight: bold; color: #ffffff; }
.topbar-sub { font-size: 11px; color: #c8e0ff; margin-top: 2px; }
.live-dot {
  width: 8px; height: 8px; border-radius: 50%; background: #00e676;
  display: inline-block; animation: blink 1s infinite;
}
@keyframes blink { 0%,100%{opacity:1} 50%{opacity:0.3} }

.status-bar {
  background: #ffffff; border: 2px solid #00c853; border-radius: 8px;
  padding: 12px 16px; margin-bottom: 12px;
  display: flex; justify-content: space-between; align-items: center;
  box-shadow: 0 2px 8px rgba(26,115,232,0.1);
}
.status-label { font-size: 20px; font-weight: bold; color: #00c853; }
.status-sub { font-size: 12px; color: #5a8fc0; margin-top: 2px; }

.metrics-row {
  display: grid; grid-template-columns: repeat(2, 1fr); gap: 12px; margin-bottom: 12px;
}
.m-card {
  background: #ffffff; border: 1px solid #90caf9; border-radius: 8px;
  padding: 10px 14px; box-shadow: 0 2px 6px rgba(26,115,232,0.08);
}
.m-label { font-size: 11px; color: #5a8fc0; margin-bottom: 4px; }
.m-val { font-size: 28px; font-weight: bold; color: #1a73e8; }
.m-unit { font-size: 12px; color: #5a8fc0; }

.led-row { display: flex; gap: 12px; margin-bottom: 12px; }
.led-card {
  flex: 1; background: #ffffff; border: 1px solid #90caf9; border-radius: 8px;
  padding: 10px; text-align: center; box-shadow: 0 2px 6px rgba(26,115,232,0.08);
}
.led-circle {
  width: 32px; height: 32px; border-radius: 50%; margin: 0 auto 6px; opacity: 0.3;
}
.led-circle.on { opacity: 1; box-shadow: 0 0 12px 4px; }
.led-merah { background: #e53935; }
.led-kuning { background: #f9a825; }
.led-hijau { background: #00c853; }
.led-label { font-size: 11px; color: #5a8fc0; }

.card {
  background: #ffffff; border: 1px solid #90caf9; border-radius: 8px;
  padding: 12px; margin-bottom: 12px; box-shadow: 0 2px 6px rgba(26,115,232,0.08);
}
.card-title {
  font-size: 12px; color: #1a73e8; font-weight: bold; margin-bottom: 10px;
  border-bottom: 1px solid #bbdefb; padding-bottom: 6px;
  display: flex; justify-content: space-between; align-items: center;
}
.btn {
  padding: 8px 14px; border-radius: 6px; border: none; font-size: 12px;
  cursor: pointer; background: #1a73e8; color: #ffffff; margin-top: 8px;
}
.btn:hover { background: #1558b0; }

.log-item { padding: 5px 0; border-bottom: 1px solid #bbdefb; color: #1a3a5c; font-size: 12px; }
.log-time { color: #5a8fc0; margin-right: 8px; }
.history-empty { text-align: center; color: #90caf9; font-size: 12px; padding: 16px 0; }

.hist-item {
  border: 1px solid #bbdefb; border-radius: 6px; padding: 8px 10px;
  margin-bottom: 8px; background: #f7fbff; position: relative; overflow: hidden;
}
.hist-item::before {
  content: ''; position: absolute; left: 0; top: 0; bottom: 0; width: 4px;
}
.hist-item.bahaya::before { background: #e53935; }
.hist-item.siaga::before { background: #f9a825; }
.hist-item.aman::before { background: #00c853; }
.hist-item.berlangsung { animation: pulse-border 1.5s infinite; }
@keyframes pulse-border { 0%,100%{opacity:1} 50%{opacity:0.6} }

.hist-badge {
  display: inline-block; font-size: 10px; font-weight: bold;
  padding: 2px 7px; border-radius: 10px; margin-bottom: 5px;
}
.hist-badge.bahaya { background: #fdecea; color: #e53935; }
.hist-badge.siaga { background: #fff8e1; color: #f9a825; }
.hist-badge.aman-badge { background: #e8f5e9; color: #00c853; }
.hist-badge.aktif { background: #e8f5e9; color: #00c853; margin-left: 4px; }

.hist-tanggal { font-size: 12px; font-weight: bold; color: #1a3a5c; margin-bottom: 3px; }
.hist-detail { font-size: 11px; color: #5a8fc0; }
.hist-detail span { color: #1a3a5c; font-weight: 600; }
.hist-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 2px 12px; margin-top: 4px; }

.badge-count {
  background: #e53935; color: white; border-radius: 10px;
  font-size: 10px; padding: 1px 6px; font-weight: bold;
}
</style>
</head>
<body>

<div class="topbar">
  <div>
    <div class="topbar-title">Sistem Peringatan Dini Banjir</div>
    <div class="topbar-sub">ESP32 · HC-SR04 · IoT Dashboard</div>
  </div>
  <div style="text-align:right">
    <span class="live-dot"></span>
    <span style="font-size:12px; color:#ffffff; margin-left:4px;">LIVE</span>
    <div style="font-size:11px; color:#c8e0ff; margin-top:2px;" id="clock">--:--:--</div>
  </div>
</div>

<div class="status-bar" id="status-bar">
  <div>
    <div class="status-label" id="status-label">Memuat...</div>
    <div class="status-sub" id="status-sub">Menghubungkan ke sensor...</div>
  </div>
  <div style="font-size:13px; font-weight:bold;" id="status-badge">-</div>
</div>

<div class="metrics-row">
  <div class="m-card">
    <div class="m-label">Ketinggian Air</div>
    <div><span class="m-val" id="jarak">--</span> <span class="m-unit">cm</span></div>
  </div>
  <div class="m-card">
    <div class="m-label">Status Sensor</div>
    <div><span class="m-val" id="sensor-status" style="font-size:16px; margin-top:6px; display:block;">--</span></div>
  </div>
</div>

<div class="led-row">
  <div class="led-card">
    <div class="led-circle led-merah" id="led-merah"></div>
    <div class="led-label">LED Merah<br>BAHAYA</div>
  </div>
  <div class="led-card">
    <div class="led-circle led-kuning" id="led-kuning"></div>
    <div class="led-label">LED Kuning<br>SIAGA</div>
  </div>
  <div class="led-card">
    <div class="led-circle led-hijau" id="led-hijau"></div>
    <div class="led-label">LED Hijau<br>AMAN</div>
  </div>
</div>

<div class="card">
  <div class="card-title">Log Peringatan</div>
  <div id="log-list"></div>
</div>

<div class="card">
  <div class="card-title">
    <span>&#128203; Histori Kejadian Banjir</span>
    <span id="hist-count-badge" style="display:none;" class="badge-count">0</span>
  </div>
  <div id="history-list"><div class="history-empty">Belum ada kejadian banjir tercatat.</div></div>
</div>

<button class="btn" onclick="fetchData()" style="width:100%; margin-bottom:12px;">Refresh Manual</button>

<script>
function pad(n) { return String(n).padStart(2, '0'); }
function nowStr() {
  var d = new Date();
  return pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
}
setInterval(function() { document.getElementById('clock').textContent = nowStr(); }, 1000);

function addLog(msg) {
  var list = document.getElementById('log-list');
  var div = document.createElement('div');
  div.className = 'log-item';
  div.innerHTML = '<span class="log-time">' + nowStr().slice(0, 5) + '</span>' + msg;
  list.insertBefore(div, list.firstChild);
  if (list.children.length > 6) list.removeChild(list.lastChild);
}

function updateLED(status) {
  document.getElementById('led-merah').classList.toggle('on', status === 'BAHAYA');
  document.getElementById('led-kuning').classList.toggle('on', status === 'SIAGA');
  document.getElementById('led-hijau').classList.toggle('on', status === 'NORMAL');
}

function updateStatus(jarak, status) {
  var label = document.getElementById('status-label');
  var sub = document.getElementById('status-sub');
  var badge = document.getElementById('status-badge');
  var bar = document.getElementById('status-bar');

  if (status === 'BAHAYA') {
    label.style.color = '#e53935'; label.textContent = 'BAHAYA';
    sub.textContent = 'Ketinggian air kritis - evakuasi segera!';
    badge.style.color = '#e53935'; badge.textContent = 'BAHAYA';
    bar.style.borderColor = '#e53935';
    addLog('BAHAYA - Jarak: ' + jarak + ' cm');
  } else if (status === 'SIAGA') {
    label.style.color = '#f9a825'; label.textContent = 'SIAGA';
    sub.textContent = 'Ketinggian air mendekati batas - waspada!';
    badge.style.color = '#f9a825'; badge.textContent = 'SIAGA';
    bar.style.borderColor = '#f9a825';
    addLog('SIAGA - Jarak: ' + jarak + ' cm');
  } else {
    label.style.color = '#00c853'; label.textContent = 'NORMAL';
    sub.textContent = 'Ketinggian air dalam batas aman';
    badge.style.color = '#00c853'; badge.textContent = 'AMAN';
    bar.style.borderColor = '#00c853';
  }
  updateLED(status);
}

function renderHistory(data) {
  var container = document.getElementById('history-list');
  var badge = document.getElementById('hist-count-badge');
  if (!data || data.length === 0) {
    container.innerHTML = '<div class="history-empty">Belum ada kejadian banjir tercatat.</div>';
    badge.style.display = 'none'; return;
  }
  badge.style.display = 'inline'; badge.textContent = data.length;
  var html = '';
  for (var i = 0; i < data.length; i++) {
    var item = data[i];
    var lvlClass = item.level === 'BAHAYA' ? 'bahaya' : 'siaga';
    var aktif = !item.selesai;
    var aktifBadge = aktif ? '<span class="hist-badge aktif">BERLANGSUNG</span>' : '';
    var selesaiStr = item.selesai ? item.waktuSelesai : '-';
    var amanInfo = '';
    if (item.selesai && item.amanWaktu) {
      amanInfo = '<div class="hist-detail" style="margin-top:6px;padding-top:6px;border-top:1px dashed #bbdefb;grid-column:1/-1;">Kembali Aman: <span>' + item.amanTanggal + ' pukul ' + item.amanWaktu + '</span></div>';
    }
    html += '<div class="hist-item ' + lvlClass + (aktif ? ' berlangsung' : '') + '">';
    html += '<div><span class="hist-badge ' + lvlClass + '">' + item.level + '</span> ' + aktifBadge + '</div>';
    html += '<div class="hist-tanggal">' + item.tanggal + '</div>';
    html += '<div class="hist-grid">';
    html += '<div class="hist-detail">Mulai: <span>' + item.waktuMulai + '</span></div>';
    html += '<div class="hist-detail">Selesai: <span>' + selesaiStr + '</span></div>';
    html += '<div class="hist-detail">Jarak Awal: <span>' + item.jarakMulai + ' cm</span></div>';
    html += '<div class="hist-detail">Jarak Maks: <span>' + item.jarakMaks + ' cm</span></div>';
    html += amanInfo;
    html += '</div></div>';
  }
  container.innerHTML = html;
}

function fetchData() {
  fetch('/data')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      document.getElementById('jarak').textContent = d.jarak.toFixed(1);
      document.getElementById('sensor-status').textContent = 'Online';
      updateStatus(d.jarak.toFixed(1), d.status);
    })
    .catch(function() {
      document.getElementById('sensor-status').textContent = 'Offline';
      document.getElementById('status-label').textContent = 'Offline';
      document.getElementById('status-sub').textContent = 'Tidak dapat terhubung ke ESP32';
      document.getElementById('status-badge').textContent = '-';
    });
}

function fetchHistory() {
  fetch('/history')
    .then(function(r) { return r.json(); })
    .then(function(data) { renderHistory(data); })
    .catch(function() {});
}

fetchData();
fetchHistory();
setInterval(fetchData, 2000);
setInterval(fetchHistory, 5000);
</script>
</body>
</html>
)rawliteral";

void handleRoot() { server.send(200, "text/html", index_html); }
void handleData()
{
  String json = "{\"jarak\":" + String(distance, 1) + ",\"status\":\"" + statusBanjir + "\"}";
  server.send(200, "application/json", json);
}

// ── Setup ──────────────────────────────────────────────────
void setup()
{
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_MERAH, OUTPUT);
  pinMode(LED_KUNING, OUTPUT);
  pinMode(LED_HIJAU, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_MERAH, LOW);
  digitalWrite(LED_KUNING, LOW);
  digitalWrite(LED_HIJAU, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // Koneksi WiFi
  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan WiFi");
  int wifiRetry = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetry < 20)
  {
    delay(500);
    Serial.print(".");
    wifiRetry++;
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nWiFi terhubung! IP Address: " + WiFi.localIP().toString());
  }
  else
  {
    Serial.println("\nGagal konek WiFi. Restart...");
    ESP.restart();
  }

  // Sinkronisasi NTP
  configTime(gmtOffset, daylightOffset, ntpServer);
  struct tm ti;
  int ntpRetry = 0;
  while (!getLocalTime(&ti) && ntpRetry < 10)
  {
    delay(500);
    ntpRetry++;
  }
  if (ntpRetry < 10)
    Serial.println("Waktu tersinkron: " + getTanggalStr() + " " + getWaktuStr());

  // Web server routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.begin();
  Serial.println("Web server aktif.");
  Serial.println("Buka browser: http://" + WiFi.localIP().toString());
}

// ── Loop ──────────────────────────────────────────────────
void loop()
{
  server.handleClient();

  unsigned long currentMillis = millis();

  // 1. Pembacaan Sensor setiap 1 detik
  if (currentMillis - prevSensorMillis >= sensorInterval)
  {
    prevSensorMillis = currentMillis;

    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    duration = pulseIn(ECHO_PIN, HIGH, 30000);
    distance = (duration * 0.034f) / 2.0f;

    if (duration != 0 && distance <= 400)
    {
      Serial.print("Jarak Air: ");
      Serial.print(distance);
      Serial.print(" cm | Status: ");

      String newStatus;
      if (distance < JARAK_BAHAYA)
        newStatus = "BAHAYA";
      else if (distance < JARAK_SIAGA)
        newStatus = "SIAGA";
      else
        newStatus = "NORMAL";

      Serial.println(newStatus);

      if (newStatus != "NORMAL" && prevStatus == "NORMAL")
      {
        mulaiEvent(newStatus, distance);
        sendTelegram("⚠️ PERINGATAN BANJIR!\nStatus: " + newStatus + "\nJarak: " + String(distance, 1) + " cm\nJam: " + getWaktuStr());
      }
      if (newStatus == "BAHAYA" && prevStatus == "SIAGA")
      {
        selesaiEvent();
        mulaiEvent("BAHAYA", distance);
        sendTelegram("🚨 STATUS NAIK: BAHAYA!\nJarak: " + String(distance, 1) + " cm");
      }
      if (newStatus == "SIAGA" && prevStatus == "BAHAYA")
      {
        selesaiEvent();
        mulaiEvent("SIAGA", distance);
        sendTelegram("⚠️ STATUS TURUN: SIAGA\nJarak: " + String(distance, 1) + " cm");
      }
      if (newStatus != "NORMAL")
        updateEventMaks(distance);
      if (newStatus == "NORMAL" && prevStatus != "NORMAL")
      {
        selesaiEvent();
        sendTelegram("✅ KONDISI KEMBALI AMAN\nJarak: " + String(distance, 1) + " cm");
      }

      prevStatus = newStatus;
      statusBanjir = newStatus;
    }
  }

  // 2. Kontrol LED & Buzzer (Non-Blocking, tanpa tone())
  if (statusBanjir == "BAHAYA")
  {
    digitalWrite(LED_MERAH, HIGH);
    digitalWrite(LED_KUNING, LOW);
    digitalWrite(LED_HIJAU, LOW);

    // Buzzer: bunyi 500ms, mati 500ms
    if (currentMillis - prevBuzzerMillis >= 500)
    {
      prevBuzzerMillis = currentMillis;
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }
  }
  else if (statusBanjir == "SIAGA")
  {
    digitalWrite(LED_MERAH, LOW);
    digitalWrite(LED_KUNING, HIGH);
    digitalWrite(LED_HIJAU, LOW);

    // Buzzer: bunyi 200ms, mati 800ms
    unsigned long interval = buzzerState ? 200 : 800;
    if (currentMillis - prevBuzzerMillis >= interval)
    {
      prevBuzzerMillis = currentMillis;
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }
  }
  else // NORMAL
  {
    digitalWrite(LED_MERAH, LOW);
    digitalWrite(LED_KUNING, LOW);
    digitalWrite(LED_HIJAU, HIGH);
    digitalWrite(BUZZER_PIN, LOW);
  }
}