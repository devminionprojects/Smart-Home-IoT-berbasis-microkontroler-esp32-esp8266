// ================================================================
//  SMART PANEL PLTS - HLM25
//  Relay 1 : INVERTER    [Manual | PZEM 0V→ON, PLN>=50V→OFF]
//  Relay 2 : LAMPU UTAMA [Manual | PZEM 0V→ON, PLN>=50V→OFF]
//  Relay 3 : OUTPUT AC   [Manual | Timer aktif → ON]
//  Relay 4 : LAMPU PANEL [Manual | PZEM 0V→ON, PLN>=50V→OFF]
//
//  VERSI OPEN SOURCE -- data pribadi sudah dikosongkan.
//  WAJIB ISI sebelum upload (cari tanda "ISI"):
//    1. ssid, pass                    -> WiFi rumah
//    2. ap_pass                       -> password hotspot ESP32 (min 8 karakter)
//    3. STA_LOCAL_IP/GATEWAY/SUBNET/DNS -> sesuaikan dengan jaringanmu
//    4. LOGIN_PASSWORD, CP_PASSWORD   -> password admin (kosong = login ditolak)
//    5. TUNNEL_URL_SOURCE (opsional)  -> hanya jika diakses dari internet via tunnel
//  Penjelasan lengkap ada di README.md
// ================================================================

// ================= LIBRARY =================
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>   // Install via Library Manager: "WebSockets" by Markus Sattler
#include <PZEM004Tv30.h>
#include "time.h"
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>   // Install via Library Manager: "LiquidCrystal I2C" by Frank de Brabander
#include <Adafruit_INA219.h>     // Install via Library Manager: "Adafruit INA219" by Adafruit

// ================= WIFI =================
char ssid[] = "";   // ISI: nama WiFi rumah/tempatmu
char pass[] = "";   // ISI: password WiFi tersebut
const char* ap_ssid = "Smart Panel By HLM25";   // nama hotspot ESP32 (boleh diganti)
const char* ap_pass = "";                       // ISI: password hotspot, MIN 8 karakter (kosong = hotspot terbuka)

// IP statis untuk WiFi rumah (STA) -- ISI/ganti sesuai jaringan kamu.
// Nilai di bawah hanya CONTOH. IP harus satu subnet dengan router dan
// di luar range DHCP router supaya tidak bentrok.
IPAddress STA_LOCAL_IP(192, 168, 1, 50);  // ISI: IP statis ESP32 (CONTOH)
IPAddress STA_GATEWAY (192, 168, 1, 1);   // ISI: IP router (CONTOH)
IPAddress STA_SUBNET  (255, 255, 255, 0); // biasanya 255.255.255.0
// PENTING -- PENYEBAB UTAMA "NTP GAGAL / TIMER TIDAK JALAN":
// WiFi.config() TANPA DNS server membuat ESP32 BISA tersambung ke router
// (RSSI normal), TAPI tidak bisa menerjemahkan nama domain (mis.
// "id.pool.ntp.org") ke alamat IP -- jadi NTP SELALU gagal walau WiFi
// "Terhubung". Makanya DNS wajib diisi dengan benar.
// DNS1 pakai gateway/router, DNS2 pakai DNS publik Google sebagai cadangan.
IPAddress STA_DNS1     (192, 168, 1, 1);  // ISI: biasanya sama dengan STA_GATEWAY (CONTOH)
IPAddress STA_DNS2     (8, 8, 8, 8);      // cadangan: Google Public DNS

// IP untuk Access Point (AP) -- default bawaan ESP32 192.168.4.1
IPAddress AP_LOCAL_IP (192, 168, 4, 1);
IPAddress AP_GATEWAY  (192, 168, 4, 1);
IPAddress AP_SUBNET   (255, 255, 255, 0);

// ================= ADMIN LOGIN (kontrol relay & timer) =================
String LOGIN_PASSWORD    = "";   // ISI: password tombol LOGIN header (kosong = login ditolak). Bisa diganti lewat Control Panel.
String adminToken        = "";
unsigned long tokenCreatedMillis = 0;

// ================= CONTROL PANEL (status/password/kalibrasi/log) =================
// Password TERPISAH dari LOGIN_PASSWORD di atas — sengaja dipisah supaya
// login relay/timer tidak otomatis membuka Control Panel, dan sebaliknya.
String CP_PASSWORD       = "";   // ISI: password Control Panel (kosong = login ditolak). Bisa diganti lewat Control Panel.
String cpToken            = "";
unsigned long cpTokenCreatedMillis = 0;

// ================= PIN =================
#define RELAY1    26
#define RELAY2    27
#define RELAY3    14
#define RELAY4    13
// (Sensor tegangan analog BATT_PIN/PANEL_PIN sudah dihapus — sekarang pakai INA219)
#define PZEM_RX   16
#define PZEM_TX   17

// ================= I2C — LCD + INA219 gabung di 1 bus yang sama =================
// Pin 32/33 sempat dicoba dipakai buat bus terpisah, tapi ternyata pin itu
// bermasalah di board ESP32 ini -> balik pakai 1 bus tunggal 21/22 untuk
// SEMUA device I2C (LCD, INA219 #1, INA219 #2), digabung/paralel. Ini aman
// karena alamat masing-masing device sudah unik (LCD=0x27, INA219=0x40/0x41).
#define I2C_SDA   21
#define I2C_SCL   22

// Tarif listrik PLN 900VA non-subsidi (Rp per kWh).
// Ganti angka ini kalau tarif PLN berubah atau daya listrik kamu beda.
#define TARIFF_PER_KWH  1352.0

float rpLimit      = 0;      // Limit biaya listrik (Rupiah). 0 = limit nonaktif.
bool  budgetLocked  = false; // true = biaya sudah menyentuh limit -> SEMUA relay dikunci OFF

float maxAcCurrent   = 0;     // Batas maksimal arus AC (Ampere). 0 = nonaktif.
bool  currentTripped = false; // true = arus pernah menyentuh/lebihi batas -> SEMUA relay
                               // TRIP OFF dan TETAP OFF walau arus sudah turun lagi,
                               // sampai direset manual dari Control Panel (seperti MCB).

// ================= BATAS TEGANGAN AC =================
#define AC_BATAS_BAWAH 100.0   // batas bawah (referensi lama, tidak dipakai aktif)
#define AC_BATAS_ATAS  200.0   // batas atas  (referensi lama, tidak dipakai aktif)
#define AC_PLN_ADA     50.0    // >= 50V → PLN dianggap ADA
                               // <  50V → PLN dianggap TIDAK ADA

// ================= OBJECT =================
WebServer        server(80);
WebSocketsServer webSocket(81);   // WebSocket untuk push data sensor realtime (port terpisah dari HTTP)
PZEM004Tv30      pzem(Serial2, PZEM_RX, PZEM_TX);
Preferences       prefs;

// LCD I2C 16x2. Alamat umum: 0x27 atau 0x3F.
// Kalau layar blank/kotak-kotak setelah upload, ganti 0x27 <-> 0x3F.
#define LCD_ADDR 0x27
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// ================= INA219 (SENSOR TEGANGAN + ARUS) =================
// Sensor #1 (alamat default 0x40, A0 & A1 tidak disolder) → BATERAI
// Sensor #2 (alamat 0x41, pin A0 disolder jumper ke VCC)  → PANEL SURYA
// Kalau alamat i2c beda di modul kamu, cek dgn I2C scanner lalu ganti di bawah ini.
Adafruit_INA219 inaBatt(0x40);
Adafruit_INA219 inaPanel(0x41);
bool inaBattOK  = false;
bool inaPanelOK = false;

// true kalau LCD terdeteksi nyambung di bus I2C. Semua pemanggilan fungsi
// lcd.* HARUS dicek pakai flag ini dulu -- kalau tidak, dan LCD tidak
// tertancap, komunikasi I2C ke alamat yang tidak ada akan HANG (macet
// total) di ESP32 karena tidak ada ACK dari device, dan itu ikut membekukan
// loop() (termasuk server.handleClient()) sehingga web seakan mati / data
// stuck di 0.
bool lcdPresent = false;
unsigned long lastLcdScanMillis = 0;
const unsigned long LCD_SCAN_INTERVAL = 5000UL;  // coba deteksi ulang tiap 5 detik kalau belum ada

// ================= DATA SENSOR DC =================
float voltageRaw = 0;  // tegangan baterai (dari INA219 #1)
float currentRaw = 0;  // arus baterai (A). Wiring: VIN+ ke baterai, VIN- ke beban -> + = discharge (ke beban), - = charging
float powerRaw   = 0;  // daya baterai (W)
float energyWh   = 0;  // energi baterai hari ini (Wh), reset tiap jam 00:00
float panelVolt  = 0;  // tegangan panel surya (dari INA219 #2)
float panelCurrentRaw = 0;  // arus panel surya (A)
float panelPowerRaw   = 0;  // daya panel surya (W)
float panelEnergyWh   = 0;  // energi panel surya hari ini (Wh), reset tiap jam 00:00
int   lastEnergyResetDay = -1;  // buat deteksi pergantian hari (reset energi harian)

const float BATT_CAPACITY_AH = 30.0f;   // kapasitas baterai (Ah) -- dipakai buat hitung Wh = Ah x V real-time
float battWhNow = 0;   // hasil hitungan 30Ah x tegangan baterai saat ini (Wh)

// ================= COULOMB COUNTING: KAPASITAS ASLI BATERAI (Ah) =================
// Tujuan: mengukur kapasitas SEBENARNYA dari baterai (bukan cuma percaya label
// "30Ah" yang tertulis di baterainya), dengan cara mengintegralkan arus (A)
// terhadap waktu (jam) -> Ampere-hour (Ah), baik saat discharge (ke beban)
// maupun saat charge (dari panel/PLN via charger).
//
// CARA PAKAI (manual, dilakukan sendiri oleh pengguna):
//  1. Charge baterai sampai benar-benar PENUH (100%).
//  2. Tekan tombol RESET FISIK di board ESP32 (bukan dari web) -> otomatis
//     kedua akumulasi ini (dischargeAhTotal & chargeAhTotal) kembali ke 0,
//     karena keduanya SENGAJA hanya variabel RAM (tidak disimpan ke flash).
//  3. Biarkan baterai terpakai secara normal (lewat beban asli) sampai
//     benar-benar habis/cutoff.
//  4. Nilai "Ah Terpakai (Discharge)" pada saat itu = kapasitas ASLI baterai
//     dalam Ah, tinggal dibandingkan dengan label di baterai (mis. 30Ah).
//
// Sengaja TIDAK dibackup ke flash (beda dengan energyWh/acEnergy) supaya
// pengetesan ini selalu mulai bersih dari 0 kapanpun user menekan RESET FISIK,
// tanpa perlu tombol reset terpisah di web.
float dischargeAhTotal = 0;   // total Ah yang sudah KELUAR dari baterai (discharge/ke beban), sejak boot/reset terakhir
float chargeAhTotal    = 0;   // total Ah yang sudah MASUK ke baterai (charge/dari panel-PLN), sejak boot/reset terakhir

// Ambang noise arus (Ampere). Sensor INA219 punya offset/noise kecil di
// sekitar 0A (mis. -0.001A s/d -0.01A) walau tidak ada arus mengalir sama
// sekali. Tanpa ambang ini, LCD & web menampilkan angka "hantu" seperti
// "-0.00A"/"-0.001A" saat idle, yang hilang lagi begitu arus sungguhan
// (lebih besar dari noise) mengalir -- itu sebabnya tanda "-" terlihat
// muncul-hilang. Nilai di bawah ambang ini dibulatkan jadi 0 murni.
const float CURRENT_NOISE_FLOOR = 0.01f;  // 10 mA

// ================= DATA SENSOR AC =================
float acVoltage = 0;
float acCurrent = 0;
float acPower   = 0;
float acFreq    = 0;
float acPF      = 0;
float acEnergy  = 0;  // Akumulasi energi (kWh) - dibaca langsung dari memori internal PZEM,
                      // sehingga TIDAK ikut ter-reset saat ESP32 restart/reset.

// ================= BACKUP acEnergy KE FLASH (NVS) =================
// PENJELASAN BUG "data jadi 0000 saat ESP restart + PZEM mati":
// acEnergy di atas HANYA variabel RAM, jadi begitu ESP32 restart, nilainya
// selalu mulai dari 0 dulu. Kalau PZEM saat itu MASIH MENYALA, dalam waktu
// <1 detik pembacaan pertama pzem.energy() langsung berhasil dan acEnergy
// langsung terisi angka akumulasi asli dari memori internal PZEM -- makanya
// data langsung "ada". TAPI kalau PZEM sedang MATI/tidak merespons saat ESP
// restart, pzem.energy() akan terus gagal (NaN) sampai PZEM menyala lagi,
// sehingga acEnergy nyangkut di 0 (nilai awal) dan web/LCD ikut menampilkan
// kWh=0.000 & Rp=0 selama itu.
//
// SOLUSI: acEnergy terakhir yang valid dibackup berkala ke flash (Preferences/
// NVS), lalu dimuat kembali sebagai nilai AWAL saat boot (bukan 0) -- jadi
// selama menunggu PZEM merespons lagi, web & LCD tetap menampilkan angka
// terakhir yang benar. Begitu PZEM berhasil terbaca, acEnergy otomatis
// ditimpa oleh nilai asli dari chip PZEM (lebih akurat & selalu naik).
float lastSavedAcEnergy = -1;
unsigned long lastEnergySaveMillis = 0;
const unsigned long ENERGY_SAVE_INTERVAL  = 30000UL; // cek simpan tiap 30 detik
const float ENERGY_SAVE_THRESHOLD = 0.001f;          // kWh, hindari tulis flash tiap goyang kecil

// ---- Backup "Energi Hari Ini" BATERAI & PANEL SURYA (Wh) ke flash juga ----
// PENJELASAN BUG "Energi Hari Ini reset ke 0 tiap ESP restart":
// energyWh (baterai) & panelEnergyWh (panel) dulu HANYA variabel RAM biasa
// (beda dengan acEnergy yang sudah dibackup di atas), jadi begitu ESP32
// reset/restart nilainya otomatis balik ke 0 walau secara logika hari yang
// sama belum berganti. SOLUSI: dibackup berkala ke flash (Preferences) sama
// seperti acEnergy, lalu dimuat lagi sebagai nilai AWAL saat boot (BUKAN 0)
// -- jadi energi hari ini melanjutkan angka terakhir sebelum restart, dan
// HANYA benar-benar direset ke 0 saat pergantian tanggal (jam 00:00), bukan
// setiap kali ESP restart.
float lastSavedEnergyWh      = -1;
float lastSavedPanelEnergyWh = -1;
const float ENERGY_WH_SAVE_THRESHOLD = 0.5f;  // Wh, hindari tulis flash tiap goyang kecil

void saveEnergyToFlash() {
  prefs.begin("energy", false);
  prefs.putFloat("acE", acEnergy);
  prefs.putFloat("battE", energyWh);
  prefs.putFloat("panelE", panelEnergyWh);
  prefs.end();
  lastSavedAcEnergy      = acEnergy;
  lastSavedEnergyWh      = energyWh;
  lastSavedPanelEnergyWh = panelEnergyWh;
  lastEnergySaveMillis   = millis();
}

void loadEnergyFromFlash() {
  prefs.begin("energy", true);
  acEnergy      = prefs.getFloat("acE", 0);
  energyWh      = prefs.getFloat("battE", 0);
  panelEnergyWh = prefs.getFloat("panelE", 0);
  prefs.end();
  lastSavedAcEnergy      = acEnergy;
  lastSavedEnergyWh      = energyWh;
  lastSavedPanelEnergyWh = panelEnergyWh;
  if (acEnergy > 0) {
    Serial.printf("[ENERGY] Backup dimuat dari flash: %.3f kWh (dipakai sementara sampai PZEM terbaca)\n", acEnergy);
  }
  if (energyWh > 0 || panelEnergyWh > 0) {
    Serial.printf("[ENERGY] Energi hari ini dimuat dari flash: Baterai=%.1fWh, Panel=%.1fWh (melanjutkan, tidak reset)\n", energyWh, panelEnergyWh);
  }
}

// ================= KURVA SOC (STATE OF CHARGE) — LiFePO4 4S 12.8V 30Ah =================
// PENTING: LiFePO4 (LFP) punya kurva tegangan yang SANGAT DATAR dibanding aki
// biasa (lead-acid/SLA) -- sebagian besar kapasitas (dari ~90% sampai ~20%)
// cuma bergeser di rentang 12.8V - 13.2V saja, lalu voltase "jatuh" cepat
// di ujung-ujung (dekat full 14.6V atau dekat kosong <12.0V).
// Karena itu TIDAK BOLEH dipetakan pakai garis lurus (linear) biasa seperti
// aki -- kalau dipaksa linear, hasil %  akan jauh meleset (medium terlihat
// "penuh" padahal sebenarnya rendah, atau sebaliknya).
// Tabel di bawah adalah kurva discharge standar LiFePO4 4S (per sel 2.50V -
// 3.65V) dalam kondisi rest/tanpa beban. Titik cutoff 0% sengaja diset di
// 10.0V (2.50V/sel) sebagai batas aman standar BMS LiFePO4.
// Kalau mau lebih presisi, titik-titik ini bisa disesuaikan dengan hasil ukur
// AVO baterai kamu sendiri (misal pas benar-benar 100%/50%/0% menurut BMS).
const int SOC_LUT_SIZE = 11;
float SOC_LUT_V[SOC_LUT_SIZE]   = { 10.00f, 12.00f, 12.40f, 12.64f, 12.80f, 12.84f, 12.88f, 12.92f, 13.00f, 13.20f, 14.60f };
float SOC_LUT_PCT[SOC_LUT_SIZE] = {   0,      5,      10,     20,     30,     40,     50,     60,     70,     90,     100   };

float battSOC(float v) {
  if (v <= SOC_LUT_V[0]) return 0.0f;
  if (v >= SOC_LUT_V[SOC_LUT_SIZE - 1]) return 100.0f;
  for (int i = 0; i < SOC_LUT_SIZE - 1; i++) {
    if (v >= SOC_LUT_V[i] && v <= SOC_LUT_V[i + 1]) {
      float t = (v - SOC_LUT_V[i]) / (SOC_LUT_V[i + 1] - SOC_LUT_V[i]);
      return SOC_LUT_PCT[i] + t * (SOC_LUT_PCT[i + 1] - SOC_LUT_PCT[i]);
    }
  }
  return 0.0f;
}

// ================= KALIBRASI =================
float PANEL_CAL = 1.0;    // Kalibrasi tegangan panel surya

// Lookup table kalibrasi baterai: (volt_web_raw, volt_avo_asli)
// Diurutkan dari kecil ke besar berdasarkan volt_web
//
// CATATAN PENTING: tabel ini dulu dibuat buat SENSOR TEGANGAN ANALOG LAMA
// (voltage divider + ADC ESP32), yang memang butuh koreksi manual karena
// toleransi resistor & nonlinearitas ADC bisa meleset cukup jauh dari
// tegangan asli. INA219 SUDAH membaca tegangan bus asli langsung dari
// chip-nya sendiri (akurasi pabrik ±0.5%-1%), jadi TIDAK butuh kalibrasi
// sedrastis itu -- kalau tabel lama tetap dipakai, hasilnya malah bikin
// distorsi (misal 14.2V asli kebaca 14.6V karena ke-clamp ke titik
// tertinggi tabel lama yang cuma sampai ~13V raw).
//
// Makanya default di bawah ini di-reset jadi NETRAL/PASSTHROUGH (raw = AVO,
// alias tidak ada koreksi sama sekali) dengan rentang yang mencakup penuh
// operasi baterai LiFePO4 (cutoff ~8V sampai full charge 15V). Kalau nanti
// setelah dibandingkan ke AVO meter ternyata INA219 kamu punya selisih
// kecil yang konsisten, tinggal isi 17 titik ini manual lewat Control Panel
// (Admin -> Kalibrasi Baterai) dengan data pembacaan INA219 asli vs AVO
// meter asli -- bukan pakai tabel lama ini.
const int BATT_LUT_SIZE = 17;
float BATT_LUT_WEB[BATT_LUT_SIZE] = { 8.00f, 8.44f, 8.88f, 9.31f, 9.75f, 10.19f, 10.63f, 11.06f, 11.50f, 11.94f, 12.38f, 12.81f, 13.25f, 13.69f, 14.13f, 14.56f, 15.00f };
float BATT_LUT_AVO[BATT_LUT_SIZE] = { 8.00f, 8.44f, 8.88f, 9.31f, 9.75f, 10.19f, 10.63f, 11.06f, 11.50f, 11.94f, 12.38f, 12.81f, 13.25f, 13.69f, 14.13f, 14.56f, 15.00f };

// ================= LOG AKSES ADMIN =================
#define MAX_LOGS 20
struct AccessLog { char ip[20]; char waktu[25]; bool berhasil; };
AccessLog accessLogs[MAX_LOGS];
int       logCount   = 0;
unsigned long bootMillis = 0;

float battCalibrate(float rawVolt) {
  if (rawVolt <= BATT_LUT_WEB[0]) return BATT_LUT_AVO[0];
  if (rawVolt >= BATT_LUT_WEB[BATT_LUT_SIZE-1]) return BATT_LUT_AVO[BATT_LUT_SIZE-1];
  for (int i = 0; i < BATT_LUT_SIZE - 1; i++) {
    if (rawVolt >= BATT_LUT_WEB[i] && rawVolt <= BATT_LUT_WEB[i+1]) {
      float t = (rawVolt - BATT_LUT_WEB[i]) / (BATT_LUT_WEB[i+1] - BATT_LUT_WEB[i]);
      return BATT_LUT_AVO[i] + t * (BATT_LUT_AVO[i+1] - BATT_LUT_AVO[i]);
    }
  }
  return rawVolt;
}

// battCalibrate() WAJIB data terurut naik (ascending) berdasarkan BATT_LUT_WEB.
// Kalau titik yang diinput dari admin panel urutannya kebalik/tidak monoton
// (sering kejadian kalau titik ke-4/ke-5 dekat sekali atau titik tegangan
// tinggi ternyata secara raw sensor tidak lebih besar dari titik sebelumnya),
// loop di atas gagal mencocokkan range dan HASILNYA rawVolt DIKEMBALIKAN
// APA ADANYA (tidak dikalibrasi) -> inilah penyebab "kalibrasi tidak ngaruh".
// Fungsi ini mengurutkan ulang pasangan (WEB,AVO) berdasarkan WEB menaik
// setiap kali data baru disimpan/dimuat, supaya battCalibrate() selalu valid.
void sortBattLUT() {
  for (int i = 1; i < BATT_LUT_SIZE; i++) {
    float kw = BATT_LUT_WEB[i], ka = BATT_LUT_AVO[i];
    int j = i - 1;
    while (j >= 0 && BATT_LUT_WEB[j] > kw) {
      BATT_LUT_WEB[j+1] = BATT_LUT_WEB[j];
      BATT_LUT_AVO[j+1] = BATT_LUT_AVO[j];
      j--;
    }
    BATT_LUT_WEB[j+1] = kw;
    BATT_LUT_AVO[j+1] = ka;
  }
}

// ================= TIMER INTERVAL =================
unsigned long lastEnergyMillis = 0;
unsigned long lastSensorMillis = 0;
unsigned long lastPzemMillis   = 0;

// ================= RELAY STATE =================
bool relay1State = false;  // INVERTER
bool relay2State = false;  // LAMPU UTAMA
bool relay3State = false;  // OUTPUT AC
bool relay4State = false;  // LAMPU PANEL

// Status timer aktif saat ini (dipakai gabungan bersama PZEM untuk Lampu Utama)
bool timerActiveNow = false;

// ================= MANUAL OVERRIDE =================
// Jika true → relay sedang dalam mode manual (baru saja ditoggle
// dari web).
//
// Relay 1 (Inverter), Relay 2 (Lampu Utama) & Relay 3 (Output AC):
// manual BERTAHAN sampai user tekan tombol "AUTO" (atau, khusus R3,
// sampai kondisi timer berubah) — TIDAK otomatis kembali sendiri.
//
// Relay 4: manual hanya SESAAT. Setiap ±1 detik, fungsi
// checkAcVoltage() otomatis me-reset flag ini ke false dan memaksa
// relay balik mengikuti PZEM. Jadi TIDAK PERLU menekan tombol AUTO
// untuk R4 — PZEM selalu menang dengan sendirinya dalam hitungan detik.
bool manualR1 = false;  // INVERTER manual (bertahan sampai ditekan AUTO)
bool manualR2 = false;  // LAMPU UTAMA manual (bertahan sampai ditekan AUTO)
bool manualR3 = false;  // OUTPUT AC manual (bertahan sampai ditekan AUTO)
bool manualR4 = false;  // LAMPU PANEL manual (sesaat, di-override PZEM)

// ================= NTP =================
const char*   ntpServer    = "id.pool.ntp.org";
time_t        ntpBaseTime  = 0;
unsigned long ntpBaseMillis = 0;
bool          ntpSynced    = false;

// ================= AUTO TIMER =================
int autoStartH = 18;
int autoStartM = 0;
int autoEndH   = 21;
int autoEndM   = 0;

// ================= SIMPAN & MUAT TIMER KE FLASH =================
void saveTimer() {
  prefs.begin("timer", false);
  prefs.putInt("sh", autoStartH);
  prefs.putInt("sm", autoStartM);
  prefs.putInt("eh", autoEndH);
  prefs.putInt("em", autoEndM);
  prefs.end();
  Serial.printf("[TIMER] Disimpan: %02d:%02d - %02d:%02d\n", autoStartH, autoStartM, autoEndH, autoEndM);
}

void loadTimer() {
  prefs.begin("timer", true);
  autoStartH = prefs.getInt("sh", 18);
  autoStartM = prefs.getInt("sm", 0);
  autoEndH   = prefs.getInt("eh", 21);
  autoEndM   = prefs.getInt("em", 0);
  prefs.end();
  Serial.printf("[TIMER] Dimuat : %02d:%02d - %02d:%02d\n", autoStartH, autoStartM, autoEndH, autoEndM);
}

// ================================================================
//  FUNGSI: GET CURRENT TIME
// ================================================================
// CATATAN PENTING (bug lama): dulu fungsi ini mencoba getLocalTime()
// dengan timeout 2000ms SETIAP KALI dipanggil (setiap 5 detik lewat
// checkAutoTime()) selama NTP belum sync. Kalau NTP server pertama
// (id.pool.ntp.org) lambat/tidak bisa dijangkau, ini bikin loop() ke-
// block 2 detik berulang-ulang TANPA HENTI, dan yang lebih parah:
// checkAutoTime() jadi selalu early-return (lihat pemanggilnya) --
// artinya SELURUH logika timer & auto-relay (termasuk Output AC) tidak
// pernah jalan sama sekali kalau NTP gagal sync. Ini penyebab paling
// mungkin dari "timer tidak nyala sesuai jadwal" dan "manual->auto tidak
// balik" yang dilaporkan, karena bukan cuma timer yang macet, TAPI
// SEMUA relay yang bergantung ke checkAutoTime() (termasuk kembalinya
// Output AC ke mode auto) ikut macet juga.
//
// Perbaikan: retry NTP diberi jeda (cooldown) supaya tidak nge-block
// terus-menerus, dan configTime() sekarang punya 2 server cadangan.
bool getCurrentTime(struct tm* timeinfo) {
  if (!ntpSynced) {
    static unsigned long lastNtpAttempt = 0;
    // Cuma coba ulang tiap 10 detik sekali (bukan tiap dipanggil/5 detik),
    // dan timeout dipersingkat jadi 500ms supaya tidak lama nge-block loop().
    if (millis() - lastNtpAttempt < 10000UL && lastNtpAttempt != 0) return false;
    lastNtpAttempt = millis();

    if (getLocalTime(timeinfo, 500)) {
      ntpBaseTime   = mktime(timeinfo);
      ntpBaseMillis = millis();
      ntpSynced     = true;
      Serial.println("[NTP] Sync berhasil!");
      return true;
    }
    Serial.println("[NTP] Gagal sync (server belum terjangkau) -- coba lagi 10 detik lagi. Timer/Auto-relay ditunda sampai NTP berhasil.");
    return false;
  }
  time_t nowEpoch = ntpBaseTime + (time_t)((millis() - ntpBaseMillis) / 1000UL);
  localtime_r(&nowEpoch, timeinfo);
  static unsigned long lastNtpSync = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastNtpSync > 3600000UL) {
    struct tm fresh;
    if (getLocalTime(&fresh, 500)) {
      ntpBaseTime   = mktime(&fresh);
      ntpBaseMillis = millis();
      lastNtpSync   = millis();
      Serial.println("[NTP] Re-sync berhasil!");
    }
  }
  return true;
}

// ================================================================
//  FUNGSI: APPLY RELAY
// ================================================================
void applyRelay() {
  // Kalau budgetLocked (limit biaya) ATAU currentTripped (over-current trip)
  // aktif, SEMUA relay dipaksa OFF secara fisik di sini -- tidak peduli
  // status relayXState-nya apa (manual ON, auto ON, dsb). Ini satu-satunya
  // tempat yang benar-benar menulis ke pin fisik, jadi menjamin tidak ada
  // jalur lain yang bisa menyalakan relay selama masih terkunci/trip.
  bool lock = budgetLocked || currentTripped;
  digitalWrite(RELAY1, (relay1State && !lock) ? LOW : HIGH);
  digitalWrite(RELAY2, (relay2State && !lock) ? LOW : HIGH);
  digitalWrite(RELAY3, (relay3State && !lock) ? LOW : HIGH);
  digitalWrite(RELAY4, (relay4State && !lock) ? LOW : HIGH);
}

// ================================================================
//  FUNGSI: CEK RANGE TIMER
// ================================================================
bool isInTimerRange(int nowH, int nowM) {
  int nowTot   = nowH * 60 + nowM;
  int startTot = autoStartH * 60 + autoStartM;
  int endTot   = autoEndH   * 60 + autoEndM;
  if (startTot <= endTot) return (nowTot >= startTot && nowTot < endTot);
  return (nowTot >= startTot || nowTot < endTot);  // melewati tengah malam
}

// ================================================================
//  HELPER: Cek apakah PLN ada
// ================================================================
bool isPLNAda() {
  return (acVoltage >= AC_PLN_ADA);  // PLN ada jika >= 50V
}

// ================================================================
//  FUNGSI: AUTO TIME CONTROL
//
//  Relay 3 (Output AC):
//    → ON selama timer aktif, OFF selama timer tidak aktif
//    → TIDAK dipengaruhi PLN/PZEM sama sekali, murni ikut jadwal timer
//
//  Relay 2 (Lampu Utama):
//    → ON jika Timer aktif ATAU PLN tidak ada (gabungan timer + PZEM)
//    → Lihat evaluateLampuUtama()
//
//  Relay 1 (Inverter) & Relay 4 (Lampu Panel):
//    → TIDAK ikut timer. Sepenuhnya diatur oleh logika PZEM
//      di checkAcVoltage() — ON hanya saat PLN benar-benar tidak ada.
//
//  Semua relay (1-4) tetap bisa dioperasikan manual dari web kapan saja.
// ================================================================
void evaluateLampuUtama();  // forward declaration

void checkAutoTime() {
  struct tm timeinfo;
  if (!getCurrentTime(&timeinfo)) return;

  bool timerOn = isInTimerRange(timeinfo.tm_hour, timeinfo.tm_min);
  timerActiveNow = timerOn;

  // State yang seharusnya menurut logika timer (hanya R3)
  bool r3Should = timerOn;

  // Deteksi perubahan kondisi timer — hanya untuk logging
  static bool lastTimerOn = false;
  bool kondisiBerubah = (timerOn != lastTimerOn);
  if (kondisiBerubah) {
    lastTimerOn = timerOn;
    Serial.printf("[AUTO] Kondisi timer berubah -> %s\n", timerOn ? "ON" : "OFF");
  }

  // Terapkan ke relay hanya jika tidak sedang manual override
  bool changed = false;
  if (!manualR3 && r3Should != relay3State) { relay3State = r3Should; changed = true; }

  if (changed) {
    applyRelay();
    Serial.printf("[AUTO] R3(OutAC):%s | Manual:%d\n",
                  relay3State?"ON":"OFF", manualR3);
  }

  // Timer ikut menentukan Lampu Utama (gabungan dengan status PZEM)
  evaluateLampuUtama();
}

// ================================================================
//  FUNGSI: EVALUASI LAMPU UTAMA (Relay 2) — GABUNGAN TIMER + PZEM
//
//  Lampu Utama ON jika salah satu kondisi berikut terpenuhi:
//    → Timer sedang aktif (sesuai jadwal jam yang diset), ATAU
//    → PLN tidak ada (PZEM mendeteksi tegangan < 50V)
//  Lampu Utama OFF jika timer tidak aktif DAN PLN ada.
//
//  Toggle manual TETAP BISA dipencet dari web. SELAMA manualR2 masih
//  true, Timer+PZEM TIDAK akan menimpa status relay ini — persis
//  seperti Relay 3 (Output AC). Manual baru dilepas kalau user
//  menekan tombol "AUTO" (lihat handleRelay(), s=auto).
// ================================================================
void evaluateLampuUtama() {
  bool plnAda = isPLNAda();
  bool should = timerActiveNow || !plnAda;

  // Kalau sedang manual, jangan timpa relay2State sama sekali —
  // tunggu user tekan tombol AUTO.
  if (manualR2) return;

  if (should != relay2State) {
    relay2State = should;
    applyRelay();
    Serial.printf("[LAMPU UTAMA] -> %s (Timer:%s, PLN:%s)\n",
                  relay2State ? "ON" : "OFF",
                  timerActiveNow ? "AKTIF" : "TIDAK AKTIF",
                  plnAda ? "ADA" : "TIDAK ADA");
  }
}

// ================================================================
//  FUNGSI: LOGIKA PZEM REALTIME
//
//  Relay 1 (Inverter):
//    → 0V (PLN tidak ada) → ON
//    → >=50V (PLN ada)    → OFF
//
//  Relay 2 (Lampu Utama):
//    → Dievaluasi lewat evaluateLampuUtama() — gabungan Timer + PZEM
//
//  Relay 4 (Lampu Panel):
//    → 0V (PLN tidak ada) → ON  (lampu panel nyala otomatis)
//    → >=50V (PLN ada)    → OFF (PLN ada, lampu panel mati)
//
//  Toggle manual TETAP BISA dipencet dari web untuk Relay 1 & 4.
//  Fungsi ini dievaluasi ULANG SETIAP ADA DATA PZEM BARU (±1 detik).
//
//  Relay 4 (Lampu Panel): TETAP perilaku lama — setiap kali fungsi
//  ini berjalan, status manual otomatis direset dan relay dipaksa
//  mengikuti PZEM. TIDAK PERLU tekan tombol "AUTO".
//
//  Relay 1 (Inverter): SEKARANG BERTAHAN di mode manual — selama
//  manualR1 masih true, PZEM TIDAK menimpa status relay ini. Manual
//  baru dilepas kalau user menekan tombol "AUTO".
// ================================================================
bool lastPlnState = false;

void checkAcVoltage() {
  bool plnAda = isPLNAda();

  if (plnAda != lastPlnState) {
    lastPlnState = plnAda;
    Serial.printf("[PZEM] PLN berubah → %s (%.1fV)\n",
                  plnAda ? "ADA" : "TIDAK ADA", acVoltage);
  }

  bool r1Should = !plnAda;
  bool r4Should = !plnAda;
  bool changed = false;

  // Relay 4: tetap sesaat, PZEM selalu menang otomatis.
  if (manualR4) {
    manualR4 = false;   // otomatis kembali ke AUTO, tanpa perlu tombol
    Serial.println("[LAMPU PANEL] Manual di-override otomatis oleh PZEM");
  }
  if (r4Should != relay4State) { relay4State = r4Should; changed = true; }

  // Relay 1: HANYA dievaluasi ulang kalau TIDAK sedang manual.
  // Kalau sedang manual, biarkan relay1State apa adanya sampai
  // user menekan tombol AUTO.
  if (!manualR1 && r1Should != relay1State) { relay1State = r1Should; changed = true; }

  if (changed) {
    applyRelay();
    Serial.printf("[PZEM] R1(Inverter):%s R4(LampPanel):%s\n",
                  relay1State?"ON":"OFF", relay4State?"ON":"OFF");
  }

  // Lampu Utama selalu dievaluasi ulang tiap data PZEM masuk
  // (gabungan Timer + PZEM, lihat evaluateLampuUtama())
  evaluateLampuUtama();
}

// ================================================================
//  FUNGSI: CEK LIMIT BIAYA (RUPIAH)
//
//  Kalau rpLimit > 0 dan akumulasi Rupiah (dari total kWh x tarif)
//  sudah menyentuh/melebihi limit yang diset di Control Panel, maka
//  budgetLocked = true, dan applyRelay() akan memaksa SEMUA relay
//  (1-4) OFF secara fisik -- baik statusnya manual maupun auto,
//  walau tombol AUTO dipencet sekalipun tetap tidak akan menyala.
//
//  Kembali normal otomatis begitu limit dinaikkan di Control Panel
//  ke angka yang lebih tinggi dari Rupiah saat ini (atau di-set ke 0
//  untuk menonaktifkan limit sepenuhnya) -- relay akan kembali ke
//  status terakhir (manual/auto) yang tersimpan, tanpa perlu
//  menekan tombol apa pun lagi.
// ================================================================
void checkBudgetLimit() {
  float rpNow = acEnergy * TARIFF_PER_KWH;
  bool overBudget = (rpLimit > 0) && (rpNow >= rpLimit);
  if (overBudget != budgetLocked) {
    budgetLocked = overBudget;
    applyRelay();
    Serial.printf("[BUDGET] %s | Rp %.2f / Limit Rp %.2f\n",
                  budgetLocked ? "LIMIT TERCAPAI - semua relay dikunci OFF"
                               : "Limit dilonggarkan - relay kembali normal",
                  rpNow, rpLimit);
  }
}

// ================================================================
//  FUNGSI: CEK MAKSIMAL ARUS AC (PROTEKSI OVER-CURRENT / TRIP)
//
//  Kalau maxAcCurrent > 0 dan arus AC terbaca (acCurrent) sudah
//  menyentuh/melebihi batas ini, currentTripped = true, dan
//  applyRelay() akan memaksa SEMUA relay OFF -- baik manual maupun
//  auto, walau tombol AUTO dipencet sekalipun tetap tidak menyala.
//
//  BEDA dengan limit biaya: begitu sudah TRIP, relay TETAP OFF
//  meskipun arus sudah turun kembali di bawah batas -- perilaku ini
//  disengaja, meniru MCB/circuit breaker. Satu-satunya cara membuka
//  kunci lagi adalah menekan tombol RESET di Control Panel
//  (lihat resetCurrentTrip()), bukan otomatis pulih sendiri.
// ================================================================
void checkCurrentLimit() {
  if (maxAcCurrent > 0 && !currentTripped && acCurrent >= maxAcCurrent) {
    currentTripped = true;
    applyRelay();
    Serial.printf("[OVERCURRENT] Arus AC %.2fA >= batas %.2fA - SEMUA relay TRIP OFF, perlu RESET manual!\n",
                  acCurrent, maxAcCurrent);
  }
  // Sengaja TIDAK ada auto-recovery di sini walau acCurrent sudah turun
  // lagi di bawah maxAcCurrent -- harus direset manual (lihat di atas).
}

// ================================================================
//  FUNGSI: RESET TRIP ARUS (dipanggil dari Control Panel)
// ================================================================
void resetCurrentTrip() {
  currentTripped = false;
  applyRelay();
  Serial.println("[OVERCURRENT] Trip arus di-reset manual dari Control Panel");
}

// ================================================================
//  FUNGSI: GENERATE TOKEN ADMIN ACAK
// ================================================================
String generateToken() {
  const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  String tok = "";
  randomSeed(esp_random());
  for (int i = 0; i < 24; i++) {
    tok += charset[random(0, sizeof(charset) - 1)];
  }
  return tok;
}

// ================================================================
//  WEB: HANDLE ROOT (HTML)
// ================================================================
void handleRoot() {
  // Kirim HTML secara bertahap (chunked) supaya ESP32 tidak perlu
  // menyimpan seluruh halaman (~77KB) sekaligus di RAM. Tiap potongan
  // dibuat dalam blok {} sendiri supaya langsung dibebaskan dari heap
  // begitu selesai dikirim, sebelum potongan berikutnya dibuat.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  {
    String html1 = R"rawliteral(<!DOCTYPE html>
<html lang="id">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>WEB MONITOR PLTS</title>
  <link href="https://fonts.googleapis.com/css2?family=Orbitron:wght@400;700;900&family=Exo+2:wght@300;400;600;700&display=swap" rel="stylesheet">
  <style>
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    :root {
      --sun: #FFB300; --sg: #FF6F00; --teal: #00E5CC;
      --green: #00E676; --red: #FF1744; --orange: #FF6D00;
      --bg0: #020B18; --bg1: #051525; --bg2: #0A1E35;
      --card: rgba(10,30,55,0.85); --bdr: rgba(0,229,204,0.2);
      --txt: #E0F7FA; --mut: #78909C;
    }
    html { scroll-behavior: smooth; }
    body { font-family: 'Exo 2', sans-serif; background: var(--bg0); color: var(--txt); min-height: 100vh; overflow-x: hidden; }
    .bgl { position: fixed; inset: 0; z-index: 0; pointer-events: none;
      background: radial-gradient(ellipse 80% 60% at 50% -10%, rgba(255,179,0,0.07) 0%, transparent 70%),
                  radial-gradient(ellipse 60% 50% at 80% 100%, rgba(0,229,204,0.06) 0%, transparent 60%),
                  linear-gradient(180deg, #020B18 0%, #051525 40%, #0A1E35 100%); }
    .bgg { position: fixed; inset: 0; z-index: 0; pointer-events: none; overflow: hidden; }
    .bgg::before { content: ''; position: absolute; inset: -50%;
      background-image: linear-gradient(rgba(0,229,204,0.04) 1px, transparent 1px),
                        linear-gradient(90deg, rgba(0,229,204,0.04) 1px, transparent 1px);
      background-size: 48px 48px; animation: gm 20s linear infinite; }
    @keyframes gm { to { transform: translateY(48px); } }
    .wrap { position: relative; z-index: 1; max-width: 1280px; margin: 0 auto; padding: 0 20px 40px; }
    header { position: relative; text-align: center; padding: 32px 20px 26px; border-bottom: 1px solid var(--bdr); overflow: hidden; }
    header::after { content: ''; position: absolute; bottom: 0; left: 50%; transform: translateX(-50%);
      width: 60%; height: 1px; background: linear-gradient(90deg, transparent, var(--teal), var(--sun), var(--teal), transparent); }
    .hicon { width: 56px; height: 56px; margin: 0 auto 12px;
      background: radial-gradient(circle, var(--sun) 0%, var(--sg) 60%, transparent 100%);
      border-radius: 50%; display: flex; align-items: center; justify-content: center; font-size: 26px;
      box-shadow: 0 0 30px rgba(255,179,0,0.5); animation: sp 4s ease-in-out infinite; }
    @keyframes sp { 0%,100% { transform: scale(1); } 50% { transform: scale(1.06); } }
    .htitle { font-family: 'Orbitron', monospace; font-size: clamp(20px,4.5vw,40px); font-weight: 900; letter-spacing: 5px;
      background: linear-gradient(135deg, var(--sun) 0%, #FFF176 40%, var(--teal) 100%);
      -webkit-background-clip: text; -webkit-text-fill-color: transparent; background-clip: text; }
    .hsub { font-size: 11px; letter-spacing: 3px; color: var(--mut); text-transform: uppercase; margin-top: 6px; }
    .lbdg { display: inline-flex; align-items: center; gap: 6px;
      background: rgba(0,230,118,0.1); border: 1px solid rgba(0,230,118,0.3);
      border-radius: 20px; padding: 3px 12px; font-size: 10px; color: var(--green); letter-spacing: 2px; margin-top: 10px; }
    .admin-btn { position: absolute; top: 10px; right: 10px; z-index: 50; display: inline-flex; align-items: center; gap: 4px;
      background: rgba(0,229,204,0.08); border: 1px solid var(--bdr); border-radius: 16px;
      padding: 5px 10px; font-size: 9px; color: var(--teal); letter-spacing: 0.5px; cursor: pointer;
      transition: all 0.25s; font-family: 'Orbitron', monospace; white-space: nowrap; line-height: 1.4; }
    .admin-btn:hover { background: rgba(0,229,204,0.16); border-color: rgba(0,229,204,0.5); }
    .admin-btn.logged { color: var(--green); border-color: rgba(0,230,118,0.4); background: rgba(0,230,118,0.1); }
    @media (max-width: 600px) { .admin-btn { top: 8px; right: 8px; padding: 4px 8px; font-size: 8px; } }
    .modal-overlay { position: fixed; inset: 0; z-index: 1000; background: rgba(2,11,24,0.78);
      display: none; align-items: center; justify-content: center; padding: 20px; backdrop-filter: blur(4px); }
    .modal-overlay.show { display: flex; }
    .modal-box { background: var(--bg2); border: 1px solid var(--bdr); border-radius: 16px;
      padding: 28px 26px; max-width: 340px; width: 100%; position: relative;
      box-shadow: 0 0 40px rgba(0,229,204,0.15); }
    .modal-box::before { content: ''; position: absolute; top: 0; left: 0; right: 0; height: 2px;
      background: linear-gradient(90deg, var(--sun), var(--teal), var(--sun)); border-radius: 16px 16px 0 0; }
    .modal-title { font-family: 'Orbitron', monospace; font-size: 14px; letter-spacing: 2px;
      color: var(--teal); margin-bottom: 16px; text-align: center; }
    .modal-box input[type=password] { width: 100%; background: rgba(0,0,0,0.35);
      border: 1px solid rgba(0,229,204,0.25); border-radius: 10px; color: var(--txt);
      font-size: 14px; padding: 11px 14px; outline: none; transition: border-color 0.3s; margin-bottom: 14px; }
    .modal-box input[type=password]:focus { border-color: rgba(0,229,204,0.6); }
    .modal-btns { display: flex; gap: 10px; }
    .modal-btns button { flex: 1; padding: 10px; border-radius: 10px; cursor: pointer;
      font-family: 'Orbitron', monospace; font-size: 11px; letter-spacing: 1.5px; transition: all 0.25s; }
    .modal-submit { border: 1px solid rgba(0,230,118,0.4); background: rgba(0,230,118,0.12); color: var(--green); }
    .modal-submit:hover { background: rgba(0,230,118,0.22); }
    .modal-cancel { border: 1px solid rgba(255,23,68,0.3); background: rgba(255,23,68,0.08); color: var(--red); }
    .modal-cancel:hover { background: rgba(255,23,68,0.18); }
    .modal-err { font-size: 11px; color: var(--red); text-align: center; margin-top: 10px; min-height: 14px; }
    .lock-note { font-size: 9px; color: var(--mut); text-align: center; margin-top: 8px; letter-spacing: 0.5px; }
    .ldot { width: 6px; height: 6px; border-radius: 50%; background: var(--green); animation: bk 1.2s infinite; }
    @keyframes bk { 0%,100% { opacity: 1; } 50% { opacity: 0.2; } }
    .clkval { font-family: 'Orbitron', monospace; font-size: 18px; color: var(--teal); letter-spacing: 2px; margin-top: 10px; }
    .banner { margin: 24px 0 0; padding: 13px 20px; border-radius: 12px;
      background: linear-gradient(135deg, rgba(255,179,0,0.08), rgba(0,229,204,0.08));
      border: 1px solid rgba(255,179,0,0.2);
      display: flex; align-items: center; gap: 12px; font-size: 12px; color: var(--mut); }
    .banner strong { color: var(--sun); }
    .acb { margin: 10px 0 0; padding: 13px 20px; border-radius: 12px;
      display: flex; align-items: center; gap: 12px; font-size: 12px; transition: all 0.4s; }
    .acb.ok  { background: rgba(0,230,118,0.08); border: 1px solid rgba(0,230,118,0.3); color: var(--mut); }
    .acb.warn{ background: rgba(255,109,0,0.08); border: 1px solid rgba(255,109,0,0.4); color: var(--mut); }
    .acb.ok strong  { color: var(--green); }
    .acb.warn strong{ color: var(--orange); }
    .slbl { font-family: 'Orbitron', monospace; font-size: 10px; letter-spacing: 3px; color: var(--teal);
      text-transform: uppercase; margin: 32px 0 14px; display: flex; align-items: center; gap: 10px; }
    .slbl::after { content: ''; flex: 1; height: 1px; background: linear-gradient(90deg, var(--bdr), transparent); }
    .tcard { background: var(--card); border: 1px solid rgba(255,179,0,0.25);
      border-radius: 16px; padding: 22px 26px; position: relative; overflow: hidden; }
    .tcard::before { content: ''; position: absolute; top: 0; left: 0; right: 0; height: 2px;
      background: linear-gradient(90deg, var(--sun), var(--teal), var(--sun)); }
    .thead { display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 10px; margin-bottom: 18px; }
    .thl   { display: flex; align-items: center; gap: 10px; }
    .tico  { width: 38px; height: 38px; border-radius: 10px; background: rgba(255,179,0,0.12);
      color: var(--sun); display: flex; align-items: center; justify-content: center; font-size: 18px; }
    .tlbl  { font-family: 'Orbitron', monospace; font-size: 11px; letter-spacing: 2px; color: var(--sun); }
    .tlbl small { display: block; font-family: 'Exo 2', sans-serif; font-size: 10px; color: var(--mut); letter-spacing: 1px; margin-top: 2px; }
    .tbdg { padding: 4px 12px; border-radius: 20px; font-size: 10px; font-weight: 700; letter-spacing: 2px;
      background: rgba(0,230,118,0.12); color: var(--green); border: 1px solid rgba(0,230,118,0.3); font-family: 'Orbitron', monospace; }
    .tbdg.pend { background: rgba(255,179,0,0.12); color: var(--sun); border-color: rgba(255,179,0,0.35); }
    .tarow { display: flex; align-items: center; gap: 8px; font-size: 11px; color: var(--mut);
      letter-spacing: 1px; margin-bottom: 18px; padding: 9px 13px;
      background: rgba(0,0,0,0.2); border-radius: 8px; border: 1px solid rgba(255,255,255,0.05); flex-wrap: wrap; }
    .tv { color: var(--teal); font-family: 'Orbitron', monospace; font-size: 13px; letter-spacing: 2px; }
    .tinp { display: flex; align-items: center; gap: 14px; flex-wrap: wrap; margin-bottom: 18px; }
    .tf   { display: flex; flex-direction: column; gap: 5px; }
    .tf label { font-size: 10px; letter-spacing: 2px; color: var(--mut); text-transform: uppercase; }
    .tf input[type=time] { background: rgba(0,0,0,0.35); border: 1px solid rgba(0,229,204,0.25);
      border-radius: 10px; color: var(--teal); font-family: 'Orbitron', monospace; font-size: 20px;
      padding: 9px 14px; outline: none; cursor: pointer; transition: border-color 0.3s; letter-spacing: 2px; width: 138px; }
    .tf input[type=time]:focus { border-color: rgba(0,229,204,0.6); }
    .tf input[type=time]::-webkit-calendar-picker-indicator { filter: invert(1) opacity(0.4); cursor: pointer; }
    .tarr { font-size: 20px; color: var(--mut); margin-top: 18px; }
    .tbr  { display: flex; gap: 10px; flex-wrap: wrap; }
    .bconf { display: flex; align-items: center; gap: 7px; padding: 9px 22px; border-radius: 10px;
      border: 1px solid rgba(0,230,118,0.4); background: rgba(0,230,118,0.12); color: var(--green);
      font-family: 'Orbitron', monospace; font-size: 11px; letter-spacing: 2px; cursor: pointer; transition: all 0.25s; }
    .bconf:hover { background: rgba(0,230,118,0.22); box-shadow: 0 0 12px rgba(0,230,118,0.3); }
    .bconf:active { transform: scale(0.97); }
    .brst { display: flex; align-items: center; gap: 7px; padding: 9px 22px; border-radius: 10px;
      border: 1px solid rgba(255,23,68,0.3); background: rgba(255,23,68,0.08); color: var(--red);
      font-family: 'Orbitron', monospace; font-size: 11px; letter-spacing: 2px; cursor: pointer; transition: all 0.25s; }
    .brst:hover { background: rgba(255,23,68,0.18); }
    .brst:active { transform: scale(0.97); }
    .toast { position: fixed; bottom: 28px; left: 50%; transform: translateX(-50%) translateY(80px);
      background: rgba(10,30,55,0.97); border: 1px solid rgba(0,229,204,0.3);
      border-radius: 12px; padding: 11px 22px; font-size: 12px; color: var(--teal);
      letter-spacing: 1px; z-index: 999; transition: transform 0.4s cubic-bezier(0.34,1.56,0.64,1); white-space: nowrap; }
    .toast.show { transform: translateX(-50%) translateY(0); }
    .toast.ok { border-color: rgba(0,230,118,0.4); color: var(--green); }
    .toast.er { border-color: rgba(255,23,68,0.4); color: var(--red); }
    .mgrid { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 16px; }
    .mc { background: var(--card); border: 1px solid var(--bdr); border-radius: 16px; padding: 22px 24px;
      position: relative; overflow: hidden; transition: transform 0.3s, border-color 0.3s; }
    .mc::before { content: ''; position: absolute; top: 0; left: 0; right: 0; height: 2px;
      background: linear-gradient(90deg, var(--teal), var(--sun)); opacity: 0; transition: opacity 0.3s; }
    .mc:hover { transform: translateY(-3px); border-color: rgba(0,229,204,0.4); }
    .mc:hover::before { opacity: 1; }
    .mh { display: flex; align-items: center; gap: 10px; margin-bottom: 14px; }
    .mi { width: 38px; height: 38px; border-radius: 10px; display: flex; align-items: center; justify-content: center; font-size: 16px; flex-shrink: 0; }
    .it { background: rgba(0,229,204,0.12); color: var(--teal); }
    .is { background: rgba(255,179,0,0.12); color: var(--sun); }
    .ig { background: rgba(0,230,118,0.12); color: var(--green); }
    .ib { background: rgba(33,150,243,0.12); color: #42A5F5; }
    .io { background: rgba(255,109,0,0.12); color: var(--orange); }
    .ml { font-size: 11px; letter-spacing: 1px; text-transform: uppercase; color: var(--mut); font-weight: 600; }
    .mv { font-family: 'Orbitron', monospace; font-size: clamp(30px,4.5vw,44px); font-weight: 700;
      line-height: 1; color: #fff; display: flex; align-items: baseline; gap: 5px; }
    .mu { font-size: 14px; color: var(--mut); font-family: 'Exo 2', sans-serif; font-weight: 400; }
    .mc.bc .mv { color: var(--green); }
    .mc.pc .mv { color: var(--sun); }
    .mc.acc .mv{ color: var(--orange); }
    .btrack { margin-top: 14px; height: 7px; border-radius: 8px; background: rgba(255,255,255,0.06); overflow: hidden; }
    .bfill  { height: 100%; border-radius: 8px;
      background: linear-gradient(90deg, #F44336, #FFC107 40%, var(--green));
      transition: width 1.2s cubic-bezier(0.4,0,0.2,1); }
    .blbls { display: flex; justify-content: space-between; font-size: 10px; color: var(--mut); margin-top: 5px; }
    .rgrid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 16px; }
    .rc { background: var(--card); border: 1px solid var(--bdr); border-radius: 16px; padding: 22px;
      display: flex; flex-direction: column; gap: 16px; transition: border-color 0.3s; }
    .rc.ron  { border-color: rgba(0,230,118,0.4); }
    .rc.roff { border-color: rgba(255,23,68,0.25); }
    .rh { display: flex; align-items: flex-start; justify-content: space-between; }
    .rname { font-family: 'Orbitron', monospace; font-size: 12px; letter-spacing: 1.5px; color: var(--txt); font-weight: 700; }
    .rsub  { font-size: 9px; color: var(--sun); letter-spacing: 0.5px; margin-top: 3px; }
    .rbdg  { padding: 3px 10px; border-radius: 20px; font-size: 10px; font-weight: 700; letter-spacing: 2px; transition: all 0.4s; }
    .bon  { background: rgba(0,230,118,0.15); color: var(--green); border: 1px solid rgba(0,230,118,0.4); }
    .boff { background: rgba(255,23,68,0.12);  color: var(--red);   border: 1px solid rgba(255,23,68,0.3); }
    .trow { display: flex; align-items: center; justify-content: center; }
    .tw { position: relative; display: inline-block; width: 62px; height: 30px; }
    .tw input { opacity: 0; width: 0; height: 0; }
    .ts { position: absolute; inset: 0; cursor: pointer; border-radius: 30px; transition: 0.4s;
      background: rgba(255,23,68,0.25); border: 1px solid rgba(255,23,68,0.5); }
    .ts::before { content: ''; position: absolute; width: 22px; height: 22px; left: 3px; top: 3px;
      border-radius: 50%; background: var(--red); transition: 0.4s; box-shadow: 0 0 8px rgba(255,23,68,0.6); }
    .tw input:checked + .ts { background: rgba(0,230,118,0.2); border-color: rgba(0,230,118,0.5); }
    .tw input:checked + .ts::before { transform: translateX(31px); background: var(--green); box-shadow: 0 0 12px rgba(0,230,118,0.7); }
    .pzlbl { font-size: 10px; letter-spacing: 0.5px; color: var(--orange); text-align: center; margin-top: -6px; }
    .modebdg { font-size: 9px; letter-spacing: 1px; padding: 2px 9px; border-radius: 10px; font-weight: 700; }
    .modebdg.auto   { background: rgba(0,229,204,0.12); color: var(--teal); border: 1px solid rgba(0,229,204,0.3); }
    .modebdg.manual { background: rgba(255,179,0,0.12); color: var(--sun); border: 1px solid rgba(255,179,0,0.35); }
    .autobtn { display: flex; align-items: center; justify-content: center; gap: 5px; width: 100%;
      padding: 7px 10px; border-radius: 9px; font-size: 10px; letter-spacing: 1px; font-weight: 700;
      background: rgba(0,229,204,0.1); color: var(--teal); border: 1px solid rgba(0,229,204,0.35);
      cursor: pointer; transition: all 0.25s; }
    .autobtn:hover:not(:disabled) { background: rgba(0,229,204,0.2); box-shadow: 0 0 10px rgba(0,229,204,0.25); }
    .autobtn:active:not(:disabled) { transform: scale(0.97); }
    .autobtn:disabled { opacity: 0.35; cursor: default; }
    footer { margin-top: 52px; padding: 26px 0; border-top: 1px solid var(--bdr); text-align: center; }
    .flogo { font-family: 'Orbitron', monospace; font-size: 12px; letter-spacing: 3px; color: var(--mut); margin-bottom: 6px; }
    .fsince { font-size: 11px; letter-spacing: 2px; color: rgba(120,144,156,0.6); }
    .fsince span { color: var(--teal); font-weight: 700; }
    @media (max-width: 600px) {
      .mgrid, .rgrid { grid-template-columns: 1fr; }
      .banner, .acb  { flex-direction: column; text-align: center; }
      .tinp { flex-direction: column; align-items: flex-start; }
      .tarr { display: none; }
      .tf input[type=time] { font-size: 18px; width: 128px; }
    }
    /* ===== ADMIN PANEL ===== */
    .adm-wrap { max-width: 700px; margin: 20px auto; background: var(--bg2); border: 1px solid var(--bdr);
      border-radius: 16px; padding: 24px 20px; }
    .adm-title { font-family:'Orbitron',monospace; font-size:13px; letter-spacing:2px; color:var(--teal);
      text-align:center; margin-bottom:18px; }
    .adm-tabs { display:flex; gap:6px; flex-wrap:wrap; margin-bottom:18px; justify-content:center; }
    .adm-tab { background:rgba(0,229,204,0.06); border:1px solid var(--bdr); border-radius:10px;
      color:var(--mut); font-family:'Orbitron',monospace; font-size:9px; letter-spacing:1px;
      padding:7px 12px; cursor:pointer; transition:all 0.2s; }
    .adm-tab.active,.adm-tab:hover { background:rgba(0,229,204,0.14); border-color:rgba(0,229,204,0.4); color:var(--teal); }
    .adm-pane { animation: fi 0.25s; }
    @keyframes fi { from{opacity:0;transform:translateY(6px)} to{opacity:1;transform:none} }
    .adm-grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(150px,1fr)); gap:10px; margin-bottom:14px; }
    .adm-card { background:rgba(0,0,0,0.25); border:1px solid var(--bdr); border-radius:10px; padding:12px; text-align:center; }
    .adm-lbl { font-size:9px; color:var(--mut); letter-spacing:1px; margin-bottom:5px; }
    .adm-val { font-family:'Orbitron',monospace; font-size:12px; color:var(--teal); }
    .adm-btn { background:rgba(0,229,204,0.1); border:1px solid rgba(0,229,204,0.3); border-radius:10px;
      color:var(--teal); font-family:'Orbitron',monospace; font-size:10px; letter-spacing:1px;
      padding:9px 18px; cursor:pointer; transition:all 0.2s; }
    .adm-btn:hover { background:rgba(0,229,204,0.2); }
    .adm-btn-save { background:rgba(0,230,118,0.1); border-color:rgba(0,230,118,0.3); color:var(--green); }
    .adm-btn-save:hover { background:rgba(0,230,118,0.2); }
    .adm-form { display:flex; flex-direction:column; gap:10px; max-width:340px; }
    .adm-form input { background:rgba(0,0,0,0.3); border:1px solid rgba(0,229,204,0.2); border-radius:10px;
      color:var(--txt); font-size:13px; padding:10px 13px; outline:none; transition:border-color 0.3s; }
    .adm-form input:focus { border-color:rgba(0,229,204,0.5); }
    .adm-sub { font-size:10px; letter-spacing:1.5px; color:var(--teal); margin-bottom:10px; }
    .cal-table { width:100%; border-collapse:collapse; margin-bottom:6px; }
    .cal-table th { font-size:9px; color:var(--mut); letter-spacing:1px; padding:6px 8px; text-align:left;
      border-bottom:1px solid var(--bdr); }
    .cal-table td { padding:5px 6px; }
    .cal-in { background:rgba(0,0,0,0.3); border:1px solid rgba(0,229,204,0.2); border-radius:8px;
      color:var(--txt); font-size:12px; padding:7px 10px; outline:none; width:100%; transition:border-color 0.2s; }
    .cal-in:focus { border-color:rgba(0,229,204,0.5); }
    .adm-msg { font-size:11px; margin-top:10px; min-height:14px; }
    .adm-msg.ok { color:var(--green); } .adm-msg.er { color:var(--red); }
    .cp-lockbox { max-width: 300px; margin: 6px auto 4px; display: flex; flex-direction: column; align-items: center; text-align: center; gap: 10px; }
    .cp-lockicon { font-size: 30px; opacity: 0.85; }
    .cp-locktxt { font-size: 11px; color: var(--mut); line-height: 1.5; }
    .cp-lockbox input[type=password] { width: 100%; background: rgba(0,0,0,0.3);
      border: 1px solid rgba(0,229,204,0.25); border-radius: 10px; color: var(--txt);
      font-size: 14px; padding: 11px 14px; outline: none; transition: border-color 0.3s; text-align: center; }
    .cp-lockbox input[type=password]:focus { border-color: rgba(0,229,204,0.6); }
    .cp-lockbox .adm-btn { width: 100%; }
    .log-row { display:flex; gap:10px; align-items:center; padding:8px 10px; border-radius:8px;
      background:rgba(0,0,0,0.2); margin-bottom:5px; font-size:11px; flex-wrap:wrap; }
    .log-ip { color:var(--teal); font-family:'Orbitron',monospace; font-size:10px; min-width:110px; }
    .log-time { color:var(--mut); flex:1; }
    .log-ok { padding:2px 8px; border-radius:6px; font-size:9px; letter-spacing:1px; }
    .log-ok.y { background:rgba(0,230,118,0.15); color:var(--green); }
    .log-ok.n { background:rgba(255,23,68,0.12); color:var(--red); }
    /* ================= TAB LAYOUT (PILL BAR DI BAWAH DASBOR) ================= */
    .app-body { display: flex; flex-direction: column; gap: 18px; margin-top: 22px; }
    .side-tabs { position: sticky; top: 10px; z-index: 40; display: flex; flex-direction: row;
      align-items: center; gap: 4px; width: 100%;
      background: rgba(6,20,38,0.72); backdrop-filter: blur(14px); -webkit-backdrop-filter: blur(14px);
      border: 1px solid var(--bdr); border-radius: 999px; padding: 6px;
      box-shadow: 0 8px 28px rgba(0,0,0,0.35), inset 0 0 0 1px rgba(255,255,255,0.02); }
    .side-tab { display: flex; flex-direction: row; align-items: center; justify-content: center; gap: 7px;
      flex: 1 1 0; text-align: center; background: transparent; border: 1px solid transparent;
      border-radius: 999px; color: var(--mut); font-family: 'Orbitron', monospace; font-size: 9.5px;
      letter-spacing: 0.6px; padding: 10px 10px; cursor: pointer; transition: all 0.25s;
      position: relative; overflow: hidden; white-space: nowrap; }
    .side-tab .sti { font-size: 14px; flex-shrink: 0; line-height: 1; }
    .side-tab:hover { color: var(--teal); background: rgba(0,229,204,0.08); }
    .side-tab.active { color: #04141f; border-color: transparent;
      background: linear-gradient(135deg, var(--teal) 0%, var(--sun) 100%);
      box-shadow: 0 0 18px rgba(0,229,204,0.45), 0 0 10px rgba(255,179,0,0.3);
      font-weight: 700; }
    .side-tab .stlock { margin-left: 2px; font-size: 10px; opacity: 0.7; }
    .side-tab.active .stlock { opacity: 0.85; }
    .side-tab .stlbl { white-space: nowrap; }
    .tab-content { flex: 1 1 auto; min-width: 0; }
    .tab-pane { display: none; animation: tabin 0.3s ease; }
    .tab-pane.active { display: block; }
    @keyframes tabin { from{opacity:0; transform:translateY(8px);} to{opacity:1; transform:none;} }
    .tab-pane .slbl:first-of-type { margin-top: 4px; }

    .pagehead { margin-bottom: 18px; }
    .pagehead .ptitle { font-family:'Orbitron',monospace; font-size:15px; letter-spacing:2px; color:var(--teal);
      display:flex; align-items:center; gap:10px; }
    .pagehead .psub { font-size:11px; color:var(--mut); margin-top:5px; letter-spacing:0.5px; }

    /* Pill bar tetap horizontal & sticky di semua ukuran layar — di layar sempit
       ikon & label tetap tampil (label ditaruh di bawah ikon) supaya tiap tab
       tetap jelas keterangannya. Tinggi tab DIKUNCI sama rata (tidak mengikuti
       panjang teks) supaya bentuk pill-nya konsisten — tidak ada yang lonjong. */
    @media (max-width: 700px) {
      .side-tabs { padding: 5px; gap: 3px; }
      .side-tab { flex-direction: column; justify-content: center; padding: 6px 2px; gap: 3px;
        font-size: 7.3px; height: 54px; }
      .side-tab .sti { font-size: 16px; }
      .side-tab .stlbl { display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 2;
        overflow: hidden; white-space: normal; line-height: 1.1; max-width: 100%; }
      .side-tab .stlock { margin-left: 0; font-size: 7.5px; }
      .pagehead .ptitle { font-size: 13px; }
      .about-card { padding: 16px 16px; }
    }
    @media (max-width: 420px) {
      .side-tabs { padding: 4px; }
      .side-tab { padding: 5px 1px; height: 48px; font-size: 6.4px; }
      .side-tab .sti { font-size: 14px; }
    }

    /* ================= ABOUT TAB ================= */
    .about-card { background: var(--card); border: 1px solid var(--bdr); border-radius: 16px;
      padding: 20px 24px; margin-bottom: 16px; position: relative; overflow: hidden; }
    .about-card::before { content:''; position:absolute; top:0; left:0; right:0; height:2px;
      background: linear-gradient(90deg, var(--teal), var(--sun)); }
    .about-card h3 { font-family:'Orbitron',monospace; font-size:12px; letter-spacing:1.5px; color:var(--sun);
      margin-bottom:10px; display:flex; align-items:center; gap:8px; }
    .about-card p, .about-card li { font-size:12.5px; color:var(--txt); line-height:1.8; }
    .about-card ul { padding-left:18px; margin-top:6px; }
    .about-card b { color: var(--teal); }
    .about-grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(260px,1fr)); gap:14px; margin-bottom:16px; }
    .about-mini { background: rgba(0,0,0,0.2); border:1px solid rgba(255,255,255,0.06); border-radius:12px;
      padding:14px 16px; }
    .about-mini .am-t { font-family:'Orbitron',monospace; font-size:10px; letter-spacing:1px; color:var(--teal); margin-bottom:6px; }
    .about-mini .am-d { font-size:11.5px; color:var(--mut); line-height:1.6; }
    .creator-card { border-color: rgba(255,179,0,0.35); }
    .creator-card::before { background: linear-gradient(90deg, var(--teal), var(--sun), var(--teal)); }
    .creator-block { margin-top:14px; padding:16px 18px; border-radius:12px;
      background: linear-gradient(135deg, rgba(0,229,204,0.07), rgba(255,179,0,0.07));
      border: 1px solid var(--bdr); }
    .creator-name { font-family:'Cormorant Garamond', serif; font-style:italic; font-weight:700;
      font-size:22px; letter-spacing:0.5px; color:var(--txt);
      background: linear-gradient(135deg, var(--sun) 0%, #FFF176 45%, var(--teal) 100%);
      -webkit-background-clip:text; -webkit-text-fill-color:transparent; background-clip:text; }
    .creator-meta { display:flex; flex-wrap:wrap; gap:8px 20px; margin-top:10px; }
    .creator-meta span { font-size:11.5px; color:var(--mut); letter-spacing:0.3px; }
    .creator-meta b { color:var(--teal); font-family:'Orbitron',monospace; font-size:10px; letter-spacing:1px; text-transform:uppercase; margin-right:4px; }
    .creator-contact { margin-top:16px; display:inline-flex; align-items:center; gap:12px;
      padding:12px 18px; border-radius:12px; text-decoration:none; cursor:pointer;
      background: rgba(0,229,204,0.08); border:1px solid var(--bdr); transition: all 0.25s; }
    .creator-contact:hover { background: rgba(0,229,204,0.16); border-color: rgba(0,229,204,0.5);
      transform: translateY(-2px); box-shadow: 0 6px 18px rgba(0,229,204,0.15); }
    .creator-contact .cc-ico { font-size:20px; color:var(--sun); flex-shrink:0; }
    .creator-contact .cc-lbl { display:block; font-family:'Orbitron',monospace; font-size:9px;
      letter-spacing:1.5px; color:var(--mut); text-transform:uppercase; margin-bottom:3px; }
    .creator-contact .cc-mail { display:block; font-size:13px; color:var(--teal); letter-spacing:0.3px; font-weight:600; }
  </style>
)rawliteral";
    server.sendContent(html1);
  }
  {
    String html2 = R"rawliteral(<body>
  <div class="bgl"></div>
  <div class="bgg"></div>
  <div class="toast" id="toast"></div>

  <div class="modal-overlay" id="loginModal">
    <div class="modal-box">
      <div class="modal-title">&#128274; ADMIN LOGIN</div>
      <input type="password" id="adminPass" placeholder="Masukkan password admin" autocomplete="off">
      <div class="modal-btns">
        <button class="modal-submit" onclick="doLogin()">MASUK</button>
        <button class="modal-cancel" onclick="closeLoginModal()">BATAL</button>
      </div>
      <div class="modal-err" id="loginErr"></div>
      <div class="lock-note">Login diperlukan untuk kontrol relay &amp; timer</div>
    </div>
  </div>

  <div class="wrap">
    <header>
      <button class="admin-btn" id="adminBtn" onclick="onAdminBtnClick()">&#128274; LOGIN</button>
      
      <div class="hicon">&#9728;</div>
      <div class="htitle">WEB MONITOR PLTS</div>
      <div class="hsub">Sistem Monitoring Panel Tenaga Surya &mdash; Real-Time</div>
      <div><span class="lbdg"><span class="ldot"></span>LIVE MONITORING</span></div>
      <div class="clkval" id="clk">--:--:--</div>
    </header>
    <div class="app-body">
      <nav class="side-tabs">
        <button class="side-tab active" id="navtab-monitoring" onclick="switchTab('monitoring')" title="MONITORING"><span class="sti"><svg viewBox="0 0 24 24" width="1em" height="1em" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:-0.15em"><rect x="3" y="4" width="18" height="12" rx="2"></rect><path d="M8 20h8M12 16v4"></path></svg></span><span class="stlbl">MONITORING</span></button>
        <button class="side-tab" id="navtab-kontrol" onclick="switchTab('kontrol')" title="KONTROL"><span class="sti">&#127899;</span><span class="stlbl">KONTROL</span> <span class="stlock"><svg viewBox="0 0 24 24" width="1em" height="1em" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:-0.15em"><rect x="5" y="11" width="14" height="9" rx="2"></rect><path d="M8 11V7a4 4 0 0 1 8 0v4"></path></svg></span></button>
        <button class="side-tab" id="navtab-admin" onclick="switchTab('admin')" title="CONTROL PANEL"><span class="sti"><svg viewBox="0 0 24 24" width="1em" height="1em" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:-0.15em"><circle cx="12" cy="12" r="3.2"></circle><path d="M12 3v2.5M12 18.5V21M21 12h-2.5M5.5 12H3M18.4 5.6l-1.8 1.8M7.4 16.6l-1.8 1.8M18.4 18.4l-1.8-1.8M7.4 7.4L5.6 5.6"></path></svg></span><span class="stlbl">CONTROL PANEL</span> <span class="stlock"><svg viewBox="0 0 24 24" width="1em" height="1em" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:-0.15em"><rect x="5" y="11" width="14" height="9" rx="2"></rect><path d="M8 11V7a4 4 0 0 1 8 0v4"></path></svg></span></button>
        <button class="side-tab" id="navtab-about" onclick="switchTab('about')" title="TENTANG WEB"><span class="sti"><svg viewBox="0 0 24 24" width="1em" height="1em" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:-0.15em"><circle cx="12" cy="12" r="9"></circle><path d="M12 16v-4.5M12 8h.01"></path></svg></span><span class="stlbl">TENTANG WEB</span></button>
      </nav>

      <div class="tab-content">
        <section class="tab-pane active" id="tab-monitoring">
          <div class="pagehead">
            <div class="ptitle">&#128225; MONITORING</div>
            <div class="psub">Data sensor baterai, panel surya &amp; PLN secara real-time</div>
          </div>

    <div class="acb ok" id="acBanner">
      <div style="font-size:18px;flex-shrink:0">&#9889;</div>
      <div id="acBTxt"><strong>Menunggu data PZEM...</strong></div>
    </div>

    <div class="slbl">&#9670; Data Sensor DC (Baterai)</div>
    <div class="mgrid">
      <div class="mc">
        <div class="mh"><div class="mi it">&#9889;</div><div class="ml">Tegangan Baterai</div></div>
        <div class="mv"><span id="v">--</span><span class="mu">V</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi ib">&#9195;</div><div class="ml">Arus Baterai</div></div>
        <div class="mv"><span id="ic">--</span><span class="mu">A</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi is">&#9889;</div><div class="ml">Daya Baterai</div></div>
        <div class="mv"><span id="pw">--</span><span class="mu">W</span></div>
      </div>
      <div class="mc bc">
        <div class="mh"><div class="mi ig">&#128267;</div><div class="ml">Level Baterai</div></div>
        <div class="mv"><span id="bt">--</span><span class="mu">%</span></div>
        <div class="btrack"><div class="bfill" id="bf" style="width:0%"></div></div>
        <div class="blbls"><span>0%</span><span>50%</span><span>100%</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi it">&#9196;</div><div class="ml">Energi Hari Ini (Baterai)</div></div>
        <div class="mv"><span id="en">--</span><span class="mu">Wh</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi ig">&#128267;</div><div class="ml">Kapasitas Baterai (30Ah)</div></div>
        <div class="mv"><span id="bwh">--</span><span class="mu">Wh</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi ig">&#9203;</div><div class="ml">Estimasi Sisa Baterai</div></div>
        <div class="mv"><span id="best" style="font-size:19px">--</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi ib">&#128267;</div><div class="ml">Ah Terpakai (Discharge)</div></div>
        <div class="mv"><span id="dah">--</span><span class="mu">Ah</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi ig">&#9889;</div><div class="ml">Ah Terisi (Charge)</div></div>
        <div class="mv"><span id="cah">--</span><span class="mu">Ah</span></div>
      </div>
    </div>
    <div style="font-size:11px;color:var(--mut);margin:10px 2px 4px;line-height:1.6">
      &#128161; <b style="color:var(--sun)">Cara cek kapasitas ASLI baterai (bukan cuma percaya label):</b>
      charge baterai sampai benar-benar 100%, lalu tekan tombol <b>RESET fisik</b> di board ESP32
      (bukan dari web) supaya angka Ah di atas mulai dari 0, lalu pakai baterai seperti biasa sampai
      benar-benar habis. Angka <b>"Ah Terpakai (Discharge)"</b> pada saat itu adalah kapasitas asli
      baterai kamu &mdash; bandingkan dengan label yang tertulis di baterainya.
    </div>

    <div class="slbl">&#9670; Data Sensor DC (Panel Surya)</div>
    <div class="mgrid">
      <div class="mc pc">
        <div class="mh"><div class="mi is">&#9728;</div><div class="ml">Tegangan Panel Surya</div></div>
        <div class="mv"><span id="vp">--</span><span class="mu">V</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi ib">&#9195;</div><div class="ml">Arus Panel Surya</div></div>
        <div class="mv"><span id="pi">--</span><span class="mu">A</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi is">&#9889;</div><div class="ml">Daya Panel Surya</div></div>
        <div class="mv"><span id="pp">--</span><span class="mu">W</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi it">&#9196;</div><div class="ml">Energi Hari Ini (Panel)</div></div>
        <div class="mv"><span id="pe">--</span><span class="mu">Wh</span></div>
      </div>
    </div>

    <div class="slbl">&#9670; Data Sensor AC (PZEM-004T)</div>
    <div class="mgrid">
      <div class="mc acc">
        <div class="mh"><div class="mi io">&#9889;</div><div class="ml">Tegangan AC</div></div>
        <div class="mv"><span id="acv">--</span><span class="mu">V</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi ib">&#9195;</div><div class="ml">Arus AC</div></div>
        <div class="mv"><span id="aci">--</span><span class="mu">A</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi is">&#9889;</div><div class="ml">Daya AC</div></div>
        <div class="mv"><span id="acp">--</span><span class="mu">W</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi it">~</div><div class="ml">Frekuensi</div></div>
        <div class="mv"><span id="acf">--</span><span class="mu">Hz</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi ig">&#10003;</div><div class="ml">Power Factor</div></div>
        <div class="mv"><span id="acpf">--</span></div>
      </div>
      <div class="mc">
        <div class="mh"><div class="mi io">&#9889;</div><div class="ml">Energi (Energy / kWh)</div></div>
        <div class="mv"><span id="ace">--</span><span class="mu">kWh</span></div>
        <div class="mu" style="margin-top:4px;">&#8776; <span id="acrp">--</span></div>
      </div>
    </div>

        </section>

)rawliteral";
    server.sendContent(html2);
  }
  {
    String html3 = R"rawliteral(        <section class="tab-pane" id="tab-kontrol">
          <div class="pagehead">
            <div class="ptitle">&#127899; KONTROL</div>
            <div class="psub">Atur timer otomatis &amp; nyala/matikan relay &mdash; perlu login admin</div>
          </div>

    <div id="budgetBanner" style="display:none;background:rgba(255,77,77,0.15);border:1px solid rgba(255,77,77,0.5);border-radius:10px;padding:10px 14px;margin-bottom:14px;color:#ff4d4d;font-size:13px;text-align:center;font-weight:600;">
      &#128683; LIMIT BIAYA TERCAPAI &mdash; Semua relay dikunci OFF. Naikkan limit di Control Panel untuk membuka kembali.
    </div>
    <div id="currentBanner" style="display:none;background:rgba(255,77,77,0.15);border:1px solid rgba(255,77,77,0.5);border-radius:10px;padding:10px 14px;margin-bottom:14px;color:#ff4d4d;font-size:13px;text-align:center;font-weight:600;">
      &#9888; ARUS AC MELEBIHI BATAS &mdash; Semua relay TRIP OFF. Tekan RESET TRIP ARUS di Control Panel untuk menyalakan kembali.
    </div>
    <div class="slbl">&#9670; Pengaturan Timer Auto &mdash; Output AC &amp; Lampu Utama</div>
    <div class="tcard">
      <div class="thead">
        <div class="thl">
          <div class="tico">&#9200;</div>
          <div class="tlbl">JADWAL OTOMATIS
            <small>Output AC &amp; Lampu Utama ON/OFF sesuai waktu ini</small>
          </div>
        </div>
        <div class="tbdg" id="tbdg">AKTIF</div>
      </div>
      <div class="tarow">
        <span class="tv">&#9200;</span>
        <span style="color:var(--mut);font-size:11px">Timer aktif:</span>
        <span class="tv" id="tad">18:00</span>
        <span style="color:var(--mut);font-size:11px">sampai</span>
        <span class="tv" id="tae">21:00</span>
        <span style="color:var(--mut);font-size:10px">(Output AC + Lampu Utama ON)</span>
      </div>
      <div class="tinp">
        <div class="tf">
          <label>Jam Mulai</label>
          <input type="time" id="ts" value="18:00">
        </div>
        <div class="tarr">&#8594;</div>
        <div class="tf">
          <label>Jam Selesai</label>
          <input type="time" id="te" value="21:00">
        </div>
      </div>
      <div class="tbr">
        <button class="bconf" onclick="cfmTimer()">&#10003; &nbsp;CONFIRM</button>
        <button class="brst"  onclick="rstTimer()">&#8634; &nbsp;RESET DEFAULT</button>
      </div>
    </div>

    <div class="slbl">&#9670; Kontrol Relay</div>
    <div class="rgrid">
      <div class="rc roff" id="cr1">
        <div class="rh">
          <div>
            <div class="rname">INVERTER</div>
            <div class="rsub">[MANUAL + PZEM]</div>
          </div>
          <div class="rbdg boff" id="r1">MATI</div>
        </div>
        <div class="trow">
          <label class="tw">
            <input type="checkbox" id="t1" onclick="return onToggleClick(event,1)">
            <span class="ts"></span>
          </label>
        </div>
        <div class="pzlbl">&#9889; PZEM: PLN tidak ada &rarr; ON &nbsp;|&nbsp; PLN ada &rarr; OFF</div>
        <div style="display:flex;align-items:center;justify-content:center">
          <span class="modebdg auto" id="m1">AUTO</span>
        </div>
        <button class="autobtn" id="ab1" onclick="setAuto(1)">&#8635; KEMBALI KE AUTO</button>
      </div>
      <div class="rc roff" id="cr2">
        <div class="rh">
          <div>
            <div class="rname">LAMPU UTAMA</div>
            <div class="rsub">[MANUAL | TIMER + PZEM]</div>
          </div>
          <div class="rbdg boff" id="r2">MATI</div>
        </div>
        <div class="trow">
          <label class="tw">
            <input type="checkbox" id="t2" onclick="return onToggleClick(event,2)">
            <span class="ts"></span>
          </label>
        </div>
        <div class="pzlbl">&#9728; Timer aktif &rarr; ON &nbsp;|&nbsp; &#9889; PLN tidak ada &rarr; ON</div>
        <div style="display:flex;align-items:center;justify-content:center">
          <span class="modebdg auto" id="m2">AUTO</span>
        </div>
        <button class="autobtn" id="ab2" onclick="setAuto(2)">&#8635; KEMBALI KE AUTO</button>
      </div>
      <div class="rc roff" id="cr3">
        <div class="rh">
          <div>
            <div class="rname">OUTPUT AC</div>
            <div class="rsub">[MANUAL + AUTO TIMER]</div>
          </div>
          <div class="rbdg boff" id="r3">MATI</div>
        </div>
        <div class="trow">
          <label class="tw">
            <input type="checkbox" id="t3" onclick="return onToggleClick(event,3)">
            <span class="ts"></span>
          </label>
        </div>
        <div class="pzlbl">&#9728; ON selama jam timer aktif</div>
        <div style="display:flex;align-items:center;justify-content:center">
          <span class="modebdg auto" id="m3">AUTO</span>
        </div>
        <button class="autobtn" id="ab3" onclick="setAuto(3)">&#8635; KEMBALI KE AUTO</button>
      </div>
      <div class="rc roff" id="cr4">
        <div class="rh">
          <div>
            <div class="rname">LAMPU PANEL</div>
            <div class="rsub">[MANUAL SESAAT + PZEM]</div>
          </div>
          <div class="rbdg boff" id="r4">MATI</div>
        </div>
        <div class="trow">
          <label class="tw">
            <input type="checkbox" id="t4" onclick="return onToggleClick(event,4)">
            <span class="ts"></span>
          </label>
        </div>
        <div class="pzlbl">&#9889; PLN 0V &rarr; ON &nbsp;|&nbsp; PLN &ge;50V &rarr; OFF</div>
        <div style="display:flex;align-items:center;justify-content:center">
          <span class="modebdg auto" id="m4">AUTO</span>
        </div>
        <div style="font-size:9px;color:var(--mut);text-align:center;letter-spacing:0.5px">
          PZEM selalu berlaku &mdash; manual otomatis kembali &plusmn;1 detik
        </div>
      </div>
    </div>

        </section>

)rawliteral";
    server.sendContent(html3);
  }
  {
    String html4 = R"rawliteral(        <section class="tab-pane" id="tab-admin">
          <div class="pagehead">
            <div class="ptitle">&#9881; CONTROL PANEL ADMIN</div>
            <div class="psub">Status sistem, kalibrasi, limit biaya, batas arus &amp; log akses &mdash; punya password sendiri</div>
          </div>

    <div id="adminPanel">
      <div class="adm-wrap">
        <div class="adm-title">&#9881; CONTROL PANEL ADMIN</div>

        <!-- TAMPILAN TERKUNCI -->
        <div id="cpLocked">
          <div class="cp-lockbox">
            <div class="cp-lockicon">&#128274;</div>
            <div class="cp-locktxt">Panel ini terkunci. Masukkan password <b>Control Panel</b> (BEDA dari password tombol LOGIN di header) untuk membukanya.</div>
            <input type="password" id="cpPass" placeholder="Password Control Panel" autocomplete="off">
            <button class="adm-btn adm-btn-save" id="cpLoginBtn" onclick="cpLogin()">&#128274; BUKA CONTROL PANEL</button>
            <div class="adm-msg er" id="cpLoginErr"></div>
          </div>
        </div>

        <!-- TAMPILAN TERBUKA -->
        <div id="cpUnlocked" style="display:none">
        <div class="adm-tabs">
          <button class="adm-tab active" onclick="admTab(this,'admStatus')">&#128268; STATUS</button>
          <button class="adm-tab" onclick="admTab(this,'admPass')">&#128274; PASSWORD</button>
          <button class="adm-tab" onclick="admTab(this,'admCal')">&#128200; KALIBRASI</button>
          <button class="adm-tab" onclick="admTab(this,'admBudget')">&#128176; LIMIT BIAYA</button>
          <button class="adm-tab" onclick="admTab(this,'admCurrent')">&#9889; MAX ARUS</button>
          <button class="adm-tab" onclick="admTab(this,'admLog')">&#128196; LOG AKSES</button>
        </div>
        <div class="adm-pane" id="admStatus">
          <div class="adm-grid">
            <div class="adm-card"><div class="adm-lbl">UPTIME</div><div class="adm-val" id="aUptime">--</div></div>
            <div class="adm-card"><div class="adm-lbl">FREE HEAP</div><div class="adm-val" id="aHeap">--</div></div>
            <div class="adm-card"><div class="adm-lbl">WiFi RSSI</div><div class="adm-val" id="aRssi">--</div></div>
            <div class="adm-card"><div class="adm-lbl">IP LOKAL</div><div class="adm-val" id="aIp">--</div></div>
            <div class="adm-card"><div class="adm-lbl">IP AP</div><div class="adm-val" id="aApIp">--</div></div>
            <div class="adm-card"><div class="adm-lbl">PW LOGIN</div><div class="adm-val" id="aCurPass">--</div></div>
            <div class="adm-card"><div class="adm-lbl">PW PANEL</div><div class="adm-val" id="aCurCPPass">--</div></div>
          </div>
          <button class="adm-btn" onclick="loadAdmStatus()">&#8635; REFRESH</button>
        </div>
        <div class="adm-pane" id="admPass" style="display:none">
          <div class="adm-form">
            <div class="adm-sub">Password LOGIN (tombol header, kontrol relay &amp; timer)</div>
            <label class="adm-lbl">Password Baru</label>
            <input type="password" id="newPass1" placeholder="Password baru (min 4 karakter)">
            <label class="adm-lbl">Konfirmasi</label>
            <input type="password" id="newPass2" placeholder="Ulangi password baru">
            <button class="adm-btn" onclick="doSetPassword()">&#10003; SIMPAN PASSWORD LOGIN</button>
            <div class="adm-msg" id="passMsg"></div>
          </div>
          <div class="adm-form" style="margin-top:20px">
            <div class="adm-sub">Password CONTROL PANEL (panel ini sendiri)</div>
            <label class="adm-lbl">Password Baru</label>
            <input type="password" id="newCPPass1" placeholder="Password baru (min 4 karakter)">
            <label class="adm-lbl">Konfirmasi</label>
            <input type="password" id="newCPPass2" placeholder="Ulangi password baru">
            <button class="adm-btn" onclick="doSetCPPassword()">&#10003; SIMPAN PASSWORD PANEL</button>
            <div class="adm-msg" id="cpPassMsg"></div>
          </div>
        </div>
        <div class="adm-pane" id="admCal" style="display:none">
          <div class="adm-sub">Baterai — 17 Titik Lookup Table</div>
          <table class="cal-table">
            <tr><th>#</th><th>Volt Sensor (web)</th><th>Volt Asli (avo)</th></tr>
            <tr><td>1</td><td><input class="cal-in" id="w0" type="number" step="0.001"></td><td><input class="cal-in" id="a0" type="number" step="0.001"></td></tr>
            <tr><td>2</td><td><input class="cal-in" id="w1" type="number" step="0.001"></td><td><input class="cal-in" id="a1" type="number" step="0.001"></td></tr>
            <tr><td>3</td><td><input class="cal-in" id="w2" type="number" step="0.001"></td><td><input class="cal-in" id="a2" type="number" step="0.001"></td></tr>
            <tr><td>4</td><td><input class="cal-in" id="w3" type="number" step="0.001"></td><td><input class="cal-in" id="a3" type="number" step="0.001"></td></tr>
            <tr><td>5</td><td><input class="cal-in" id="w4" type="number" step="0.001"></td><td><input class="cal-in" id="a4" type="number" step="0.001"></td></tr>
            <tr><td>6</td><td><input class="cal-in" id="w5" type="number" step="0.001"></td><td><input class="cal-in" id="a5" type="number" step="0.001"></td></tr>
            <tr><td>7</td><td><input class="cal-in" id="w6" type="number" step="0.001"></td><td><input class="cal-in" id="a6" type="number" step="0.001"></td></tr>
            <tr><td>8</td><td><input class="cal-in" id="w7" type="number" step="0.001"></td><td><input class="cal-in" id="a7" type="number" step="0.001"></td></tr>
            <tr><td>9</td><td><input class="cal-in" id="w8" type="number" step="0.001"></td><td><input class="cal-in" id="a8" type="number" step="0.001"></td></tr>
            <tr><td>10</td><td><input class="cal-in" id="w9" type="number" step="0.001"></td><td><input class="cal-in" id="a9" type="number" step="0.001"></td></tr>
            <tr><td>11</td><td><input class="cal-in" id="w10" type="number" step="0.001"></td><td><input class="cal-in" id="a10" type="number" step="0.001"></td></tr>
            <tr><td>12</td><td><input class="cal-in" id="w11" type="number" step="0.001"></td><td><input class="cal-in" id="a11" type="number" step="0.001"></td></tr>
            <tr><td>13</td><td><input class="cal-in" id="w12" type="number" step="0.001"></td><td><input class="cal-in" id="a12" type="number" step="0.001"></td></tr>
            <tr><td>14</td><td><input class="cal-in" id="w13" type="number" step="0.001"></td><td><input class="cal-in" id="a13" type="number" step="0.001"></td></tr>
            <tr><td>15</td><td><input class="cal-in" id="w14" type="number" step="0.001"></td><td><input class="cal-in" id="a14" type="number" step="0.001"></td></tr>
            <tr><td>16</td><td><input class="cal-in" id="w15" type="number" step="0.001"></td><td><input class="cal-in" id="a15" type="number" step="0.001"></td></tr>
            <tr><td>17</td><td><input class="cal-in" id="w16" type="number" step="0.001"></td><td><input class="cal-in" id="a16" type="number" step="0.001"></td></tr>
          </table>
          <div class="adm-sub" style="margin-top:14px">Panel Surya</div>
          <div style="display:flex;align-items:center;gap:10px;margin-bottom:14px">
            <label class="adm-lbl" style="margin:0">Faktor Kalibrasi:</label>
            <input class="cal-in" id="panelCal" type="number" step="0.0001" style="width:110px">
          </div>
          <div style="display:flex;gap:10px">
            <button class="adm-btn" onclick="loadAdmCal()">&#8635; LOAD DARI ESP32</button>
            <button class="adm-btn adm-btn-save" onclick="doSetCal()">&#10003; SIMPAN</button>
          </div>
          <div class="adm-msg" id="calMsg"></div>
        </div>

        <!-- LIMIT BIAYA (RUPIAH) -->
        <div class="adm-pane" id="admBudget" style="display:none">
          <div class="adm-sub">Limit Biaya Listrik (Rupiah)</div>
          <div class="adm-grid" style="margin-bottom:14px">
            <div class="adm-card"><div class="adm-lbl">BIAYA SAAT INI</div><div class="adm-val" id="bRpNow">--</div></div>
            <div class="adm-card"><div class="adm-lbl">STATUS</div><div class="adm-val" id="bStatus">--</div></div>
          </div>
          <div class="adm-form">
            <label class="adm-lbl">Limit Rupiah (isi 0 untuk menonaktifkan limit)</label>
            <input type="number" id="rpLimitInput" placeholder="Contoh: 50000" step="1" min="0">
            <button class="adm-btn adm-btn-save" onclick="doSetBudget()">&#10003; SIMPAN LIMIT</button>
            <div class="adm-msg" id="budgetMsg"></div>
          </div>
          <div class="adm-sub" style="margin-top:14px;font-size:12px;color:var(--mut);line-height:1.6">
            Kalau biaya (Rupiah dari akumulasi kWh) sudah menyentuh atau melebihi limit ini,
            <b>SEMUA relay (1-4) otomatis dikunci OFF</b> -- baik status manual maupun auto,
            walau tombol AUTO ditekan tetap tidak akan menyala -- sampai limit ini dinaikkan
            lagi di atas biaya saat ini, atau di-set ke 0 untuk menonaktifkan limit sepenuhnya.
          </div>
        </div>

        <!-- MAX ARUS AC (PROTEKSI OVER-CURRENT / TRIP) -->
        <div class="adm-pane" id="admCurrent" style="display:none">
          <div class="adm-sub">Batas Maksimal Arus AC (Ampere)</div>
          <div class="adm-grid" style="margin-bottom:14px">
            <div class="adm-card"><div class="adm-lbl">ARUS SAAT INI</div><div class="adm-val" id="cAcNow">--</div></div>
            <div class="adm-card"><div class="adm-lbl">STATUS</div><div class="adm-val" id="cStatus">--</div></div>
          </div>
          <div class="adm-form">
            <label class="adm-lbl">Batas Arus Maksimal (Ampere, isi 0 untuk menonaktifkan)</label>
            <input type="number" id="maxCurrentInput" placeholder="Contoh: 4" step="0.1" min="0">
            <button class="adm-btn adm-btn-save" onclick="doSetMaxCurrent()">&#10003; SIMPAN BATAS</button>
            <div class="adm-msg" id="maxCurrentMsg"></div>
          </div>
          <button class="adm-btn" style="margin-top:14px;width:100%;background:rgba(255,77,77,0.15);border-color:rgba(255,77,77,0.5);color:#ff4d4d" onclick="doResetCurrentTrip()">&#8635; RESET TRIP ARUS</button>
          <div class="adm-msg" id="resetCurrentMsg"></div>
          <div class="adm-sub" style="margin-top:14px;font-size:12px;color:var(--mut);line-height:1.6">
            Kalau arus AC sudah menyentuh atau melebihi batas ini, <b>SEMUA relay (1-4)
            langsung TRIP OFF</b> -- baik status manual maupun auto, walau tombol AUTO
            ditekan tetap tidak akan menyala. <b>Berbeda dengan Limit Biaya</b>: relay akan
            <b>TETAP OFF</b> meskipun arus sudah turun kembali di bawah batas -- kamu harus
            menekan tombol <b>RESET TRIP ARUS</b> di atas secara manual untuk menyalakan
            relay kembali (mirip cara kerja MCB/circuit breaker).
          </div>
        </div>

        <div class="adm-pane" id="admLog" style="display:none">
          <button class="adm-btn" onclick="loadAdmLogs()" style="margin-bottom:12px">&#8635; REFRESH LOG</button>
          <div id="logTable"><div class="adm-lbl" style="text-align:center">Tekan REFRESH LOG untuk memuat</div></div>
        </div>
        <button class="adm-btn" style="margin-top:16px;width:100%" onclick="cpLogout()">&#128275; KUNCI CONTROL PANEL</button>
        </div>
      </div>
    </div>
        </section>

        <section class="tab-pane" id="tab-about">
          <div class="pagehead">
            <div class="ptitle">&#8505; TENTANG WEB INI</div>
            <div class="psub">Penjelasan singkat cara kerja Web Monitor PLTS</div>
          </div>

          <div class="about-card">
            <h3>&#9728; Apa Ini?</h3>
            <p>
              <b>WEB MONITOR PLTS</b> adalah panel monitoring &amp; kontrol untuk sistem Pembangkit Listrik
              Tenaga Surya (PLTS) berbasis ESP32. Web ini menampilkan data sensor baterai, panel surya,
              dan listrik PLN (AC) secara real-time, sekaligus memungkinkan kontrol 4 relay dari jarak jauh
              melalui koneksi WiFi/internet (tunnel).
            </p>
          </div>

          <div class="about-grid">
            <div class="about-mini">
              <div class="am-t">&#128225; TAB MONITORING</div>
              <div class="am-d">Menampilkan seluruh data sensor: tegangan &amp; arus baterai, tegangan panel surya,
              level baterai, energi harian, serta tegangan/arus/daya/frekuensi/energi listrik AC dari PZEM-004T.
              Semua data ini otomatis diperbarui setiap 2 detik &mdash; tidak perlu login untuk melihatnya.</div>
            </div>
            <div class="about-mini">
              <div class="am-t">&#127899; TAB KONTROL</div>
              <div class="am-d">Tempat mengatur jadwal timer otomatis dan menyalakan/mematikan 4 relay secara manual.
              Karena bisa mengubah kondisi perangkat fisik, tab ini <b>mewajibkan LOGIN admin</b> (tombol LOGIN
              di kanan atas header) sebelum tombol/toggle bisa digunakan.</div>
            </div>
            <div class="about-mini">
              <div class="am-t">&#9881; CONTROL PANEL ADMIN</div>
              <div class="am-d">Panel khusus admin untuk melihat status ESP32 (uptime, RAM, WiFi, IP), mengganti
              password, mengatur kalibrasi sensor, menetapkan limit biaya listrik, batas arus AC (proteksi
              overcurrent), serta melihat log akses login. Panel ini <b>punya password sendiri</b>, terpisah
              dari password LOGIN untuk kontrol relay.</div>
            </div>
            <div class="about-mini">
              <div class="am-t">&#8505; TAB TENTANG</div>
              <div class="am-d">Halaman yang sedang kamu baca ini &mdash; ringkasan cara kerja seluruh sistem
              supaya pengguna baru bisa cepat memahami fungsi setiap bagian dari web ini.</div>
            </div>
          </div>

          <div class="about-card">
            <h3>&#9200; Logika Otomatis Tiap Relay</h3>
            <ul>
              <li><b>R1 &mdash; INVERTER</b> [Manual + PZEM]: otomatis ON saat PLN tidak ada/lemah (&lt;50V),
              OFF saat PLN ada (&ge;50V). Bisa dioverride manual, dan akan tetap MANUAL sampai tombol
              <b>AUTO</b> ditekan kembali.</li>
              <li><b>R2 &mdash; LAMPU UTAMA</b> [Manual | Timer + PZEM]: ON otomatis selama jam timer aktif,
              ATAU saat PLN tidak ada. Bisa dioverride manual, tetap MANUAL sampai tombol AUTO ditekan.</li>
              <li><b>R3 &mdash; OUTPUT AC</b> [Manual + Auto Timer]: ON otomatis selama jam timer aktif saja.
              Bisa dioverride manual, tetap MANUAL sampai tombol AUTO ditekan.</li>
              <li><b>R4 &mdash; LAMPU PANEL</b> [Manual sesaat + PZEM]: ON saat PLN 0V, OFF saat PLN &ge;50V.
              Override manual hanya berlaku sesaat &mdash; otomatis kembali ke mode PZEM dalam &plusmn;1 detik.</li>
            </ul>
          </div>

          <div class="about-card">
            <h3>&#128274; Dua Jenis Login yang Berbeda</h3>
            <p>
              Web ini memakai <b>dua sistem password terpisah</b> supaya lebih aman:
            </p>
            <ul>
              <li><b>LOGIN (tombol header)</b> &mdash; dipakai untuk mengontrol relay &amp; mengubah jadwal timer
              di tab Kontrol.</li>
              <li><b>Password Control Panel</b> &mdash; dipakai khusus untuk membuka tab Control Panel Admin.
              Login di salah satu tidak otomatis membuka yang lain.</li>
            </ul>
          </div>

          <div class="about-card">
            <h3>&#128176; Proteksi Biaya &amp; Arus</h3>
            <p>
              Admin dapat menetapkan <b>Limit Biaya</b> (Rupiah) &mdash; jika biaya akumulasi listrik AC menyentuh
              limit ini, semua relay otomatis dikunci OFF sampai limit dinaikkan. Admin juga dapat menetapkan
              <b>Batas Maksimal Arus AC</b> &mdash; jika terlampaui, semua relay langsung TRIP OFF (seperti MCB)
              dan tetap OFF sampai direset manual di Control Panel, walau arus sudah turun kembali.
            </p>
          </div>

          <div class="about-card creator-card">
            <h3>&#128100; Tentang Pengembang</h3>
            <p>
              Web Monitor PLTS ini dirancang, dikodekan, dan dirawat dari nol &mdash; mulai dari firmware ESP32,
              logika otomasi tiap relay, hingga antarmuka yang sedang kamu gunakan sekarang &mdash; oleh:
            </p>
            <div class="creator-block">
              <div class="creator-name">Moh Hilmi Mulia Rahardyan</div>
              <div class="creator-meta">
                <span><b>Sekolah</b> &mdash; SMKN 1 Singosari</span>
                <span><b>Tim</b> &mdash; Team MP</span>
              </div>
            </div>
            <p style="margin-top:12px">
              Proyek ini lahir dari ketertarikan pada perpaduan antara elektronika, firmware, dan pengembangan web
              &mdash; sebuah usaha untuk menghadirkan sistem energi surya yang tidak hanya berfungsi di balik layar,
              tetapi juga mudah dipahami dan dikendalikan oleh siapa pun yang menggunakannya.
            </p>
            <a class="creator-contact" href="mailto:hilmimulia855@gmail.com">
              <span class="cc-ico">&#9993;</span>
              <span>
                <span class="cc-lbl">Hubungi Pengembang</span>
                <span class="cc-mail">hilmimulia855@gmail.com</span>
              </span>
            </a>
          </div>
        </section>
      </div>
    </div>


    <footer>
      <div class="flogo">&#9728; WEB MONITOR PLTS &#9728;</div>
      <div class="fsince">SINCE <span>2026</span> &nbsp;&mdash;&nbsp; BY <span>MINIONS PROJECT</span></div>
    </footer>
  </div>

)rawliteral";
    server.sendContent(html4);
  }
  {
    String html5 = R"rawliteral(  <script>
    // ===== URL TUNNEL DIAMBIL OTOMATIS DARI GITHUB (hanya dipakai kalau diakses dari LUAR, misal lewat tunnel) =====
    // ISI (opsional): ganti USERNAME & REPO dengan milik kamu. File tunnel-url.txt berisi 1 baris URL tunnel.
    // Kalau web hanya diakses lewat jaringan lokal / hotspot ESP32, bagian ini TIDAK perlu diisi.
    var TUNNEL_URL_SOURCE = "https://raw.githubusercontent.com/USERNAME/REPO/main/tunnel-url.txt";
    // FIX LOGIN: kalau halaman ini dibuka LANGSUNG dari ESP32 (AP 192.168.4.1 atau
    // IP lokal STA seperti 192.168.x.x / 10.x.x.x), TIDAK perlu tunnel/internet sama
    // sekali -> pakai origin sendiri (fetch relatif, BASE = ''). Sebelumnya BASE selalu
    // wajib diisi dari tunnel-url.txt (butuh internet), padahal saat konek ke AP ESP32
    // biasanya HP/laptop TIDAK ada internet -> BASE gagal terisi -> tombol LOGIN &
    // Control Panel selalu bilang "BASE URL belum siap" walau password sudah benar.
    var IS_LOCAL_ESP = /^192\.168\.|^10\.|^172\.(1[6-9]|2[0-9]|3[0-1])\.|^localhost$/.test(location.hostname);
    var BASE = IS_LOCAL_ESP ? '' : (localStorage.getItem('tunnelBase') || null);
    var baseReady = IS_LOCAL_ESP ? true : !!BASE;

    // ===== SISTEM LOGIN ADMIN (tombol header, kontrol relay & timer) =====
    // Token ini TERPISAH dari cpToken (Control Panel) di bawah.
    var adminToken = localStorage.getItem('adminToken') || null;

    function updAdminBtn(){
      var b = document.getElementById('adminBtn');
      if(adminToken){
        b.classList.add('logged'); b.innerHTML='&#128275; LOGOUT';
      } else {
        b.classList.remove('logged'); b.innerHTML='&#128274; LOGIN';
      }
    }

    // ===== CONTROL PANEL — password & token SENDIRI, beda dari LOGIN header =====
    var cpToken = localStorage.getItem('cpToken') || null;

    function updCPPanel(){
      var locked = document.getElementById('cpLocked');
      var unlocked = document.getElementById('cpUnlocked');
      if(cpToken){
        if(locked) locked.style.display = 'none';
        if(unlocked){ unlocked.style.display = 'block'; loadAdmStatus(); }
      } else {
        if(locked) locked.style.display = 'block';
        if(unlocked) unlocked.style.display = 'none';
      }
    }

    function cpLogin(){
      if(!baseReady){ document.getElementById('cpLoginErr').innerText = 'BASE URL belum siap, coba lagi.'; return; }
      var pass = document.getElementById('cpPass').value;
      if(!pass){ document.getElementById('cpLoginErr').innerText = 'Password tidak boleh kosong.'; return; }
      document.getElementById('cpLoginErr').innerText = '';
      fetch(BASE+'/cplogin?password='+encodeURIComponent(pass))
        .then(function(r){
          if(r.status===401){ throw new Error('badpass'); }
          if(!r.ok){ throw new Error('network'); }
          return r.text();
        })
        .then(function(tok){
          tok = tok.trim();
          if(!tok){ throw new Error('empty'); }
          cpToken = tok;
          localStorage.setItem('cpToken', cpToken);
          document.getElementById('cpPass').value = '';
          updCPPanel();
          toast('&#10003; Control panel terbuka','ok');
        })
        .catch(function(e){
          var el = document.getElementById('cpLoginErr');
          if(e && e.message==='badpass'){ el.innerText = 'Password Control Panel salah. (Ini beda dari password LOGIN header)'; }
          else { el.innerText = 'Gagal konek ke ESP32 / tunnel. Cek koneksi lalu coba lagi.'; }
        });
    }

    function cpLogout(){
      if(!confirm('Kunci Control Panel & logout?')) return;
      cpToken = null;
      localStorage.removeItem('cpToken');
      updCPPanel();
      toast('Logout Control Panel berhasil','');
    }

    document.getElementById('cpPass').addEventListener('keydown', function(e){
      if(e.key === 'Enter') cpLogin();
    });

    function onAdminBtnClick(){
      if(adminToken){
        if(confirm('Logout dari mode admin?')){
          adminToken = null;
          localStorage.removeItem('adminToken');
          updAdminBtn();
          toast('Logout berhasil','');
        }
      } else {
        openLoginModal();
      }
    }

    function openLoginModal(){
      document.getElementById('loginErr').innerText = '';
      document.getElementById('adminPass').value = '';
      document.getElementById('loginModal').classList.add('show');
      setTimeout(function(){ document.getElementById('adminPass').focus(); }, 50);
    }

    function closeLoginModal(){
      document.getElementById('loginModal').classList.remove('show');
    }

    function doLogin(){
      if(!baseReady){ document.getElementById('loginErr').innerText = 'BASE URL belum siap, coba lagi.'; return; }
      var pass = document.getElementById('adminPass').value;
      if(!pass){ document.getElementById('loginErr').innerText = 'Password tidak boleh kosong.'; return; }
      fetch(BASE+'/login?password='+encodeURIComponent(pass))
        .then(function(r){
          if(r.status===401){ throw new Error('badpass'); }
          if(!r.ok){ throw new Error('network'); }
          return r.text();
        })
        .then(function(tok){
          tok = tok.trim();
          if(!tok){ throw new Error('empty'); }
          adminToken = tok;
          localStorage.setItem('adminToken', adminToken);
          updAdminBtn();
          closeLoginModal();
          toast('&#10003; Login admin berhasil','ok');
        })
        .catch(function(e){
          var el = document.getElementById('loginErr');
          if(e && e.message==='badpass'){ el.innerText = 'Password LOGIN salah.'; }
          else { el.innerText = 'Gagal konek ke ESP32 / tunnel. Cek koneksi lalu coba lagi.'; }
        });
    }

    document.getElementById('adminPass').addEventListener('keydown', function(e){
      if(e.key === 'Enter') doLogin();
    });

    function requireAuth(){
      if(!adminToken){
        toast('Login admin dulu untuk kontrol ini!','er');
        openLoginModal();
        return false;
      }
      return true;
    }

    async function refreshBase(){
      if (IS_LOCAL_ESP) return; // akses lokal ESP32 -> sudah pakai origin sendiri, tak perlu tunnel
      try{
        var r = await fetch(TUNNEL_URL_SOURCE + "?t=" + Date.now());
        var t = await r.text();
        var newBase = t.trim();
        if(newBase && newBase.indexOf("http") === 0 && newBase !== BASE){
          BASE = newBase;
          baseReady = true;
          localStorage.setItem('tunnelBase', BASE);
          console.log("BASE diupdate ke:", BASE);
        }
      }catch(e){ console.warn("Gagal ambil URL tunnel terbaru:", e); }
    }
    // =====================================================

    var tPend = false;

    function fmt(h,m){ return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0'); }

    var tTimer = null;
    function toast(msg, type){
      var t = document.getElementById('toast');
      t.innerHTML = msg;
      t.className = 'toast show ' + (type||'');
      if(tTimer) clearTimeout(tTimer);
      tTimer = setTimeout(function(){ t.className='toast'; }, 2800);
    }

    function updTimer(sh,sm,eh,em){
      document.getElementById('tad').innerText = fmt(sh,sm);
      document.getElementById('tae').innerText = fmt(eh,em);
      var b = document.getElementById('tbdg');
      b.className = 'tbdg' + (tPend?' pend':'');
      b.innerText  = tPend ? 'PENDING...' : 'AKTIF';
    }

    function cfmTimer(){
      if(!requireAuth()) return;
      var s=document.getElementById('ts').value, e=document.getElementById('te').value;
      if(!s||!e){ toast('Isi jam mulai & selesai dulu!','er'); return; }
      var ps=s.split(':'), pe=e.split(':');
      var sh=+ps[0], sm=+ps[1], eh=+pe[0], em=+pe[1];
      if(sh===eh&&sm===em){ toast('Jam mulai & selesai tidak boleh sama!','er'); return; }
      fetch(BASE+'/settimer?start_h='+sh+'&start_m='+sm+'&end_h='+eh+'&end_m='+em+'&token='+encodeURIComponent(adminToken))
        .then(function(r){
          if(r.ok){ tPend=false; updTimer(sh,sm,eh,em); toast('&#10003; Timer: '+fmt(sh,sm)+' &ndash; '+fmt(eh,em),'ok'); }
          else if(r.status===401){ toast('Sesi admin tidak valid, login ulang!','er'); adminToken=null; localStorage.removeItem('adminToken'); updAdminBtn(); }
          else toast('Gagal simpan timer!','er');
        }).catch(function(){ toast('Gagal konek ESP32!','er'); });
    }

    function rstTimer(){
      if(!requireAuth()) return;
      fetch(BASE+'/settimer?start_h=18&start_m=0&end_h=21&end_m=0&token='+encodeURIComponent(adminToken))
        .then(function(r){
          if(r.ok){
            tPend=false;
            document.getElementById('ts').value='18:00';
            document.getElementById('te').value='21:00';
            updTimer(18,0,21,0);
            toast('&#8634; Reset ke default: 18:00 &ndash; 21:00','');
          } else if(r.status===401){ toast('Sesi admin tidak valid, login ulang!','er'); adminToken=null; localStorage.removeItem('adminToken'); updAdminBtn(); }
          else toast('Gagal reset!','er');
        }).catch(function(){ toast('Gagal konek ESP32!','er'); });
    }

    ['ts','te'].forEach(function(id){
      document.getElementById(id).addEventListener('change', function(){
        tPend=true; document.getElementById('tbdg').className='tbdg pend';
        document.getElementById('tbdg').innerText='PENDING...';
      });
    });

    function updData(){
      if(!baseReady) { console.warn('BASE belum siap, skip fetch data'); return; }
      fetch(BASE+'/data?_='+Date.now(), {cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
        document.getElementById('v').innerText   = d.v.toFixed(2);
        document.getElementById('vp').innerText  = d.vp.toFixed(2);
        document.getElementById('ic').innerText  = d.i.toFixed(3);
        document.getElementById('pw').innerText  = d.p.toFixed(1);
        document.getElementById('bt').innerText  = d.b;
        document.getElementById('en').innerText  = d.e.toFixed(1);
        document.getElementById('bwh').innerText = d.bwh.toFixed(1);
        var bestEl = document.getElementById('best');
        if(d.inaBatt === 0){
          bestEl.innerText = 'Perlu Sensor INA219';
        } else if(d.estMin < 0){
          bestEl.innerText = 'Charging / Idle';
        } else {
          var ej = Math.floor(d.estMin/60), em = d.estMin%60;
          bestEl.innerText = (ej>99 ? '>99 jam' : (ej+' jam '+em+' menit'));
        }
        document.getElementById('dah').innerText = d.dAh.toFixed(3);
        document.getElementById('cah').innerText = d.cAh.toFixed(3);
        document.getElementById('pi').innerText  = d.pi.toFixed(3);
        document.getElementById('pp').innerText  = d.pp.toFixed(1);
        document.getElementById('pe').innerText  = d.pe.toFixed(1);
        document.getElementById('bf').style.width= d.b+'%';
        document.getElementById('acv').innerText = d.acV.toFixed(1);
        document.getElementById('aci').innerText = d.acI.toFixed(2);
        document.getElementById('acp').innerText = d.acP.toFixed(1);
        document.getElementById('acf').innerText = d.acF.toFixed(1);
        document.getElementById('acpf').innerText= d.acPF.toFixed(2);
        document.getElementById('ace').innerText = d.acE.toFixed(3);
        document.getElementById('acrp').innerText = 'Rp ' + Math.round(d.acRp).toLocaleString('id-ID');
        document.getElementById('budgetBanner').style.display = d.budgetLocked ? 'block' : 'none';
        document.getElementById('currentBanner').style.display = d.currentTripped ? 'block' : 'none';

        if(!tPend){
          updTimer(d.tsh, d.tsm, d.teh, d.tem);
          document.getElementById('ts').value = fmt(d.tsh, d.tsm);
          document.getElementById('te').value = fmt(d.teh, d.tem);
        }

        var bn = document.getElementById('acBanner');
        var bt2= document.getElementById('acBTxt');
        if(d.acV < 50){
          bn.className='acb warn';
          bt2.innerHTML='&#9889; <strong>PLN tidak ada / lemah: '+d.acV.toFixed(1)+'V (&lt;50V)</strong> &rarr; Inverter, Lampu Utama &amp; Lampu Panel ON otomatis';
        } else {
          bn.className='acb ok';
          bt2.innerHTML='&#9889; <strong>PLN ada: '+d.acV.toFixed(1)+'V (&ge;50V)</strong> &mdash; Inverter &amp; Lampu Panel OFF | Lampu Utama &amp; Output AC ikut Timer';
        }

        [1,2,3,4].forEach(function(n){
          // Kalau relay ini baru saja ditoggle manual, jangan timpa tampilannya dulu
          // selama masa "lock" — supaya tombol tidak bolak-balik akibat data basi.
          if(relayLock[n] && Date.now() < relayLock[n]) return;
          var locked = (d.budgetLocked===1) || (d.currentTripped===1);
          var stateOn = d['r'+n] === 1;      // status yang "diingat" (manual/auto)
          var on = stateOn && !locked;       // status FISIK aktual (yang ditampilkan)
          document.getElementById('r'+n).innerText   = on ? 'NYALA' : (locked && stateOn ? 'DIKUNCI' : 'MATI');
          document.getElementById('r'+n).className   = 'rbdg ' + (on?'bon':'boff');
          document.getElementById('cr'+n).className  = 'rc ' + (on?'ron':'roff');
          document.getElementById('t'+n).checked     = on;

          var isManual = d['m'+n] === 1;
          var mb = document.getElementById('m'+n);
          if(mb){
            mb.innerText = isManual ? 'MANUAL' : 'AUTO';
            mb.className = 'modebdg ' + (isManual ? 'manual' : 'auto');
          }
          var ab = document.getElementById('ab'+n);
          if(ab){ ab.disabled = !isManual; }
        });
      }).catch(function(e){ console.warn('Data error:',e); });
    }

    // Timestamp "kunci" per relay: selama masih di bawah waktu ini,
    // updData() tidak boleh menimpa tampilan relay tsb (anti kedip/bolak-balik)
    var relayLock = {1:0, 2:0, 3:0, 4:0};

    // Dipanggil PERTAMA KALI saat toggle diklik, SEBELUM browser mengubah
    // tampilan checkbox. Kalau belum login, batalkan aksi toggle di sini
    // (preventDefault) supaya toggle TIDAK PERNAH kelihatan bergerak sama
    // sekali sebelum login diminta — tidak ada lagi kedipan "kepencet dulu
    // baru diminta login".
    function onToggleClick(ev, n){
      if(!adminToken){
        ev.preventDefault();
        toast('Login admin dulu untuk kontrol ini!','er');
        openLoginModal();
        return false;
      }
      // Sudah login: biarkan checkbox berubah normal (default browser),
      // baru kirim state barunya ke setR() setelah checkbox benar-benar toggle.
      setTimeout(function(){
        setR(n, document.getElementById('t'+n).checked ? 1 : 0);
      }, 0);
      return true;
    }

    function setR(n, s){
      if(!requireAuth()){
        // batalkan toggle visual karena tidak diizinkan
        document.getElementById('t'+n).checked = !s;
        return;
      }

      // Update tampilan LANGSUNG (optimistic) begitu ditekan, tidak nunggu server
      var on = (s === 1 || s === true);
      document.getElementById('r'+n).innerText  = on ? 'NYALA' : 'MATI';
      document.getElementById('r'+n).className  = 'rbdg ' + (on?'bon':'boff');
      document.getElementById('cr'+n).className = 'rc ' + (on?'ron':'roff');
      document.getElementById('t'+n).checked    = on;

      // Kunci tampilan relay ini selama 3 detik supaya polling 2 detik
      // berikutnya tidak menimpa balik dengan data yang mungkin masih basi
      relayLock[n] = Date.now() + 3000;

      fetch(BASE+'/relay?r='+n+'&s='+s+'&token='+encodeURIComponent(adminToken)+'&_='+Date.now(), {cache:'no-store'})
        .then(function(r){
          if(r.ok) {
            // konfirmasi ulang setelah lock lepas, biar sinkron pasti dengan ESP32
            setTimeout(updData, 3100);
          }
          else if(r.status===401){
            toast('Sesi admin tidak valid, login ulang!','er');
            adminToken=null; localStorage.removeItem('adminToken'); updAdminBtn();
            relayLock[n] = 0;
            document.getElementById('t'+n).checked = !s;
          } else {
            relayLock[n] = 0;
            document.getElementById('t'+n).checked = !s;
          }
        })
        .catch(function(e){
          console.warn('Relay error:',e);
          relayLock[n] = 0;
          document.getElementById('t'+n).checked = !s;
        });
    }

    function setAuto(n){
      if(!requireAuth()) return;
      relayLock[n] = 0;
      fetch(BASE+'/relay?r='+n+'&s=auto&token='+encodeURIComponent(adminToken)+'&_='+Date.now(), {cache:'no-store'})
        .then(function(r){
          if(r.ok) { toast('Relay '+n+' dikembalikan ke mode AUTO',''); updData(); }
          else if(r.status===401){
            toast('Sesi admin tidak valid, login ulang!','er');
            adminToken=null; localStorage.removeItem('adminToken'); updAdminBtn();
          } else { toast('Gagal mengubah ke AUTO!','er'); }
        })
        .catch(function(e){ console.warn('Auto error:',e); toast('Gagal konek ESP32!','er'); });
    }

    function updClk(){
      document.getElementById('clk').innerText =
        new Date().toLocaleTimeString('id-ID',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
    }

    if (baseReady) {
      // Lokal ESP32 (langsung siap) ATAU BASE tunnel sudah ada di cache → tampilkan data langsung
      updData();
      if (!IS_LOCAL_ESP) refreshBase();
    } else {
      refreshBase().then(function(){ updData(); });
    }
    updAdminBtn();
    updCPPanel();
    updClk();
    setInterval(updData, 500);
    setInterval(updClk,  1000);
    setInterval(refreshBase, 20000);

    // ===== CONTROL PANEL JS (pakai cpToken, BUKAN adminToken) =====
    // qs (opsional): parameter tambahan, misal 'newpass=xxxx&foo=bar'
    function admFetch(path, qs){
      var url = BASE+path+'?token='+encodeURIComponent(cpToken)+(qs?('&'+qs):'');
      return fetch(url)
        .then(function(r){
          if(r.status===401){ cpToken=null; localStorage.removeItem('cpToken'); updCPPanel(); toast('Sesi Control Panel expired, login ulang','er'); throw new Error('401'); }
          return r;
        });
    }

    function admTab(el, paneId){
      document.querySelectorAll('.adm-tab').forEach(function(t){ t.classList.remove('active'); });
      document.querySelectorAll('.adm-pane').forEach(function(p){ p.style.display='none'; });
      el.classList.add('active');
      document.getElementById(paneId).style.display='block';
      if(paneId==='admCal') loadAdmCal();
      if(paneId==='admBudget') loadAdmBudget();
      if(paneId==='admCurrent') loadAdmCurrent();
      if(paneId==='admLog') loadAdmLogs();
    }

    function loadAdmStatus(){
      if(!baseReady) return;
      admFetch('/admin/status').then(function(r){ return r.json(); }).then(function(d){
        document.getElementById('aUptime').innerText  = d.uptime;
        document.getElementById('aHeap').innerText    = (d.freeHeap/1024).toFixed(1)+' KB';
        document.getElementById('aRssi').innerText    = d.rssi+' dBm';
        document.getElementById('aIp').innerText      = d.ip;
        document.getElementById('aApIp').innerText    = d.apIP;
        document.getElementById('aCurPass').innerText = d.loginPassword;
        var cpEl=document.getElementById('aCurCPPass'); if(cpEl) cpEl.innerText = d.cpPassword;
      }).catch(function(){});
    }

    // Ganti password LOGIN (tombol header, kontrol relay & timer)
    function doSetPassword(){
      var p1=document.getElementById('newPass1').value.trim();
      var p2=document.getElementById('newPass2').value.trim();
      var msg=document.getElementById('passMsg');
      if(p1.length<4){ msg.className='adm-msg er'; msg.innerText='Password minimal 4 karakter!'; return; }
      if(p1!==p2){ msg.className='adm-msg er'; msg.innerText='Password tidak cocok!'; return; }
      admFetch('/admin/setpassword','newpass='+encodeURIComponent(p1))
        .then(function(r){ return r.ok ? r.text() : Promise.reject(); })
        .then(function(){ msg.className='adm-msg ok'; msg.innerText='&#10003; Password LOGIN berhasil diubah!';
          document.getElementById('newPass1').value=''; document.getElementById('newPass2').value='';
          loadAdmStatus(); })
        .catch(function(){ msg.className='adm-msg er'; msg.innerText='Gagal menyimpan password!'; });
    }

    // Ganti password CONTROL PANEL (panel ini sendiri)
    function doSetCPPassword(){
      var p1=document.getElementById('newCPPass1').value.trim();
      var p2=document.getElementById('newCPPass2').value.trim();
      var msg=document.getElementById('cpPassMsg');
      if(p1.length<4){ msg.className='adm-msg er'; msg.innerText='Password minimal 4 karakter!'; return; }
      if(p1!==p2){ msg.className='adm-msg er'; msg.innerText='Password tidak cocok!'; return; }
      admFetch('/admin/setcppassword','newpass='+encodeURIComponent(p1))
        .then(function(r){ return r.ok ? r.text() : Promise.reject(); })
        .then(function(){ msg.className='adm-msg ok'; msg.innerText='&#10003; Password Control Panel diubah! Silakan login ulang.';
          cpToken=null; localStorage.removeItem('cpToken');
          setTimeout(function(){ updCPPanel(); }, 1500); })
        .catch(function(){ msg.className='adm-msg er'; msg.innerText='Gagal menyimpan password!'; });
    }

    function loadAdmCal(){
      if(!baseReady) return;
      admFetch('/admin/getcal').then(function(r){ return r.json(); }).then(function(d){
        for(var i=0;i<17;i++){
          document.getElementById('w'+i).value = d.lut[i].w;
          document.getElementById('a'+i).value = d.lut[i].a;
        }
        document.getElementById('panelCal').value = d.panelCal;
      }).catch(function(){});
    }

    function doSetCal(){
      var msg=document.getElementById('calMsg');
      var qs='';
      for(var i=0;i<17;i++){
        qs+='w'+i+'='+document.getElementById('w'+i).value+'&';
        qs+='a'+i+'='+document.getElementById('a'+i).value+'&';
      }
      qs+='panelCal='+document.getElementById('panelCal').value;
      admFetch('/admin/setcal', qs)
        .then(function(r){ return r.ok ? r.text() : Promise.reject(); })
        .then(function(){ msg.className='adm-msg ok'; msg.innerText='&#10003; Kalibrasi disimpan ke ESP32!'; })
        .catch(function(){ msg.className='adm-msg er'; msg.innerText='Gagal menyimpan kalibrasi!'; });
    }

    function loadAdmBudget(){
      admFetch('/admin/getbudget').then(function(r){ return r.json(); }).then(function(d){
        document.getElementById('rpLimitInput').value = d.rpLimit;
        document.getElementById('bRpNow').innerText = 'Rp ' + Math.round(d.rpNow).toLocaleString('id-ID');
        var st=document.getElementById('bStatus');
        if(d.locked){ st.innerText='TERKUNCI'; st.style.color='#ff4d4d'; }
        else if(d.rpLimit>0){ st.innerText='AKTIF'; st.style.color='#00e5cc'; }
        else { st.innerText='NONAKTIF'; st.style.color='var(--mut)'; }
      }).catch(function(){});
    }

    function doSetBudget(){
      var v = document.getElementById('rpLimitInput').value;
      var msg = document.getElementById('budgetMsg');
      if(v===''){ msg.className='adm-msg er'; msg.innerText='Isi angka limit dulu (0 untuk nonaktif)!'; return; }
      admFetch('/admin/setbudget', 'limit='+encodeURIComponent(v))
        .then(function(r){ return r.ok ? r.text() : Promise.reject(); })
        .then(function(){ msg.className='adm-msg ok'; msg.innerText='&#10003; Limit biaya disimpan!'; loadAdmBudget(); })
        .catch(function(){ msg.className='adm-msg er'; msg.innerText='Gagal menyimpan limit!'; });
    }

    function loadAdmCurrent(){
      admFetch('/admin/getmaxcurrent').then(function(r){ return r.json(); }).then(function(d){
        document.getElementById('maxCurrentInput').value = d.maxAcCurrent;
        document.getElementById('cAcNow').innerText = d.acCurrentNow.toFixed(2)+' A';
        var st=document.getElementById('cStatus');
        if(d.tripped){ st.innerText='TRIP'; st.style.color='#ff4d4d'; }
        else if(d.maxAcCurrent>0){ st.innerText='AKTIF'; st.style.color='#00e5cc'; }
        else { st.innerText='NONAKTIF'; st.style.color='var(--mut)'; }
      }).catch(function(){});
    }

    function doSetMaxCurrent(){
      var v = document.getElementById('maxCurrentInput').value;
      var msg = document.getElementById('maxCurrentMsg');
      if(v===''){ msg.className='adm-msg er'; msg.innerText='Isi angka batas arus dulu (0 untuk nonaktif)!'; return; }
      admFetch('/admin/setmaxcurrent', 'limit='+encodeURIComponent(v))
        .then(function(r){ return r.ok ? r.text() : Promise.reject(); })
        .then(function(){ msg.className='adm-msg ok'; msg.innerText='&#10003; Batas arus disimpan!'; loadAdmCurrent(); })
        .catch(function(){ msg.className='adm-msg er'; msg.innerText='Gagal menyimpan batas arus!'; });
    }

    function doResetCurrentTrip(){
      var msg = document.getElementById('resetCurrentMsg');
      admFetch('/admin/resetcurrent')
        .then(function(r){ return r.ok ? r.text() : Promise.reject(); })
        .then(function(){ msg.className='adm-msg ok'; msg.innerText='&#10003; Trip arus direset, relay dibuka kembali!'; loadAdmCurrent(); })
        .catch(function(){ msg.className='adm-msg er'; msg.innerText='Gagal reset trip!'; });
    }

    function loadAdmLogs(){
      if(!baseReady) return;
      admFetch('/admin/logs').then(function(r){ return r.json(); }).then(function(logs){
        var tbl=document.getElementById('logTable');
        if(!logs.length){ tbl.innerHTML='<div class="adm-lbl" style="text-align:center">Belum ada log</div>'; return; }
        var h='';
        logs.forEach(function(l){
          h+='<div class="log-row">';
          h+='<span class="log-ip">'+l.ip+'</span>';
          h+='<span class="log-time">'+l.waktu+'</span>';
          h+='<span class="log-ok '+(l.ok?'y':'n')+'">'+(l.ok?'BERHASIL':'GAGAL')+'</span>';
          h+='</div>';
        });
        tbl.innerHTML=h;
      }).catch(function(){});
    }

    // ===== TAB SWITCHING (sidebar) — murni tampilan, tidak mengubah logika data/relay =====
    function switchTab(name){
      var panes = document.querySelectorAll('.tab-pane');
      for (var i=0; i<panes.length; i++){ panes[i].classList.remove('active'); }
      var tabs = document.querySelectorAll('.side-tab');
      for (var j=0; j<tabs.length; j++){ tabs[j].classList.remove('active'); }
      var pane = document.getElementById('tab-'+name);
      var nav  = document.getElementById('navtab-'+name);
      if (pane) pane.classList.add('active');
      if (nav)  nav.classList.add('active');
      try { localStorage.setItem('activeTab', name); } catch(e){}
    }
    (function(){
      var saved = null;
      try { saved = localStorage.getItem('activeTab'); } catch(e){}
      if (saved) switchTab(saved);
    })();
  </script>
</body>
</html>
)rawliteral";
    server.sendContent(html5);
  }
  server.sendContent("");  // akhiri chunked transfer
}

// ================================================================
//  HELPER: CEK OTORISASI TOKEN
// ================================================================
bool isAuthorized() {
  if (adminToken == "") return false;
  if (!server.hasArg("token")) return false;
  return server.arg("token") == adminToken;
}

// Otorisasi khusus Control Panel (status/password/kalibrasi/log) — token
// TERPISAH dari isAuthorized() di atas, jadi sesi login relay/timer dan
// sesi Control Panel tidak saling membuka satu sama lain.
bool isCPAuthorized() {
  if (cpToken == "") return false;
  if (!server.hasArg("token")) return false;
  return server.arg("token") == cpToken;
}

// ================================================================
//  WEB: HANDLE LOGIN
// ================================================================
void addLog(const String& ip, bool ok) {
  // Geser log lama kalau sudah penuh
  if (logCount >= MAX_LOGS) {
    for (int i = 0; i < MAX_LOGS - 1; i++) accessLogs[i] = accessLogs[i+1];
    logCount = MAX_LOGS - 1;
  }
  AccessLog& l = accessLogs[logCount++];
  strncpy(l.ip, ip.c_str(), 19); l.ip[19] = '\0';
  l.berhasil = ok;
  // Ambil waktu NTP kalau tersedia
  struct tm ti;
  if (getLocalTime(&ti, 100)) {
    strftime(l.waktu, sizeof(l.waktu), "%d/%m/%Y %H:%M:%S", &ti);
  } else {
    unsigned long s = millis()/1000;
    snprintf(l.waktu, sizeof(l.waktu), "uptime %lus", s);
  }
}

void handleLogin() {
  if (!server.hasArg("password")) { server.send(400, "text/plain", "Bad Request"); return; }
  String pass = server.arg("password");
  String clientIP = server.client().remoteIP().toString();
  // Password kosong (belum diisi di source code) SELALU ditolak
  if (LOGIN_PASSWORD.length() == 0 || pass != LOGIN_PASSWORD) {
    Serial.println("[LOGIN] Password salah dari " + clientIP);
    addLog(clientIP, false);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  adminToken = generateToken();
  tokenCreatedMillis = millis();
  addLog(clientIP, true);
  Serial.println("[LOGIN] Berhasil dari " + clientIP);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", adminToken);
}

// ================================================================
//  WEB: HANDLE LOGIN CONTROL PANEL (password terpisah dari /login)
// ================================================================
void handleCPLogin() {
  if (!server.hasArg("password")) { server.send(400, "text/plain", "Bad Request"); return; }
  String pass = server.arg("password");
  String clientIP = server.client().remoteIP().toString();
  // Password kosong (belum diisi di source code) SELALU ditolak
  if (CP_PASSWORD.length() == 0 || pass != CP_PASSWORD) {
    Serial.println("[CPLOGIN] Password salah dari " + clientIP);
    addLog(clientIP, false);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  cpToken = generateToken();
  cpTokenCreatedMillis = millis();
  addLog(clientIP, true);
  Serial.println("[CPLOGIN] Berhasil dari " + clientIP);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", cpToken);
}

// ================================================================
//  ADMIN: STATUS ESP32
// ================================================================
void handleAdminStatus() {
  if (!isCPAuthorized()) { server.sendHeader("Access-Control-Allow-Origin","*"); server.send(401,"text/plain","Unauthorized"); return; }
  unsigned long up = (millis() - bootMillis) / 1000;
  int h = up/3600, m = (up%3600)/60, s = up%60;
  char upStr[20]; snprintf(upStr, sizeof(upStr), "%02d:%02d:%02d", h, m, s);
  String json = "{";
  json += "\"uptime\":\"" + String(upStr) + "\",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"apIP\":\"192.168.4.1\",";
  json += "\"ntpSynced\":" + String(ntpSynced ? 1 : 0) + ",";
  json += "\"loginPassword\":\"" + LOGIN_PASSWORD + "\",";
  json += "\"cpPassword\":\"" + CP_PASSWORD + "\"";
  json += "}";
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "application/json", json);
}

// ================================================================
//  ADMIN: GANTI PASSWORD LOGIN (tombol LOGIN header / kontrol relay & timer)
//  Dikerjakan DARI DALAM Control Panel, makanya perlu isCPAuthorized().
// ================================================================
void handleAdminSetPassword() {
  if (!isCPAuthorized()) { server.sendHeader("Access-Control-Allow-Origin","*"); server.send(401,"text/plain","Unauthorized"); return; }
  if (!server.hasArg("newpass") || server.arg("newpass").length() < 4) {
    server.send(400, "text/plain", "Password minimal 4 karakter"); return;
  }
  LOGIN_PASSWORD = server.arg("newpass");
  adminToken = ""; // paksa login ulang di tombol LOGIN header
  prefs.begin("admin", false);
  prefs.putString("loginpass", LOGIN_PASSWORD);
  prefs.end();
  Serial.println("[ADMIN] Password LOGIN baru disimpan");
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "text/plain", "OK");
}

// ================================================================
//  ADMIN: GANTI PASSWORD CONTROL PANEL (password panel itu sendiri)
// ================================================================
void handleAdminSetCPPassword() {
  if (!isCPAuthorized()) { server.sendHeader("Access-Control-Allow-Origin","*"); server.send(401,"text/plain","Unauthorized"); return; }
  if (!server.hasArg("newpass") || server.arg("newpass").length() < 4) {
    server.send(400, "text/plain", "Password minimal 4 karakter"); return;
  }
  CP_PASSWORD = server.arg("newpass");
  cpToken = ""; // paksa login ulang ke Control Panel
  prefs.begin("admin", false);
  prefs.putString("cppass", CP_PASSWORD);
  prefs.end();
  Serial.println("[ADMIN] Password CONTROL PANEL baru disimpan");
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "text/plain", "OK");
}

// ================================================================
//  ADMIN: GANTI KALIBRASI
// ================================================================
void handleAdminSetCal() {
  if (!isCPAuthorized()) { server.sendHeader("Access-Control-Allow-Origin","*"); server.send(401,"text/plain","Unauthorized"); return; }
  bool ok = true;
  for (int i = 0; i < BATT_LUT_SIZE; i++) {
    String kw = "w" + String(i), ka = "a" + String(i);
    if (!server.hasArg(kw) || !server.hasArg(ka)) { ok = false; break; }
    BATT_LUT_WEB[i] = server.arg(kw).toFloat();
    BATT_LUT_AVO[i] = server.arg(ka).toFloat();
  }
  if (server.hasArg("panelCal")) PANEL_CAL = server.arg("panelCal").toFloat();
  if (!ok) { server.send(400,"text/plain","Parameter tidak lengkap"); return; }
  sortBattLUT();  // jaga urutan menaik supaya battCalibrate() tidak gagal
  prefs.begin("admin", false);
  for (int i = 0; i < BATT_LUT_SIZE; i++) {
    prefs.putFloat(("w"+String(i)).c_str(), BATT_LUT_WEB[i]);
    prefs.putFloat(("a"+String(i)).c_str(), BATT_LUT_AVO[i]);
  }
  prefs.putFloat("panelCal", PANEL_CAL);
  prefs.putBool("calSaved17", true);  // flag BARU, khusus tabel 17 titik
  prefs.end();
  Serial.println("[ADMIN] Kalibrasi disimpan");
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "text/plain", "OK");
}

// ================================================================
//  ADMIN: AMBIL KALIBRASI SAAT INI
// ================================================================
void handleAdminGetCal() {
  if (!isCPAuthorized()) { server.sendHeader("Access-Control-Allow-Origin","*"); server.send(401,"text/plain","Unauthorized"); return; }
  String json = "{\"lut\":[";
  for (int i = 0; i < BATT_LUT_SIZE; i++) {
    json += "{\"w\":" + String(BATT_LUT_WEB[i],3) + ",\"a\":" + String(BATT_LUT_AVO[i],3) + "}";
    if (i < BATT_LUT_SIZE - 1) json += ",";
  }
  json += "],\"panelCal\":" + String(PANEL_CAL, 4) + "}";
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "application/json", json);
}

// ================================================================
//  ADMIN: SET LIMIT BIAYA (RUPIAH)
//
//  limit=0  -> nonaktifkan limit (semua relay kembali normal)
//  limit>0  -> begitu Rupiah (kWh x tarif) menyentuh angka ini,
//              SEMUA relay dikunci OFF sampai limit dinaikkan lagi
// ================================================================
void handleAdminSetBudget() {
  if (!isCPAuthorized()) { server.sendHeader("Access-Control-Allow-Origin","*"); server.send(401,"text/plain","Unauthorized"); return; }
  if (!server.hasArg("limit")) { server.send(400,"text/plain","Bad Request"); return; }
  float lim = server.arg("limit").toFloat();
  if (lim < 0) lim = 0;
  rpLimit = lim;
  prefs.begin("admin", false);
  prefs.putFloat("rpLimit", rpLimit);
  prefs.end();
  checkBudgetLimit();  // langsung evaluasi ulang, tanpa nunggu loop berikutnya
  Serial.printf("[ADMIN] Limit biaya diset ke Rp %.2f\n", rpLimit);
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "text/plain", "OK");
}

// ================================================================
//  ADMIN: AMBIL STATUS LIMIT BIAYA SAAT INI
// ================================================================
void handleAdminGetBudget() {
  if (!isCPAuthorized()) { server.sendHeader("Access-Control-Allow-Origin","*"); server.send(401,"text/plain","Unauthorized"); return; }
  String json = "{\"rpLimit\":" + String(rpLimit,2) +
                ",\"rpNow\":" + String(acEnergy * TARIFF_PER_KWH, 2) +
                ",\"locked\":" + String(budgetLocked ? 1 : 0) + "}";
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "application/json", json);
}

// ================================================================
//  ADMIN: SET BATAS MAKSIMAL ARUS AC (AMPERE)
//
//  limit=0  -> nonaktifkan batas arus
//  limit>0  -> begitu arus AC menyentuh angka ini, SEMUA relay TRIP
//              OFF dan tetap OFF walau arus turun lagi, sampai
//              di-reset manual lewat /admin/resetcurrent
// ================================================================
void handleAdminSetMaxCurrent() {
  if (!isCPAuthorized()) { server.sendHeader("Access-Control-Allow-Origin","*"); server.send(401,"text/plain","Unauthorized"); return; }
  if (!server.hasArg("limit")) { server.send(400,"text/plain","Bad Request"); return; }
  float lim = server.arg("limit").toFloat();
  if (lim < 0) lim = 0;
  maxAcCurrent = lim;
  prefs.begin("admin", false);
  prefs.putFloat("maxAcCur", maxAcCurrent);
  prefs.end();
  checkCurrentLimit();  // langsung evaluasi ulang, tanpa nunggu loop berikutnya
  Serial.printf("[ADMIN] Batas maksimal arus AC diset ke %.2fA\n", maxAcCurrent);
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "text/plain", "OK");
}

// ================================================================
//  ADMIN: AMBIL STATUS BATAS ARUS SAAT INI
// ================================================================
void handleAdminGetMaxCurrent() {
  if (!isCPAuthorized()) { server.sendHeader("Access-Control-Allow-Origin","*"); server.send(401,"text/plain","Unauthorized"); return; }
  String json = "{\"maxAcCurrent\":" + String(maxAcCurrent,2) +
                ",\"acCurrentNow\":" + String(acCurrent, 2) +
                ",\"tripped\":" + String(currentTripped ? 1 : 0) + "}";
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "application/json", json);
}

// ================================================================
//  ADMIN: RESET TRIP ARUS (buka kunci relay setelah over-current)
// ================================================================
void handleAdminResetCurrent() {
  if (!isCPAuthorized()) { server.sendHeader("Access-Control-Allow-Origin","*"); server.send(401,"text/plain","Unauthorized"); return; }
  resetCurrentTrip();
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "text/plain", "OK");
}

// ================================================================
//  ADMIN: LOG AKSES
// ================================================================
void handleAdminLogs() {
  if (!isCPAuthorized()) { server.sendHeader("Access-Control-Allow-Origin","*"); server.send(401,"text/plain","Unauthorized"); return; }
  String json = "[";
  for (int i = logCount - 1; i >= 0; i--) {
    if (i < logCount - 1) json += ",";
    json += "{\"ip\":\"" + String(accessLogs[i].ip) + "\",";
    json += "\"waktu\":\"" + String(accessLogs[i].waktu) + "\",";
    json += "\"ok\":" + String(accessLogs[i].berhasil ? "true" : "false") + "}";
  }
  json += "]";
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200, "application/json", json);
}

// ================================================================
//  ESTIMASI SISA BATERAI (menit) — dipakai bersama oleh web & LCD
// ================================================================
// Konsep: sisaEnergiWh (Wh tersisa di baterai sesuai level % saat ini)
//         dibagi dengan daya yang sedang dipakai (Watt) -> ketemu jam,
//         lalu dikonversi ke menit.
// Sesuai wiring fisik (VIN+ ke baterai, VIN- ke beban):
// powerRaw POSITIF artinya baterai sedang discharge (disuplai ke beban).
// powerRaw NEGATIF/NOL artinya baterai sedang charge / idle (tidak ada
// beban -> estimasi waktu habis tidak relevan, tampilkan "Charging/Idle").
long computeEstMin(float bp) {
  long estMin = -1;  // default: charging / idle / tidak ada beban berarti
  if (inaBattOK) {
    if (powerRaw > 0.5f) {  // discharge dengan beban signifikan (>0.5W)
      float sisaEnergiWh = BATT_CAPACITY_AH * voltageRaw * (bp / 100.0f);
      estMin = (long)((sisaEnergiWh / powerRaw) * 60.0f);
    }
  }
  return estMin;
}

// ================================================================
//  WEB: HANDLE DATA (JSON)
// ================================================================
// Bangun JSON data sensor/status terkini. Dipakai bersama oleh handleData()
// (HTTP, untuk fallback/reload pertama) dan broadcastSensorData() (WebSocket,
// untuk push realtime).
String buildDataJson() {
  float bp = battSOC(voltageRaw);
  long estMin = computeEstMin(bp);

  String json = "{";
  json += "\"v\":"    + String(voltageRaw, 2) + ",";
  json += "\"vp\":"   + String(panelVolt,  2) + ",";
  json += "\"i\":"    + String(currentRaw, 3) + ",";
  json += "\"p\":"    + String(powerRaw,   2) + ",";
  json += "\"b\":"    + String((int)bp)        + ",";
  json += "\"e\":"    + String(energyWh,   2) + ",";
  json += "\"bah\":"  + String(BATT_CAPACITY_AH, 1) + ",";
  json += "\"bwh\":"  + String(battWhNow,  1) + ",";
  json += "\"estMin\":" + String(estMin) + ",";
  json += "\"dAh\":"  + String(dischargeAhTotal, 3) + ",";
  json += "\"cAh\":"  + String(chargeAhTotal,    3) + ",";
  json += "\"inaBatt\":" + String(inaBattOK ? 1 : 0) + ",";
  json += "\"pi\":"   + String(panelCurrentRaw, 3) + ",";
  json += "\"pp\":"   + String(panelPowerRaw,   2) + ",";
  json += "\"pe\":"   + String(panelEnergyWh,   2) + ",";
  json += "\"acV\":"  + String(acVoltage,  1) + ",";
  json += "\"acI\":"  + String(acCurrent,  2) + ",";
  json += "\"acP\":"  + String(acPower,    1) + ",";
  json += "\"acF\":"  + String(acFreq,     1) + ",";
  json += "\"acPF\":" + String(acPF,       2) + ",";
  json += "\"acE\":"  + String(acEnergy,   3) + ",";
  json += "\"acRp\":" + String(acEnergy * TARIFF_PER_KWH, 2) + ",";
  json += "\"rpLimit\":" + String(rpLimit, 2) + ",";
  json += "\"budgetLocked\":" + String(budgetLocked ? 1 : 0) + ",";
  json += "\"maxAcCurrent\":" + String(maxAcCurrent, 2) + ",";
  json += "\"currentTripped\":" + String(currentTripped ? 1 : 0) + ",";
  json += "\"tsh\":"  + String(autoStartH)    + ",";
  json += "\"tsm\":"  + String(autoStartM)    + ",";
  json += "\"teh\":"  + String(autoEndH)      + ",";
  json += "\"tem\":"  + String(autoEndM)      + ",";
  json += "\"r1\":"   + String(relay1State ? 1 : 0) + ",";
  json += "\"r2\":"   + String(relay2State ? 1 : 0) + ",";
  json += "\"r3\":"   + String(relay3State ? 1 : 0) + ",";
  json += "\"r4\":"   + String(relay4State ? 1 : 0) + ",";
  json += "\"m1\":"   + String(manualR1 ? 1 : 0) + ",";
  json += "\"m2\":"   + String(manualR2 ? 1 : 0) + ",";
  json += "\"m3\":"   + String(manualR3 ? 1 : 0) + ",";
  json += "\"m4\":"   + String(manualR4 ? 1 : 0);
  json += "}";
  return json;
}

void handleData() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", buildDataJson());
}

// Dipanggil tiap kali ada event WebSocket (klien connect/disconnect/pesan masuk).
void webSocketEvent(uint8_t clientId, WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.printf("[WS] Klien #%u konek\n", clientId);
    String initialData = buildDataJson();
    webSocket.sendTXT(clientId, initialData);   // kirim data langsung begitu konek
  } else if (type == WStype_DISCONNECTED) {
    Serial.printf("[WS] Klien #%u putus\n", clientId);
  }
  // type == WStype_TEXT sengaja tidak dipakai -- kontrol relay/login tetap lewat
  // HTTP biasa (/relay, /login, dst), WebSocket ini cuma satu arah ESP32 -> web.
}

// Kirim data sensor terbaru ke SEMUA klien web yang sedang terhubung.
void broadcastSensorData() {
  String data = buildDataJson();
  webSocket.broadcastTXT(data);
}

// ================================================================
//  WEB: HANDLE RELAY
//
//  Semua relay (1-4) tetap bisa ditoggle manual dari web kapan saja.
//
//  Relay 1 (Inverter), Relay 2 (Lampu Utama) & Relay 3 (Output AC):
//  manual BERTAHAN (persisten) sampai user menekan tombol "AUTO"
//  (s=auto) — TIDAK di-override otomatis oleh PZEM/Timer.
//
//  Relay 4 (Lampu Panel): manual hanya SESAAT — otomatis "kalah"
//  oleh PZEM dalam ±1 detik, tanpa perlu menekan tombol AUTO.
// ================================================================
void handleRelay() {
  if (!isAuthorized()) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  if (!server.hasArg("r") || !server.hasArg("s")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  int  n  = server.arg("r").toInt();
  String sVal = server.arg("s");

  // s=auto → kembalikan relay ke mode otomatis (timer/PZEM) seketika
  if (sVal == "auto") {
    if (n == 1) manualR1 = false;
    else if (n == 2) manualR2 = false;
    else if (n == 3) manualR3 = false;
    else if (n == 4) manualR4 = false;
    Serial.printf("[MANUAL] R%d dikembalikan ke mode AUTO\n", n);
    checkAutoTime();
    checkAcVoltage();
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.send(200, "text/plain", "OK");
    return;
  }

  bool st = (sVal.toInt() == 1);

  // Set relay state DAN tandai sebagai manual override.
  // Untuk R1, R2, R3: override BERTAHAN sampai ditekan AUTO
  // (R3 juga bisa terlepas otomatis kalau kondisi timer berubah).
  // Untuk R4: override ini hanya sesaat, karena PZEM akan
  // otomatis menimpanya kembali di loop berikutnya (±1 detik).
  if (n == 1) { relay1State = st; manualR1 = true; }
  else if (n == 2) { relay2State = st; manualR2 = true; }
  else if (n == 3) { relay3State = st; manualR3 = true; }
  else if (n == 4) { relay4State = st; manualR4 = true; }

  applyRelay();
  bool sesaat = (n == 4);
  Serial.printf("[MANUAL] Relay %d -> %s (%s)\n",
                n, st ? "ON" : "OFF",
                sesaat ? "override sesaat, PZEM tetap jalan" : "override bertahan sampai AUTO");

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/plain", "OK");
}

// ================================================================
//  WEB: HANDLE SET TIMER
// ================================================================
void handleSetTimer() {
  if (!isAuthorized()) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  if (!server.hasArg("start_h") || !server.hasArg("start_m") ||
      !server.hasArg("end_h")   || !server.hasArg("end_m")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  int sh = server.arg("start_h").toInt();
  int sm = server.arg("start_m").toInt();
  int eh = server.arg("end_h").toInt();
  int em = server.arg("end_m").toInt();

  if (sh < 0 || sh > 23 || sm < 0 || sm > 59 ||
      eh < 0 || eh > 23 || em < 0 || em > 59) {
    server.send(400, "text/plain", "Invalid time");
    return;
  }
  autoStartH = sh;  autoStartM = sm;
  autoEndH   = eh;  autoEndM   = em;
  saveTimer();

  Serial.printf("[TIMER] Set → %02d:%02d - %02d:%02d\n", sh, sm, eh, em);

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "OK");
}

// ================================================================
//  LCD 16x2 I2C - MENAMPILKAN DATA MONITORING (SAMA SEPERTI WEB)
//
//  Karena layar hanya 16x2 (32 karakter total) sedangkan data monitoring
//  jauh lebih banyak, data dibagi menjadi beberapa "halaman" yang
//  bergantian otomatis setiap LCD_PAGE_INTERVAL. Isi halaman yang sedang
//  tampil tetap di-refresh tiap LCD_REFRESH_INTERVAL supaya angkanya
//  real-time (tidak menunggu ganti halaman).
// ================================================================
const unsigned long LCD_PAGE_INTERVAL    = 3000UL;  // ganti halaman tiap 3 detik
const unsigned long LCD_REFRESH_INTERVAL = 200UL;   // refresh angka tiap 0.2 detik (lebih real-time)
unsigned long lastLcdPageMillis    = 0;
unsigned long lastLcdRefreshMillis = 0;
int  lcdPage = 0;
const int LCD_TOTAL_PAGES = 15;

// Cetak satu baris LCD (0=atas,1=bawah) dan otomatis dipotong/diberi
// spasi supaya selalu pas 16 kolom (sisa karakter lama ikut tertimpa).
void lcdWriteLine(int row, const char* text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  buf[16] = '\0';
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// Format angka Rupiah jadi string dengan TITIK pemisah ribuan, contoh:
// 22653 -> "22.653", 1500000 -> "1.500.000". Diperlukan khusus untuk LCD
// karena, tidak seperti web (yang pakai toLocaleString('id-ID') di
// JavaScript), Arduino/C++ tidak punya fungsi format angka ber-locale
// bawaan -- kalau dicetak langsung pakai %ld angkanya akan menyatu semua
// tanpa pemisah (mis. "22653"), makanya perlu fungsi manual ini.
void formatRibuan(long value, char* out, size_t outSize) {
  char raw[16];
  snprintf(raw, sizeof(raw), "%ld", value);
  bool neg = (raw[0] == '-');
  int startIdx  = neg ? 1 : 0;
  int digitsLen = (int)strlen(raw) - startIdx;
  int dotCount  = (digitsLen - 1) / 3;
  int totalLen  = (int)strlen(raw) + dotCount;

  if ((size_t)(totalLen + 1) > outSize) {
    // Buffer kurang besar -- fallback tampilkan tanpa titik daripada terpotong aneh
    snprintf(out, outSize, "%ld", value);
    return;
  }

  out[totalLen] = '\0';
  int outPos = totalLen - 1;
  int digitCount = 0;
  for (int i = (int)strlen(raw) - 1; i >= startIdx; i--) {
    out[outPos--] = raw[i];
    digitCount++;
    if (digitCount % 3 == 0 && i > startIdx) {
      out[outPos--] = '.';
    }
  }
  if (neg) out[outPos] = '-';
}

// Format estimasi sisa baterai jadi teks pendek buat LCD (maks 16 kolom)
void formatEstLCD(long estMin, bool sensorOK, char *out, size_t outSize) {
  if (!sensorOK) {
    snprintf(out, outSize, "Cek Sensor INA");
  } else if (estMin < 0) {
    snprintf(out, outSize, "Charging/Idle");
  } else {
    long ej = estMin / 60, em = estMin % 60;
    if (ej > 99) snprintf(out, outSize, "Sisa: >99 jam");
    else         snprintf(out, outSize, "Sisa:%ldj %ldm", ej, em);
  }
}

void updateLCD() {
  // ---- OVERRIDE: proteksi over-current (TRIP) ----
  // Kalau arus AC pernah menyentuh/melebihi batas (currentTripped == true,
  // mirip MCB yang "jeglek"), LCD TIDAK menampilkan data apapun (baterai/
  // panel/AC/relay/dll) selama kondisi ini masih berlaku. Sebagai gantinya,
  // LCD HANYA menampilkan peringatan "TRIP !!!!" yang berkedip (nyala/kosong
  // bergantian), supaya kondisi bahaya ini tidak "tenggelam"/terlewat di
  // antara halaman-halaman data biasa yang bergantian. Tampilan normal baru
  // kembali otomatis setelah trip di-reset manual dari Control Panel
  // (tombol "RESET TRIP ARUS"), yang akan mengubah currentTripped kembali
  // jadi false.
  if (currentTripped) {
    bool blinkOn = ((millis() / 500UL) % 2UL) == 0UL;
    if (blinkOn) {
      lcdWriteLine(0, "   !! TRIP !!");
      lcdWriteLine(1, "  CEK ARUS AC!");
    } else {
      lcdWriteLine(0, "");
      lcdWriteLine(1, "");
    }
    return;  // skip semua halaman data biasa selama masih trip
  }

  float bp = battSOC(voltageRaw);
  long estMin = computeEstMin(bp);
  char l1[17], l2[17];

  switch (lcdPage) {
    case 0: {  // Baterai: tegangan, persen & estimasi sisa waktu
      char estStr[17];
      formatEstLCD(estMin, inaBattOK, estStr, sizeof(estStr));
      snprintf(l1, sizeof(l1), "%.1fV  %d%%", voltageRaw, (int)bp);
      snprintf(l2, sizeof(l2), "%s", estStr);
      break;
    }
    case 1:  // Baterai: arus, daya, energi hari ini & kapasitas (Wh)
      snprintf(l1, sizeof(l1), "I:%.2fA P:%.1fW", currentRaw, powerRaw);
      snprintf(l2, sizeof(l2), "E:%.0f K:%.0fWh", energyWh, battWhNow);
      break;
    case 2:  // Panel surya - tegangan
      snprintf(l1, sizeof(l1), "Panel Surya");
      snprintf(l2, sizeof(l2), "%.1fV", panelVolt);
      break;
    case 3:  // Panel surya - arus, daya & energi (Wh) jadi satu tampilan
      snprintf(l1, sizeof(l1), "I:%.2fA P:%.1fW", panelCurrentRaw, panelPowerRaw);
      snprintf(l2, sizeof(l2), "Energi:%.1fWh", panelEnergyWh);
      break;
    case 4:  // AC - tegangan & frekuensi (PLN)
      snprintf(l1, sizeof(l1), "AC/PLN (Listrik)");
      snprintf(l2, sizeof(l2), "%.0fV  %.1fHz", acVoltage, acFreq);
      break;
    case 5:  // AC - arus & daya
      snprintf(l1, sizeof(l1), "I:%.2fA P:%.0fW", acCurrent, acPower);
      snprintf(l2, sizeof(l2), "PF:%.2f", acPF);
      break;
    case 6: {  // AC - energi kWh & biaya Rupiah
      char rpStr[16];
      formatRibuan((long)(acEnergy * TARIFF_PER_KWH), rpStr, sizeof(rpStr));
      snprintf(l1, sizeof(l1), "Energi:%.3fkWh", acEnergy);
      snprintf(l2, sizeof(l2), "Rp:%s", rpStr);
      break;
    }
    case 7: {  // Limit biaya listrik (hanya nilai yang di-SET, bukan biaya sekarang)
      if (rpLimit > 0) {
        char rpLimStr[16];
        formatRibuan((long)rpLimit, rpLimStr, sizeof(rpLimStr));
        snprintf(l1, sizeof(l1), "Limit Biaya");
        snprintf(l2, sizeof(l2), "Rp%s %s", rpLimStr, budgetLocked ? "KUNCI" : "Aktif");
      } else {
        snprintf(l1, sizeof(l1), "Limit Biaya");
        snprintf(l2, sizeof(l2), "Nonaktif");
      }
      break;
    }
    case 8:  // Batas arus AC (proteksi over-current)
      if (maxAcCurrent > 0) {
        snprintf(l1, sizeof(l1), "Batas Arus AC");
        snprintf(l2, sizeof(l2), "%.1fA  %s", maxAcCurrent, currentTripped ? "TRIP!" : "Aman");
      } else {
        snprintf(l1, sizeof(l1), "Batas Arus AC");
        snprintf(l2, sizeof(l2), "Nonaktif");
      }
      break;
    case 9:  // Status Relay 1 & 2
      snprintf(l1, sizeof(l1), "Inverter   :%s", relay1State ? "ON" : "OFF");
      snprintf(l2, sizeof(l2), "Lampu Utama:%s", relay2State ? "ON" : "OFF");
      break;
    case 10:  // Status Relay 3 & 4
      snprintf(l1, sizeof(l1), "Output AC  :%s", relay3State ? "ON" : "OFF");
      snprintf(l2, sizeof(l2), "Lampu Panel:%s", relay4State ? "ON" : "OFF");
      break;
    case 11:  // Jadwal timer
      snprintf(l1, sizeof(l1), "T %02d:%02d-%02d:%02d",
               autoStartH, autoStartM, autoEndH, autoEndM);
      snprintf(l2, sizeof(l2), "Status: %s", timerActiveNow ? "AKTIF" : "OFF");
      break;
    case 12:  // Kapasitas asli baterai (coulomb counting, reset via tombol RESET fisik)
      snprintf(l1, sizeof(l1), "Ah- %.2f", dischargeAhTotal);
      snprintf(l2, sizeof(l2), "Ah+ %.2f", chargeAhTotal);
      break;
    case 13:  // IP WiFi rumah (STA)
      snprintf(l1, sizeof(l1), "IP WiFi (rumah)");
      if (WiFi.status() == WL_CONNECTED) {
        snprintf(l2, sizeof(l2), "%s", WiFi.localIP().toString().c_str());
      } else {
        snprintf(l2, sizeof(l2), "Belum terhubung");
      }
      break;
    case 14:  // IP Access Point
      snprintf(l1, sizeof(l1), "IP Access Point");
      snprintf(l2, sizeof(l2), "%s", WiFi.softAPIP().toString().c_str());
      break;
  }

  lcdWriteLine(0, l1);
  lcdWriteLine(1, l2);
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Smart Panel HLM25 ===");
  bootMillis = millis();

  // Load kalibrasi & password dari Preferences jika sudah pernah disimpan
  // Catatan: pakai flag BARU "calSaved17" (bukan "calSaved" lama) supaya
  // data kalibrasi LAMA (5 titik, dari sebelum upgrade ke 17 titik) TIDAK
  // ikut ke-load dan menimpa default 17 titik yang baru -- inilah penyebab
  // data "ngawur lagi" setelah reset kalau masih pakai flag lama.
  prefs.begin("admin", true);
  if (prefs.getBool("calSaved17", false)) {
    for (int i = 0; i < BATT_LUT_SIZE; i++) {
      BATT_LUT_WEB[i] = prefs.getFloat(("w"+String(i)).c_str(), BATT_LUT_WEB[i]);
      BATT_LUT_AVO[i] = prefs.getFloat(("a"+String(i)).c_str(), BATT_LUT_AVO[i]);
    }
    PANEL_CAL = prefs.getFloat("panelCal", PANEL_CAL);
    sortBattLUT();  // jaga urutan menaik supaya battCalibrate() tidak gagal
    Serial.println("[ADMIN] Kalibrasi 17 titik dimuat dari Preferences");
  } else {
    Serial.println("[ADMIN] Belum ada kalibrasi 17 titik tersimpan, pakai default di source code");
  }
  // Password LOGIN (kontrol relay & timer) — cek key baru dulu, kalau belum
  // ada pakai key lama "password" supaya password yang sudah pernah diset
  // sebelum update ini tidak hilang.
  String savedLoginPass = prefs.getString("loginpass", "");
  if (savedLoginPass.length() == 0) savedLoginPass = prefs.getString("password", "");
  if (savedLoginPass.length() > 0) { LOGIN_PASSWORD = savedLoginPass; }
  // Password CONTROL PANEL (terpisah)
  String savedCPPass = prefs.getString("cppass", "");
  if (savedCPPass.length() > 0) { CP_PASSWORD = savedCPPass; }
  // Limit biaya listrik (Rupiah) — 0 = nonaktif jika belum pernah diset
  rpLimit = prefs.getFloat("rpLimit", 0);
  // Batas maksimal arus AC (Ampere) — 0 = nonaktif jika belum pernah diset.
  // currentTripped SENGAJA tidak disimpan/dimuat di sini -- kalau ESP32
  // restart, status trip otomatis kembali normal (akan langsung dievaluasi
  // ulang di loop() begitu data PZEM pertama masuk).
  maxAcCurrent = prefs.getFloat("maxAcCur", 0);
  prefs.end();

  // Peringatan kalau password belum diisi di source code (dan belum pernah
  // diganti lewat Control Panel) -- login akan selalu ditolak.
  if (LOGIN_PASSWORD.length() == 0 || CP_PASSWORD.length() == 0) {
    Serial.println("[PERINGATAN] LOGIN_PASSWORD / CP_PASSWORD masih kosong -> login akan ditolak! Isi di source code lalu upload ulang.");
  }

  // Muat backup acEnergy (kWh) dari flash SEBELUM PZEM mulai dibaca -- supaya
  // kalau PZEM belum/tidak merespons setelah restart, web & LCD tetap
  // menampilkan angka kWh/Rupiah terakhir yang benar, bukan 0.
  loadEnergyFromFlash();

  // Relay awal semua OFF (HIGH = relay OFF untuk aktif LOW)
  pinMode(RELAY1, OUTPUT);  digitalWrite(RELAY1, HIGH);
  pinMode(RELAY2, OUTPUT);  digitalWrite(RELAY2, HIGH);
  pinMode(RELAY3, OUTPUT);  digitalWrite(RELAY3, HIGH);
  pinMode(RELAY4, OUTPUT);  digitalWrite(RELAY4, HIGH);

  // ADC pin baterai & panel surya
  // Sensor tegangan analog lama sudah dihapus, sekarang murni pakai INA219

  // LCD I2C 16x2
  Wire.begin(I2C_SDA, I2C_SCL);
  // Batasi timeout I2C (dalam ms) supaya kalaupun ada device I2C yang
  // "setengah nyambung" atau lepas di tengah jalan, Wire tidak hang selamanya.
  Wire.setTimeOut(1000);

  // Cek dulu apakah LCD benar-benar merespons di alamat LCD_ADDR SEBELUM
  // memanggil lcd.init()/lcd.print(). Kalau tidak dicek, dan LCD belum
  // tertancap, ESP32 bisa hang total nunggu ACK yang tidak akan pernah
  // datang -- itu sebabnya dulu web ikut mati saat LCD dilepas.
  Wire.beginTransmission(LCD_ADDR);
  lcdPresent = (Wire.endTransmission() == 0);
  if (lcdPresent) {
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("Smart Panel PLTS");
    lcd.setCursor(0, 1);
    lcd.print("  HLM25 - Boot..");
    Serial.println("[LCD] Terdeteksi, I2C siap (SDA=21, SCL=22)");
  } else {
    Serial.println("[LCD] TIDAK terdeteksi di alamat 0x27 -- dilewati (skip), sistem tetap jalan normal");
  }

  // INA219 #1 (Baterai, 0x40) & #2 (Panel Surya, 0x41)
  // Gabung di bus I2C yang sama dengan LCD (pin 21/22), dicek dulu presence-nya
  // via Wire supaya tidak hang kalau salah satu/kedua sensor belum terpasang.
  Wire.beginTransmission(0x40);
  inaBattOK = (Wire.endTransmission() == 0);
  if (inaBattOK) {
    inaBatt.begin();
    Serial.println("[INA219] Baterai (0x40) terdeteksi");
  } else {
    Serial.println("[INA219] Baterai (0x40) TIDAK terdeteksi -- dilewati (skip)");
  }

  Wire.beginTransmission(0x41);
  inaPanelOK = (Wire.endTransmission() == 0);
  if (inaPanelOK) {
    inaPanel.begin();
    Serial.println("[INA219] Panel Surya (0x41) terdeteksi");
  } else {
    Serial.println("[INA219] Panel Surya (0x41) TIDAK terdeteksi -- dilewati (skip)");
  }

  // PZEM
  Serial2.begin(9600, SERIAL_8N1, PZEM_RX, PZEM_TX);
  Serial.println("[PZEM] Serial2 siap (RX=16, TX=17)");

  // WiFi STA - pakai IP STATIS (supaya IP selalu sama, tidak berubah-ubah)
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.config(STA_LOCAL_IP, STA_GATEWAY, STA_SUBNET, STA_DNS1, STA_DNS2)) {
    Serial.println("[WiFi] Gagal set IP statis, lanjut pakai DHCP");
  }
  WiFi.begin(ssid, pass);
  Serial.print("[WiFi] Menghubungkan");
  unsigned long wt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wt < 20000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Terhubung -> IP STA (rumah): " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] Gagal, lanjut AP saja");
  }

  // NTP -- pakai 3 server (1 utama + 2 cadangan) supaya lebih tahan kalau
  // salah satu server lambat/tidak terjangkau dari jaringan ini.
  configTime(7 * 3600, 0, ntpServer, "pool.ntp.org", "time.google.com");

  // WiFi AP - IP diset eksplisit (default sama dgn bawaan ESP32: 192.168.4.1)
  WiFi.softAPConfig(AP_LOCAL_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(ap_ssid, ap_pass);
  Serial.println("[AP] SSID : " + String(ap_ssid));
  Serial.println("[AP] IP AP: " + WiFi.softAPIP().toString());

  // Web routes
  // CORS: supaya halaman web yang dibuka dari origin berbeda (mis. lewat tunnel)
  // tidak diblokir browser saat fetch() ke ESP32.
  server.on("/",                  [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleRoot(); });
  server.on("/data",              [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleData(); });
  server.on("/relay",             [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleRelay(); });
  server.on("/settimer",          [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleSetTimer(); });
  server.on("/login",             [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleLogin(); });
  server.on("/cplogin",           [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleCPLogin(); });
  server.on("/admin/status",      [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleAdminStatus(); });
  server.on("/admin/setpassword", [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleAdminSetPassword(); });
  server.on("/admin/setcppassword", [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleAdminSetCPPassword(); });
  server.on("/admin/setcal",      [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleAdminSetCal(); });
  server.on("/admin/getcal",      [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleAdminGetCal(); });
  server.on("/admin/setbudget",   [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleAdminSetBudget(); });
  server.on("/admin/getbudget",   [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleAdminGetBudget(); });
  server.on("/admin/setmaxcurrent", [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleAdminSetMaxCurrent(); });
  server.on("/admin/getmaxcurrent", [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleAdminGetMaxCurrent(); });
  server.on("/admin/resetcurrent",  [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleAdminResetCurrent(); });
  server.on("/admin/logs",        [](){ server.sendHeader("Access-Control-Allow-Origin","*"); handleAdminLogs(); });
  loadTimer();
  server.begin();
  Serial.println("[Web] Server started");

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("[WS] WebSocket server started di port 81");

  lastEnergyMillis = lastSensorMillis = lastPzemMillis = millis();
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
  server.handleClient();
  webSocket.loop();

  // Push data sensor ke semua klien web yang konek via WebSocket, tiap 500ms.
  static unsigned long lastBroadcast = 0;
  if (millis() - lastBroadcast >= 500UL) {
    lastBroadcast = millis();
    broadcastSensorData();
  }

  // Auto-reconnect WiFi kalau putus di tengah jalan (umum terjadi pada ESP32
  // setelah berjalan lama). Tanpa ini, ESP32 bisa "nyangkut" tidak konek lagi
  // sampai direstart manual, yang bisa membuat timer/NTP berhenti berfungsi.
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck >= 10000UL) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Terputus! Mencoba reconnect...");
      WiFi.disconnect();
      WiFi.reconnect();
    }
  }

  // Cek auto timer setiap 5 detik (realtime responsif)
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck >= 5000UL) {
    checkAutoTime();
    lastCheck = millis();
  }

  // ---- LCD 16x2 I2C: ganti halaman tiap 3 detik, refresh angka tiap 0.2 detik ----
  // Semua panggilan lcd.* dijaga dengan lcdPresent supaya tidak hang kalau
  // LCD belum/tidak tertancap secara fisik.
  if (lcdPresent) {
    if (millis() - lastLcdPageMillis >= LCD_PAGE_INTERVAL) {
      lastLcdPageMillis = millis();
      lcdPage = (lcdPage + 1) % LCD_TOTAL_PAGES;
      updateLCD();
    }
    if (millis() - lastLcdRefreshMillis >= LCD_REFRESH_INTERVAL) {
      lastLcdRefreshMillis = millis();
      updateLCD();
    }
  } else if (millis() - lastLcdScanMillis >= LCD_SCAN_INTERVAL) {
    // LCD belum terdeteksi -- coba scan ulang tiap 5 detik. Kalau LCD baru
    // ditancapkan setelah ESP32 nyala, ini akan otomatis mengaktifkannya
    // tanpa perlu restart ESP32.
    lastLcdScanMillis = millis();
    Wire.beginTransmission(LCD_ADDR);
    if (Wire.endTransmission() == 0) {
      lcdPresent = true;
      lcd.init();
      lcd.backlight();
      lcdPage = 0;
      lastLcdPageMillis = millis();
      Serial.println("[LCD] Baru terdeteksi (hot-plug) -- diaktifkan");
    }
  }

  // Baca INA219 & hitung energi setiap 200ms (disamakan dgn LCD_REFRESH_INTERVAL
  // biar LCD benar-benar dapat data baru tiap kali refresh, bukan cuma gambar ulang
  // angka yang sama)
  if (millis() - lastSensorMillis >= 200UL) {
    lastSensorMillis = millis();

    // ---- INA219 #1 — tegangan & arus BATERAI ----
    float vBattRaw = 0, iBattRaw = 0;
    if (inaBattOK) {
      vBattRaw = inaBatt.getBusVoltage_V();
      // Wiring fisik: VIN+ ke kutub (+) baterai, VIN- ke kutub (+) beban.
      // Dengan pemasangan ini, INA219 membaca arus POSITIF saat discharge
      // (baterai menyuplai ke beban) dan NEGATIF saat charging. Ini beda
      // dari asumsi lama (+ = charging), makanya seluruh logika yang
      // bergantung ke tanda powerRaw (estimasi sisa baterai, akumulasi
      // energi harian) disesuaikan ke konvensi ini -- BUKAN dengan
      // membalik tanda di sini, supaya angka yang tampil di web tetap
      // POSITIF & enak dilihat saat ada beban aktif (kondisi paling umum).
      iBattRaw = inaBatt.getCurrent_mA() / 1000.0f;  // mA -> A
    }
    if (vBattRaw < 0) vBattRaw = 0;
    float vBattCal = battCalibrate(vBattRaw);  // koreksi dengan lookup table (kalibrasi ulang pakai data INA219 kamu)
    Serial.printf("[BATT-RAW] raw=%.3f  ->  calibrated=%.3f  (bandingkan raw ini dengan AVO)\n", vBattRaw, vBattCal);
    // Smoothing EMA supaya pembacaan tidak melompat-lompat
    static bool battInit = false;
    if (!battInit) { voltageRaw = vBattCal; currentRaw = iBattRaw; battInit = true; }
    else {
      voltageRaw = voltageRaw * 0.85f + vBattCal * 0.15f;
      currentRaw = currentRaw * 0.85f + iBattRaw * 0.15f;
    }
    if (fabs(currentRaw) < CURRENT_NOISE_FLOOR) currentRaw = 0;  // buang noise kecil di sekitar 0
    powerRaw   = voltageRaw * currentRaw;                 // daya baterai (W)
    battWhNow  = BATT_CAPACITY_AH * voltageRaw;            // 30Ah x tegangan real-time (Wh)

    // ---- INA219 #2 — tegangan & arus PANEL SURYA ----
    float vPanelRaw = 0, iPanelRaw = 0;
    if (inaPanelOK) {
      vPanelRaw = inaPanel.getBusVoltage_V() * PANEL_CAL;  // PANEL_CAL tetap dipakai buat fine-tuning kecil
      iPanelRaw = inaPanel.getCurrent_mA() / 1000.0f;
    }
    if (vPanelRaw < 0) vPanelRaw = 0;
    static bool panelInit = false;
    if (!panelInit) { panelVolt = vPanelRaw; panelCurrentRaw = iPanelRaw; panelInit = true; }
    else {
      panelVolt = panelVolt * 0.85f + vPanelRaw * 0.15f;
      panelCurrentRaw = panelCurrentRaw * 0.85f + iPanelRaw * 0.15f;
    }
    if (fabs(panelCurrentRaw) < CURRENT_NOISE_FLOOR) panelCurrentRaw = 0;  // buang noise kecil di sekitar 0

    // ---- AKAL-AKALAN SEMENTARA (software workaround) ----
    // Ini BUKAN solusi permanen -- solusi aslinya tetap pasang dioda Schottky
    // (seri di VIN- -> PV+ SCC) + resistor pull-down 100k (VIN+ -> GND), yang
    // membereskan akar masalah floating/leakage di hardware. Sebelum
    // komponen itu terpasang, kita akal-akali lewat logika: kalau arus panel
    // surya terbaca 0 (sudah lewat noise floor di atas), itu tandanya
    // memang tidak ada matahari sama sekali -- jadi tegangan panel yang
    // "nyangkut"/nempel ke tegangan baterai (akibat floating/leakage) kita
    // paksa jadi 0V juga, di web MAUPUN di LCD (keduanya baca variabel
    // panelVolt yang sama). Begitu dioda & resistor sudah dipasang, baris
    // if di bawah ini boleh dihapus lagi karena datanya sudah akurat asli
    // dari hardware.
    if (panelCurrentRaw == 0) {
      panelVolt = 0;
    }

    panelPowerRaw = panelVolt * panelCurrentRaw;            // daya panel surya (W)

    // ---- Akumulasi energi (Wh) berjalan, integrasi terhadap waktu ----
    // "Energi Hari Ini (Baterai)" = energi yang MASUK ke baterai (charging).
    // Sesuai konvensi baru (powerRaw POSITIF = discharge ke beban), maka
    // kondisi charging sekarang ada di powerRaw NEGATIF -> pakai fabs().
    static unsigned long lastEnergyCalcMillis = 0;
    unsigned long nowMs = millis();
    if (lastEnergyCalcMillis != 0) {
      float dtHours = (nowMs - lastEnergyCalcMillis) / 3600000.0f;
      if (powerRaw < 0)      energyWh      += fabs(powerRaw) * dtHours;
      if (panelPowerRaw > 0) panelEnergyWh += panelPowerRaw * dtHours;

      // ---- Coulomb counting: Ah asli yang keluar/masuk baterai ----
      // currentRaw POSITIF = discharge (ke beban), NEGATIF = charging
      // (konvensi sama dengan powerRaw, lihat catatan wiring INA219 di atas).
      if (currentRaw > 0)      dischargeAhTotal += currentRaw * dtHours;
      else if (currentRaw < 0) chargeAhTotal    += fabs(currentRaw) * dtHours;
    }
    lastEnergyCalcMillis = nowMs;

    // ---- Reset energi harian tiap pergantian tanggal (jam 00:00) ----
    struct tm ti;
    if (getLocalTime(&ti, 50)) {
      if (lastEnergyResetDay != ti.tm_mday) {
        if (lastEnergyResetDay != -1) {   // jangan reset di boot pertama
          energyWh = 0;
          panelEnergyWh = 0;
          saveEnergyToFlash();  // simpan reset ke flash juga -- supaya kalau ESP
                                 // restart tepat setelah lewat tengah malam, tidak
                                 // ke-load lagi angka Wh hari kemarin dari flash
          Serial.println("[ENERGI] Reset harian (pergantian tanggal) -- disimpan ke flash");
        }
        lastEnergyResetDay = ti.tm_mday;
      }
    }

    unsigned long now = millis();
    lastEnergyMillis = now;
  }


  // Baca PZEM setiap 1 detik
  if (millis() - lastPzemMillis >= 1000UL) {
    lastPzemMillis = millis();

    float v  = pzem.voltage();
    float c  = pzem.current();
    float pw = pzem.power();
    float f  = pzem.frequency();
    float pf = pzem.pf();
    float e  = pzem.energy();   // kWh akumulasi total, tersimpan di memori internal PZEM

    // Fix: jika PZEM tidak terdeteksi (voltage NaN/0)
    // reset data AC sesaat (V/I/P/F/PF) ke 0 agar tidak stuck di nilai lama.
    // KHUSUS acEnergy TIDAK direset ke 0 di sini — nilai lama tetap
    // dipertahankan selama pembacaan gagal, karena angka kWh bersifat
    // akumulatif (bukan nilai instan) dan tersimpan permanen di chip PZEM,
    // bukan di RAM ESP32. Jadi walau ESP32 restart/reset, begitu PZEM
    // terbaca lagi maka acEnergy otomatis kembali menampilkan angka
    // akumulasi yang benar (bukan mulai dari 0 lagi).
    if (isnan(v) || v <= 0) {
      acVoltage = 0; acCurrent = 0; acPower = 0;
      acFreq = 0;   acPF      = 0;
    } else {
      acVoltage = v;
      acCurrent = (!isnan(c)  && c  >= 0) ? c  : 0;
      acPower   = (!isnan(pw) && pw >= 0) ? pw : 0;
      acFreq    = (!isnan(f)  && f  >  0) ? f  : 0;
      acPF      = (!isnan(pf) && pf >= 0) ? pf : 0;
    }

    // acEnergy diupdate terpisah dari blok if/else di atas supaya nilai
    // terakhir yang valid tetap dipertahankan jika sesaat pembacaan gagal.
    if (!isnan(e) && e >= 0) {
      acEnergy = e;
    }

    // Cek logika PZEM hanya jika ada data valid
    checkAcVoltage(); // Cek perubahan PLN setiap data baru
    checkBudgetLimit(); // Cek limit biaya (Rupiah) setiap data PZEM baru
    checkCurrentLimit(); // Cek batas maksimal arus AC (proteksi over-current)
  }

  // Backup acEnergy (kWh) ke flash secara berkala -- lihat penjelasan lengkap
  // di deklarasi lastSavedAcEnergy di atas. Ini yang membuat kWh/Rupiah TIDAK
  // lagi tampil 0000 kalau ESP32 restart persis saat PZEM sedang mati.
  if ((millis() - lastEnergySaveMillis >= ENERGY_SAVE_INTERVAL) &&
      (fabs(acEnergy - lastSavedAcEnergy) >= ENERGY_SAVE_THRESHOLD ||
       fabs(energyWh - lastSavedEnergyWh) >= ENERGY_WH_SAVE_THRESHOLD ||
       fabs(panelEnergyWh - lastSavedPanelEnergyWh) >= ENERGY_WH_SAVE_THRESHOLD)) {
    saveEnergyToFlash();
  }
}
