#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_now.h>
#include <math.h>
#include "esp_wifi.h"

// ===== AYARLAR =====
#define AP_SSID          "UAV_LINK"
#define AP_PASS          "uavlink123"
#define AP_KANAL         1
#define SERVER_PORT      4210
#define PENCERE_MS       1000
#define PENCERE_BEKLENEN 10     // client saniyede 10 paket gonderiyor
// ===================

const char *BASLIK =
  "ts,received,lost,loss_ratio,avg_rssi,rssi_std,min_rssi,"
  "avg_noise,avg_snr,min_snr,retry_ratio,sniff_frames,max_consec_lost,label";

WiFiUDP udp;

// Client MAC'i baglaninca ogrenilir, PHY olcumleri sadece ondan gelen cercevelerden alinir
uint8_t clientMac[6];
volatile bool clientVar = false;

// Promiscuous callback ile paylasilan toplamlar
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
float    rssiToplam = 0, rssiKareToplam = 0, rssiMin = 0;
float    noiseToplam = 0, snrToplam = 0, snrMin = 0;
uint32_t sniffSayisi = 0, retrySayisi = 0;

// Pencere istatistikleri (UDP tarafi)
uint32_t pencereAlinan = 0, pencereKayip = 0, pencereMaxArdisik = 0;
uint32_t sonSeq = 0;
bool     seqVar = false;
uint32_t alacak = 0;        // bos pencerelerde yazilan kayip, sonraki pakette tekrar sayilmasin
uint8_t  label = 0;         // server'da 'n'/'a' ile ayarlanir

// Son gecerli PHY degerleri (frame gelmeyen pencerede bunlar yazilir)
float sonRssi = 0, sonNoise = -96, sonSnr = 0;

void wifiOlay(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    memcpy(clientMac, info.wifi_ap_staconnected.mac, 6);
    clientVar = true;
    Serial.printf("# Client baglandi: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  clientMac[0], clientMac[1], clientMac[2], clientMac[3], clientMac[4], clientMac[5]);
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    clientVar = false;
    seqVar = false;
    alacak = 0;
    Serial.println("# Client ayrildi");
  }
}

// Flooder'dan gelen otomatik etiket (ESP-NOW broadcast)
void espNowAlindi(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len >= 1) {
    label = data[0] ? 1 : 0;
    Serial.printf("# [oto] etiket = %u\n", label);
  }
}

void snifferCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (!clientVar || type != WIFI_PKT_DATA) return;
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  wifi_pkt_rx_ctrl_t *rx = &pkt->rx_ctrl;
  if (rx->sig_len < 24) return;
  if (memcmp(pkt->payload + 10, clientMac, 6) != 0) return;   // addr2 = gonderen

  float rssi  = rx->rssi;
  float noise = rx->noise_floor;
  float snr   = rssi - noise;
  bool  retry = (pkt->payload[1] >> 3) & 0x1;                 // FC retry biti (bit 11)

  portENTER_CRITICAL_ISR(&mux);
  if (sniffSayisi == 0) { rssiMin = rssi; snrMin = snr; }
  else { if (rssi < rssiMin) rssiMin = rssi; if (snr < snrMin) snrMin = snr; }
  rssiToplam += rssi;  rssiKareToplam += rssi * rssi;
  noiseToplam += noise; snrToplam += snr;
  if (retry) retrySayisi++;
  sniffSayisi++;
  portEXIT_CRITICAL_ISR(&mux);
}

void paketIsle(uint32_t seq) {
  if (!seqVar || seq <= sonSeq) {
    seqVar = true;                       // ilk paket ya da client yeniden basladi
  } else {
    uint32_t bosluk = seq - sonSeq - 1;
    if (bosluk > pencereMaxArdisik) pencereMaxArdisik = bosluk;
    uint32_t dus = min(bosluk, alacak);
    alacak -= dus;
    pencereKayip += bosluk - dus;
  }
  sonSeq = seq;
  pencereAlinan++;
}

void pencereBas() {
  float rT, rKT, rMin, nT, sT, sMin;
  uint32_t sayi, retry;
  portENTER_CRITICAL(&mux);
  rT = rssiToplam; rKT = rssiKareToplam; rMin = rssiMin;
  nT = noiseToplam; sT = snrToplam; sMin = snrMin;
  sayi = sniffSayisi; retry = retrySayisi;
  rssiToplam = rssiKareToplam = noiseToplam = snrToplam = 0;
  sniffSayisi = retrySayisi = 0;
  portEXIT_CRITICAL(&mux);

  // Hic paket gelmediyse beklenenlerin hepsi kayip
  if (pencereAlinan == 0 && seqVar) {
    pencereKayip = PENCERE_BEKLENEN;
    pencereMaxArdisik = PENCERE_BEKLENEN;
    alacak += PENCERE_BEKLENEN;
  }

  float avgRssi = sonRssi, rssiStd = 0, minRssi = sonRssi;
  float avgNoise = sonNoise, avgSnr = sonSnr, minSnr = sonSnr, retryOrani = 0;
  if (sayi > 0) {
    avgRssi  = rT / sayi;
    rssiStd  = sqrtf(fmaxf(rKT / sayi - avgRssi * avgRssi, 0));
    minRssi  = rMin;
    avgNoise = nT / sayi;
    avgSnr   = sT / sayi;
    minSnr   = sMin;
    retryOrani = (float)retry / sayi;
    sonRssi = avgRssi; sonNoise = avgNoise; sonSnr = avgSnr;
  }

  uint32_t toplamPaket = pencereAlinan + pencereKayip;
  float kayipOrani = toplamPaket ? (float)pencereKayip / toplamPaket : 0;

  Serial.printf("%lu,%lu,%lu,%.2f,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f,%.2f,%lu,%lu,%u\n",
                millis(), pencereAlinan, pencereKayip, kayipOrani,
                avgRssi, rssiStd, minRssi, avgNoise, avgSnr, minSnr,
                retryOrani, sayi, pencereMaxArdisik, label);

  pencereAlinan = pencereKayip = pencereMaxArdisik = 0;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.onEvent(wifiOlay);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, AP_KANAL);
  WiFi.setSleep(false);
  udp.begin(SERVER_PORT);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&snifferCallback);
  wifi_promiscuous_filter_t filtre = { .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA };
  esp_wifi_set_promiscuous_filter(&filtre);

  // Flooder faz bilgisini ESP-NOW ile yollar -> etiket otomatik
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(espNowAlindi);
    Serial.println("# ESP-NOW hazir: etiket flooder'dan otomatik gelecek");
  } else {
    Serial.println("# ESP-NOW baslatilamadi, etiketi n/a ile elle verin");
  }

  Serial.printf("# AP: %s, IP: %s, port: %d\n", AP_SSID, WiFi.softAPIP().toString().c_str(), SERVER_PORT);
  Serial.println("# Elle override: n = normal, a = atak, h = baslik");
  Serial.println(BASLIK);
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'n') { label = 0; Serial.println("# Mod: NORMAL (label=0)"); }
    if (c == 'a') { label = 1; Serial.println("# Mod: ATAK/PARAZIT (label=1)"); }
    if (c == 'h') Serial.println(BASLIK);
  }

  int boyut;
  while ((boyut = udp.parsePacket()) > 0) {
    uint32_t seq;
    if (boyut == sizeof(seq) && udp.read((uint8_t *)&seq, sizeof(seq)) == sizeof(seq)) {
      paketIsle(seq);
    } else {
      udp.flush();
    }
  }

  static uint32_t sonPencere = 0;
  if (millis() - sonPencere >= PENCERE_MS) {
    sonPencere += PENCERE_MS;
    if (clientVar) pencereBas();
  }
  delay(1);
}
