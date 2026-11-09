#include <WiFi.h>
#include <WiFiUdp.h>
#include <math.h>
#include "esp_wifi.h"
#include "model.h"

// ===== AYARLAR =====
#define AP_SSID          "UAV_LINK"
#define AP_PASS          "uavlink123"
#define AP_KANAL         1
#define SERVER_PORT      4210
#define PENCERE_MS       1000   // dataset 1 sn pencerelerle toplandi
#define PENCERE_BEKLENEN 100    // client saniyede 100 paket
// ===================

// DETECTOR: receiver.ino ile ayni olcumleri yapar, ama CSV yerine her pencerede
// egitilmis MLP'yi (model.h, train.py uretir) ESP32 uzerinde calistirir ve
// tahmini client'in gercek etiketiyle karsilastirir.

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
float    rssiToplam = 0, rssiKareToplam = 0;
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
uint32_t tn = 0, fp = 0, fn = 0, tp = 0;   // karisiklik matrisi

// Son gecerli RSSI (frame gelmeyen pencerede ekrana bu yazilir)
float sonRssi = 0;

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
  bool  retry = (pkt->payload[1] >> 3) & 0x1;                 // FC retry biti (bit 11)

  portENTER_CRITICAL_ISR(&mux);
  rssiToplam += rssi;  rssiKareToplam += rssi * rssi;
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

// ---- MLP ileri gecis: N_OZELLIK -> 16 -> 8 -> 1 ----
void katman(const float *W, const float *B, const float *giris, float *cikis,
            int nGiris, int nCikis, bool relu) {
  for (int o = 0; o < nCikis; o++) {
    float t = B[o];
    for (int i = 0; i < nGiris; i++) t += W[o * nGiris + i] * giris[i];
    cikis[o] = (relu && t < 0) ? 0 : t;
  }
}

float atakOlasiligi(const float *oz) {
  float x[N_OZELLIK], h1[L1_CIKIS], h2[L2_CIKIS], z;
  for (int i = 0; i < N_OZELLIK; i++) x[i] = (oz[i] - OZ_MEAN[i]) / OZ_STD[i];
  katman(M_W1, M_B1, x,  h1, L1_GIRIS, L1_CIKIS, true);
  katman(M_W2, M_B2, h1, h2, L2_GIRIS, L2_CIKIS, true);
  katman(M_W3, M_B3, h2, &z, L3_GIRIS, L3_CIKIS, false);
  return 1.0f / (1.0f + expf(-z));
}

void pencereSifirla() {
  pencereAlinan = pencereKayip = pencereMaxArdisik = 0;
  gapToplam = gapKareToplam = gapMax = 0;
  gapSayisi = 0;
  pencereEtiket = -1;
  karisik = false;
}

void pencereBas() {
  float rT, rKT;
  uint32_t sayi, retry;
  portENTER_CRITICAL(&mux);
  rT = rssiToplam; rKT = rssiKareToplam;
  sayi = sniffSayisi; retry = retrySayisi;
  rssiToplam = rssiKareToplam = 0;
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

  float avgRssi = sonRssi, rssiStd = 0, retryOrani = 0;
  if (sayi > 0) {
    avgRssi    = rT / sayi;
    rssiStd    = sqrtf(fmaxf(rKT / sayi - avgRssi * avgRssi, 0));
    retryOrani = (float)retry / sayi;
    sonRssi    = avgRssi;
  }

  float avgGap = PENCERE_MS, gapStd = 0;
  if (gapSayisi > 0) {
    avgGap = gapToplam / gapSayisi;
    gapStd = sqrtf(fmaxf(gapKareToplam / gapSayisi - avgGap * avgGap, 0));
  }

  uint32_t toplamPaket = pencereAlinan + pencereKayip;
  float kayipOrani = toplamPaket ? (float)pencereKayip / toplamPaket : 0;

  // Ozellik sirasi train.py'deki OZELLIKLER ile ayni olmali
  float oz[N_OZELLIK] = {
    (float)pencereAlinan, (float)pencereKayip, kayipOrani, (float)pencereMaxArdisik,
    avgGap, gapStd, gapMax,
    rssiStd, retryOrani, (float)sayi,
  };
  uint32_t t0 = micros();
  float p = atakOlasiligi(oz);
  uint32_t sure = micros() - t0;
  uint8_t tahmin = p > 0.5f ? 1 : 0;

  if (label == 0) { if (tahmin) fp++; else tn++; }
  else            { if (tahmin) tp++; else fn++; }
  uint32_t toplam = tn + fp + fn + tp;

  Serial.printf("%s gercek=%s tahmin=%s p=%.3f (%lu us) | kayip=%.2f rssi=%.1f | "
                "dogruluk=%.3f (%lu/%lu) TN=%lu FP=%lu FN=%lu TP=%lu\n",
                tahmin == label ? "OK  " : "HATA",
                label ? "ATAK  " : "NORMAL", tahmin ? "ATAK  " : "NORMAL", p, sure,
                kayipOrani, avgRssi,
                (float)(tn + tp) / toplam, tn + tp, toplam, tn, fp, fn, tp);
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
  Serial.println("# TinyML drop atagi dedektoru hazir. 'r' = sayaclari sifirla");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'r') { tn = fp = fn = tp = 0; Serial.println("# Sayaclar sifirlandi"); }
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
    if (clientVar) pencereBas();
  }
  delay(1);
}
