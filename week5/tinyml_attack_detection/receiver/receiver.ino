#include <WiFi.h>
#include <WiFiUdp.h>
#include <math.h>
#include "esp_wifi.h"

// ===== AYARLAR =====
#define AP_SSID          "UAV_LINK"
#define AP_PASS          "uavlink123"
#define AP_KANAL         1
#define SERVER_PORT      4210
#define PENCERE_MS       1000
#define PENCERE_BEKLENEN 100    // client saniyede 100 paket
#define HEDEF_NORMAL     500
#define HEDEF_ATAK       500
// ===================

// SERVER: AP kurar, client'tan gelen UDP paketlerini dinler ve her 1 sn'lik
// pencere icin agin durumunu tek bir CSV satiri olarak basar.
// Etiket client'in paketinden gelir. Faz degisimine denk gelen (karisik etiketli)
// pencereler atlanir. 500 normal + 500 atak satira ulasinca kayit durur.

const char *BASLIK =
  "ts,received,lost,loss_ratio,max_consec_lost,avg_gap_ms,gap_std_ms,max_gap_ms,"
  "avg_rssi,rssi_std,min_rssi,avg_noise,avg_snr,min_snr,retry_ratio,sniff_frames,label";

struct __attribute__((packed)) Paket {
  uint32_t seq;
  uint8_t  label;
};

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
float    gapToplam = 0, gapKareToplam = 0, gapMax = 0;
uint32_t gapSayisi = 0;
uint32_t sonSeq = 0, sonAlimUs = 0;
bool     seqVar = false;
uint32_t alacak = 0;        // bos pencerelerde yazilan kayip, sonraki pakette tekrar sayilmasin

// Etiket takibi
int8_t   pencereEtiket = -1;   // -1 = bu pencerede henuz paket yok
bool     karisik = false;
uint8_t  sonEtiket = 0;
uint32_t sayac[2] = { 0, 0 };
bool     bitti = false;

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

void paketIsle(const Paket &p) {
  uint32_t simdiUs = micros();

  if (!seqVar || p.seq <= sonSeq) {
    seqVar = true;                       // ilk paket ya da client yeniden basladi
  } else {
    uint32_t bosluk = p.seq - sonSeq - 1;
    if (bosluk > pencereMaxArdisik) pencereMaxArdisik = bosluk;
    uint32_t dus = min(bosluk, alacak);
    alacak -= dus;
    pencereKayip += bosluk - dus;

    // Paketler arasi gecen sure (ms)
    float gap = (simdiUs - sonAlimUs) / 1000.0f;
    gapToplam += gap; gapKareToplam += gap * gap;
    if (gap > gapMax) gapMax = gap;
    gapSayisi++;
  }
  sonSeq = p.seq;
  sonAlimUs = simdiUs;
  pencereAlinan++;

  uint8_t lbl = p.label ? 1 : 0;
  if (pencereEtiket == -1) pencereEtiket = lbl;
  else if (pencereEtiket != lbl) karisik = true;
  sonEtiket = lbl;
}

void pencereSifirla() {
  pencereAlinan = pencereKayip = pencereMaxArdisik = 0;
  gapToplam = gapKareToplam = gapMax = 0;
  gapSayisi = 0;
  pencereEtiket = -1;
  karisik = false;
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

  if (!seqVar) { pencereSifirla(); return; }   // henuz hic paket gelmedi

  // Hic paket gelmediyse beklenenlerin hepsi kayip, etiket son bilinen faz
  if (pencereAlinan == 0) {
    pencereKayip = PENCERE_BEKLENEN;
    pencereMaxArdisik = PENCERE_BEKLENEN;
    alacak += PENCERE_BEKLENEN;
    gapMax = PENCERE_MS;
    pencereEtiket = sonEtiket;
  }

  // Faz degisimine denk gelen pencere: iki etiket karisik, atla
  if (karisik) {
    Serial.println("# Faz gecisi penceresi atlandi");
    pencereSifirla();
    return;
  }

  uint8_t label = pencereEtiket;
  uint32_t hedef = label ? HEDEF_ATAK : HEDEF_NORMAL;
  if (sayac[label] >= hedef) { pencereSifirla(); return; }   // bu sinif doldu

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

  float avgGap = PENCERE_MS, gapStd = 0;
  if (gapSayisi > 0) {
    avgGap = gapToplam / gapSayisi;
    gapStd = sqrtf(fmaxf(gapKareToplam / gapSayisi - avgGap * avgGap, 0));
  }

  uint32_t toplamPaket = pencereAlinan + pencereKayip;
  float kayipOrani = toplamPaket ? (float)pencereKayip / toplamPaket : 0;

  Serial.printf("%lu,%lu,%lu,%.2f,%lu,%.1f,%.1f,%.1f,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f,%.2f,%lu,%u\n",
                millis(), pencereAlinan, pencereKayip, kayipOrani, pencereMaxArdisik,
                avgGap, gapStd, gapMax,
                avgRssi, rssiStd, minRssi, avgNoise, avgSnr, minSnr,
                retryOrani, sayi, label);

  sayac[label]++;
  if (sayac[0] >= HEDEF_NORMAL && sayac[1] >= HEDEF_ATAK) {
    bitti = true;
    Serial.printf("# TAMAM: normal=%lu atak=%lu\n", sayac[0], sayac[1]);
  }
  pencereSifirla();
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

  Serial.printf("# AP: %s, IP: %s, port: %d\n", AP_SSID, WiFi.softAPIP().toString().c_str(), SERVER_PORT);
  Serial.printf("# Hedef: %d normal + %d atak satir. 'h' = baslik, 'r' = sayaclari sifirla\n",
                HEDEF_NORMAL, HEDEF_ATAK);
  Serial.println(BASLIK);
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'h') Serial.println(BASLIK);
    if (c == 'r') { sayac[0] = sayac[1] = 0; bitti = false; Serial.println("# Sayaclar sifirlandi"); }
  }

  int boyut;
  while ((boyut = udp.parsePacket()) > 0) {
    Paket p;
    if (boyut == sizeof(p) && udp.read((uint8_t *)&p, sizeof(p)) == sizeof(p)) {
      paketIsle(p);
    } else {
      udp.flush();
    }
  }

  static uint32_t sonPencere = 0;
  if (millis() - sonPencere >= PENCERE_MS) {
    sonPencere += PENCERE_MS;
    if (clientVar && !bitti) pencereBas();
  }
  delay(1);
}
