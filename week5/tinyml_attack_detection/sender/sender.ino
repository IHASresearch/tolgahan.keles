// CLIENT: server'a sabit hizda UDP paket yollar. Ayni kod hem ESP32 hem ESP8266'da calisir;
// karta ozel kisimlar asagida #if defined(ESP8266) ile ayrildi.
//
// Atak fazinda paket dusurme (drop) atagini simule eder: seq her pakette artar
// ama paketlerin bir kismi hic gonderilmez. Server bunu seq boslugu olarak gorur.
// Ayrica atakta radyo gercekten bozulur: TX gucu en dusuge iner ve PHY hizi yukseltilir.
// Boylece server'da RSSI duser, retry ve gercek havada kayip artar.
// Her paket o anki fazin etiketini tasir (0 = normal, 1 = atak), etiketleme otomatiktir.

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
  #include "esp_wifi.h"
#endif
#include <WiFiUdp.h>

// ===== AYARLAR =====
#define AP_SSID        "UAV_LINK"
#define AP_PASS        "uavlink123"
#define SERVER_PORT    4210
#define GONDERME_MS    10        // saniyede 100 paket
#define NORMAL_MS      30000     // 30 sn normal haberlesme
#define ATAK_MS        30000     // 30 sn paket dusurme atagi
#define DUSURME_MIN    20        // atakta her saniye %20..%80 arasi yeni bir dusurme orani secilir
#define DUSURME_MAX    80
// ===================

struct __attribute__((packed)) Paket {
  uint32_t seq;
  uint8_t  label;
};

WiFiUDP udp;
uint32_t seq = 0;
bool     atak = false;
uint32_t fazBaslangic = 0;
uint8_t  dusurmeOrani = 0;     // yuzde
uint32_t gonderilen = 0, dusurulen = 0;

// Faza gore radyo ayarlari (karta ozel)
void radyoAyarla(bool atakModu) {
#if defined(ESP8266)
  // Guc: 20.5 dBm -> 0 dBm (en dusuk). Hiz: normalde otomatik, atakta 54 Mbps sabit
  WiFi.setOutputPower(atakModu ? 0.0f : 20.5f);
  if (atakModu) wifi_set_user_fixed_rate(FIXED_RATE_MASK_STA, PHY_RATE_54);
  else          wifi_set_user_fixed_rate(FIXED_RATE_MASK_NONE, PHY_RATE_54);
#else
  // Guc: 19.5 dBm -> -1 dBm (en dusuk). Hiz: normalde 1 Mbps, atakta MCS7
  WiFi.setTxPower(atakModu ? WIFI_POWER_MINUS_1dBm : WIFI_POWER_19_5dBm);
  esp_wifi_config_80211_tx_rate(WIFI_IF_STA, atakModu ? WIFI_PHY_RATE_MCS7_SGI : WIFI_PHY_RATE_1M_L);
#endif
}

void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.mode(WIFI_STA);

#if defined(ESP8266)
  randomSeed(RANDOM_REG32);                  // donanim rastgele sayi uretici
  WiFi.setSleepMode(WIFI_NONE_SLEEP);        // guc tasarrufu kapali: paket araliklari sabit kalsin
#else
  randomSeed(esp_random());
  WiFi.setSleep(false);
#endif

  WiFi.setAutoReconnect(true);
  WiFi.begin(AP_SSID, AP_PASS);

  Serial.print("\n# Baglaniliyor");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
  }
  Serial.printf("\n# Baglandi, IP: %s, server: %s\n",
                WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
  Serial.printf("# Takvim: %d sn normal / %d sn drop atagi, dongu. Baslangic: NORMAL\n",
                NORMAL_MS / 1000, ATAK_MS / 1000);
  radyoAyarla(false);
  fazBaslangic = millis();
}

void loop() {
  uint32_t simdi = millis();

  // Faz degisimi
  if (!atak && simdi - fazBaslangic >= NORMAL_MS) {
    atak = true;  fazBaslangic = simdi;
    radyoAyarla(true);
    Serial.println("# >>> DROP ATAGI basladi");
  } else if (atak && simdi - fazBaslangic >= ATAK_MS) {
    atak = false; fazBaslangic = simdi;
    radyoAyarla(false);
    Serial.println("# <<< NORMAL");
  }

  // Atakta her saniye yeni dusurme orani + durum raporu
  static uint32_t sonSaniye = 0;
  if (simdi - sonSaniye >= 1000) {
    sonSaniye = simdi;
    dusurmeOrani = atak ? random(DUSURME_MIN, DUSURME_MAX + 1) : 0;
    Serial.printf("# [%s] gonderilen=%lu dusurulen=%lu oran=%%%u\n",
                  atak ? "ATAK" : "norm", (unsigned long)gonderilen, (unsigned long)dusurulen, dusurmeOrani);
    gonderilen = dusurulen = 0;
  }

  // GONDERME_MS'de bir paket
  static uint32_t sonGonderim = 0;
  if (simdi - sonGonderim < GONDERME_MS) return;
  sonGonderim = simdi;

  if (WiFi.status() != WL_CONNECTED) return;

  seq++;   // dusurulse de seq artar -> server tarafinda kayip gorunur

  if (atak && random(100) < dusurmeOrani) {
    dusurulen++;
    return;   // paket dusuruldu (gonderilmedi)
  }

  Paket p = { seq, (uint8_t)(atak ? 1 : 0) };
  udp.beginPacket(WiFi.gatewayIP(), SERVER_PORT);
  udp.write((const uint8_t *)&p, sizeof(p));
  udp.endPacket();
  gonderilen++;
}
