#include <WiFi.h>
#include <WiFiUdp.h>

// ===== AYARLAR =====
#define AP_SSID       "UAV_LINK"
#define AP_PASS       "uavlink123"
#define SERVER_PORT   4210
#define GONDERME_MS   100     // saniyede 10 paket
// ===================

// Client artik sahte paket dusurmuyor. Hep sabit hizda gonderir.
// Atak = ortamdaki gercek parazit; etiketi server tarafinda veriyoruz.

WiFiUDP udp;
uint32_t seq = 0;

void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(AP_SSID, AP_PASS);

  Serial.print("# Baglaniliyor");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
  }
  Serial.printf("\n# Baglandi, IP: %s, server: %s\n",
                WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
}

void loop() {
  static uint32_t sonGonderim = 0;
  if (millis() - sonGonderim < GONDERME_MS) return;
  sonGonderim = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  seq++;
  udp.beginPacket(WiFi.gatewayIP(), SERVER_PORT);
  udp.write((const uint8_t *)&seq, sizeof(seq));
  udp.endPacket();
}
