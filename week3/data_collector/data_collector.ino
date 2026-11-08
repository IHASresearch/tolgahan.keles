#include "esp_wifi.h"
#include "WiFi.h"

// ===== AYARLAR =====
#define KANAL          1
#define KANAL_ATLAMA   false
#define ATLAMA_MS      500
#define FILTRE_AKTIF   true
// ===================

typedef struct {
  uint16_t frame_ctrl;
  uint16_t duration;
  uint8_t  addr1[6];
  uint8_t  addr2[6];
  uint8_t  addr3[6];
  uint16_t seq_ctrl;
} __attribute__((packed)) wifi_mac_hdr_t;

uint8_t currentChannel = KANAL;
volatile uint32_t paketSayisi = 0;

void macToStr(const uint8_t *mac, char *out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char* frameTipi(uint8_t type, uint8_t subtype) {
  if (type == 0) {
    switch (subtype) {
      case 0x08: return "Beacon";
      case 0x04: return "ProbeReq";
      case 0x05: return "ProbeResp";
      case 0x00: return "AssocReq";
      case 0x01: return "AssocResp";
      case 0x0B: return "Auth";
      case 0x0C: return "Deauth";
      default:   return "Mgmt";
    }
  } else if (type == 1) {
    switch (subtype) {
      case 0x0B: return "RTS";
      case 0x0C: return "CTS";
      case 0x0D: return "ACK";
      default:   return "Ctrl";
    }
  } else if (type == 2) {
    return "Data";
  }
  return "?";
}

void ssidCikar(const uint8_t *payload, uint16_t len, char *out) {
  out[0] = '\0';
  if (len < 38) return;
  uint16_t pos = 36;
  if (payload[pos] == 0x00) {
    uint8_t ssidLen = payload[pos + 1];
    if (ssidLen > 0 && ssidLen <= 32 && (pos + 2 + ssidLen) <= len) {
      for (int i = 0; i < ssidLen; i++) {
        char c = (char)payload[pos + 2 + i];
        out[i] = (c == ',' || c == '"' || c < 32) ? '_' : c;
      }
      out[ssidLen] = '\0';
    }
  }
}

void snifferCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  wifi_pkt_rx_ctrl_t *rx = &pkt->rx_ctrl;
  const uint8_t *payload = pkt->payload;

  if (FILTRE_AKTIF) {
    if (rx->rx_state != 0)          return;
    if (rx->sig_len < 24)           return;
    if (rx->sig_len > 2400)         return;
    if (rx->rssi < rx->noise_floor) return;
  }

  paketSayisi++;

  uint16_t fc = payload[0] | (payload[1] << 8);
  uint8_t  ftype    = (fc >> 2) & 0x3;
  uint8_t  fsubtype = (fc >> 4) & 0xF;
  bool toDS    = (fc >> 8)  & 0x1;
  bool fromDS  = (fc >> 9)  & 0x1;
  bool retry   = (fc >> 11) & 0x1;
  bool protect = (fc >> 14) & 0x1;

  wifi_mac_hdr_t *hdr = (wifi_mac_hdr_t *)payload;
  uint16_t seq = (hdr->seq_ctrl >> 4) & 0xFFF;

  char a1[18], a2[18], a3[18];
  macToStr(hdr->addr1, a1);
  macToStr(hdr->addr2, a2);
  macToStr(hdr->addr3, a3);

  char ssid[33] = "";
  if (ftype == 0 && (fsubtype == 0x08 || fsubtype == 0x05)) {
    ssidCikar(payload, rx->sig_len, ssid);
  }

  const char *phy = (rx->sig_mode == 0) ? "11b/g" : (rx->sig_mode == 1) ? "11n" : "VHT";
  int snr = rx->rssi - rx->noise_floor;

  // Tek satır CSV. Her satır "D," ile başlıyor ki Python veri satırını ayırt etsin.
  Serial.printf("D,%u,%d,%d,%d,%d,%d,%s,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,%d,%d,%d,%d,%s,%s,%s,%s\n",
    rx->timestamp, rx->channel, rx->secondary_channel,
    rx->rssi, rx->noise_floor, snr,
    phy, rx->mcs, rx->rate, rx->cwb, rx->sgi, rx->stbc, rx->fec_coding, rx->ampdu_cnt,
    frameTipi(ftype, fsubtype), seq, rx->sig_len,
    retry, protect, toDS, fromDS,
    a1, a2, a3, ssid);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&snifferCallback);
  wifi_promiscuous_filter_t filter;
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

  // Başlık satırı: "H," ile başlıyor. Python bunu görünce sütun adlarını alır.
  Serial.println("H,ts,channel,sec,rssi,nf,snr,phy,mcs,rate,cwb,sgi,stbc,fec,ampdu,type,seq,len,retry,protected,toDS,fromDS,addr1,addr2,addr3,ssid");
}

void loop() {
  if (KANAL_ATLAMA) {
    static uint32_t sonAtlama = 0;
    if (millis() - sonAtlama > ATLAMA_MS) {
      sonAtlama = millis();
      currentChannel++;
      if (currentChannel > 13) currentChannel = 1;
      esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    }
  }
  delay(10);
}