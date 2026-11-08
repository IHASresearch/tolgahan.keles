#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_now.h>

// ===== AYARLAR =====
#define AP_SSID     "UAV_LINK"
#define AP_PASS     "uavlink123"
#define NORMAL_MS   60000     // 60 sn sakin
#define ATAK_MS     30000     // 30 sn kanal doldurma
#define FLOOD_PORT  4211      // server'in dinlemedigi bir port (sadece hava mesgul olsun)
#define BATCH       50        // her dongude gonderilen paket sayisi
// ===================

// Ucuncu ESP: atak uretecidir. Kendi takvimini isletir, atak penceresinde
// kanali yogun trafikle doldurur ve her faz degisiminde server'a ESP-NOW ile
// "normal/atak" bilgisini yollar. Boylece etiketleme tamamen otomatik olur.

WiFiUDP udp;
uint8_t broadcastMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint8_t floodBuf[1400];
bool     atak = false;
uint32_t fazBaslangic = 0;

// Faz degisimini server'a bildir (atak yokken gonderildigi icin guvenle ulasir)
void etiketYolla(uint8_t label) {
  for (int i = 0; i < 5; i++) {
    esp_now_send(broadcastMac, &label, 1);
    delay(20);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  memset(floodBuf, 0xAB, sizeof(floodBuf));

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(AP_SSID, AP_PASS);
  Serial.print("# Baglaniliyor");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
  }
  Serial.printf("\n# Baglandi, IP: %s\n", WiFi.localIP().toString().c_str());

  if (esp_now_init() != ESP_OK) {
    Serial.println("# ESP-NOW baslatilamadi!");
    while (1) delay(1000);
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastMac, 6);
  peer.channel = 0;      // mevcut kanal (AP ile ayni)
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  fazBaslangic = millis();
  etiketYolla(0);
  Serial.printf("# Takvim: %d sn normal / %d sn atak, dongu. Baslangic: NORMAL\n",
                NORMAL_MS / 1000, ATAK_MS / 1000);
}

void loop() {
  uint32_t simdi = millis();

  if (!atak && simdi - fazBaslangic >= NORMAL_MS) {
    atak = true;  fazBaslangic = simdi;
    etiketYolla(1);
    Serial.println("# >>> ATAK (kanal doldurma basladi)");
  } else if (atak && simdi - fazBaslangic >= ATAK_MS) {
    atak = false; fazBaslangic = simdi;
    etiketYolla(0);
    Serial.println("# <<< NORMAL");
  }

  // Atak penceresinde kanali doldur: hizli UDP broadcast akisi -> gercek cakisma
  if (atak && WiFi.status() == WL_CONNECTED) {
    for (int i = 0; i < BATCH; i++) {
      udp.beginPacket(IPAddress(255, 255, 255, 255), FLOOD_PORT);
      udp.write(floodBuf, sizeof(floodBuf));
      udp.endPacket();
    }
  }
  delay(1);
}
