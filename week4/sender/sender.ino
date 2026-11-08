// UAV side (master): owns the clock, hops every 5 s and never blocks.
// Sends a DATA packet every 20 ms and receives ground station data in the ACK payload.

#include <SPI.h>
#include <RF24.h>
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "mbedtls/aes.h"

// Pin definitions
#define PIN_SCK   18
#define PIN_MISO  19
#define PIN_MOSI  23
#define PIN_CSN   5
#define PIN_CE    22

// Radio configuration (must be identical on both devices)
const byte LINK_ADDRESS[6] = "UAV01";
const uint8_t PAYLOAD_SIZE = 32;   // 32 bytes = 2 AES blocks
const rf24_pa_dbm_e PA_LEVEL = RF24_PA_LOW;   // LOW for desk tests, MAX for range tests

// AES-128 key
const uint8_t AES_KEY[16] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

// Frequency hopping configuration (must be identical on both devices)
const uint8_t CHANNELS[8] = {75, 25, 49, 81, 50, 76, 80, 77};  // Gaps between WiFi channels 1, 6, 11
const uint8_t NUM_CHANNELS = sizeof(CHANNELS);
const int64_t HOP_INTERVAL_US = 5000000LL;   // Hop every 5 s
const int64_t GUARD_US = 20000LL;            // No TX within 20 ms of a slot boundary

// Link timing
const uint32_t SEND_INTERVAL_MS = 20;        // One DATA packet every 20 ms
const uint32_t TX_TIMEOUT_MS = 10;           // A send with all retries takes ~4 ms
const uint32_t LINK_TIMEOUT_MS = 500;        // LINK_DOWN if no ACK for 500 ms
const uint32_t REPORT_INTERVAL_MS = 1000;    // Status line once per second

// Packet types
#define PKT_DATA  1
#define PKT_ACK   2

// UAV -> ground station
struct __attribute__((packed)) DataPacket {
  uint8_t type;
  uint16_t sessionId;   // Random per boot, changes when the UAV restarts
  uint32_t seq;         // Increments on every packet
  int64_t timeUs;       // UAV clock in microseconds
  char payload[17];
};

// Ground station -> UAV (carried in the ACK payload)
struct __attribute__((packed)) AckPacket {
  uint8_t type;
  uint32_t seq;
  char payload[27];
};

static_assert(sizeof(DataPacket) == PAYLOAD_SIZE, "DataPacket must be 32 bytes");
static_assert(sizeof(AckPacket) == PAYLOAD_SIZE, "AckPacket must be 32 bytes");

// RF24 radio instance: CE, CSN, 2 MHz SPI clock
RF24 radio(PIN_CE, PIN_CSN, 2000000UL);

uint8_t txBuffer[PAYLOAD_SIZE];   // Encrypted DATA packet
uint8_t rxBuffer[PAYLOAD_SIZE];   // Encrypted ACK payload

uint16_t sessionId = 0;
uint32_t txSeq = 1;
uint8_t currentChannel = 255;
uint32_t lastSendMs = 0;
uint32_t lastAckMs = 0;
bool gotAck = false;
bool linkUp = false;

// Packet statistics for both directions
struct Stats {
  uint32_t sent;     // UAV -> GS packets sent
  uint32_t acked;    // UAV -> GS packets acknowledged
  uint32_t gsRx;     // GS -> UAV messages received in ACK payloads
  uint32_t gsLost;   // GS -> UAV messages lost (from seq gaps)
};
Stats secStats = {0, 0, 0, 0};     // Last second
Stats totalStats = {0, 0, 0, 0};   // Since boot
uint32_t lastGsSeq = 0;
uint32_t lastReportMs = 0;
char lastGroundPayload[sizeof(AckPacket::payload) + 1] = "-";

// Loss ratio in percent
float lossPct(uint32_t ok, uint32_t total) {
  return total ? 100.0f * (total - ok) / total : 0.0f;
}

// Encrypt 32 bytes with AES-128 ECB
void aesEncryptECB(const uint8_t* in, uint8_t* out) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, AES_KEY, 128);
  for (uint8_t i = 0; i < PAYLOAD_SIZE; i += 16) {
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in + i, out + i);
  }
  mbedtls_aes_free(&aes);
}

// Decrypt 32 bytes with AES-128 ECB
void aesDecryptECB(const uint8_t* in, uint8_t* out) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, AES_KEY, 128);
  for (uint8_t i = 0; i < PAYLOAD_SIZE; i += 16) {
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, in + i, out + i);
  }
  mbedtls_aes_free(&aes);
}

// Switch to the channel of the current time slot
void updateChannel(int64_t nowUs) {
  int64_t slot = nowUs / HOP_INTERVAL_US;
  uint8_t ch = CHANNELS[slot % NUM_CHANNELS];
  if (ch == currentChannel) return;

  radio.setChannel(ch);
  currentChannel = ch;
  Serial.printf("Hop -> slot %lld, channel %u (%u MHz)\n", slot, ch, 2400 + ch);
}

// Read the ground station data that arrived in the ACK payload
void readAckPayload() {
  // The RX FIFO holds at most 3 payloads, so never loop more than that
  for (uint8_t i = 0; i < 3 && radio.available(); i++) {
    if (radio.getDynamicPayloadSize() != PAYLOAD_SIZE) {
      radio.flush_rx();
      return;
    }
    radio.read(rxBuffer, PAYLOAD_SIZE);

    AckPacket ack;
    aesDecryptECB(rxBuffer, (uint8_t*)&ack);
    if (ack.type != PKT_ACK) continue;

    // Same seq again means the ground station had nothing new queued
    if (ack.seq == lastGsSeq) continue;

    // A smaller seq means the ground station restarted, so there is no gap to count
    uint32_t lost = (lastGsSeq != 0 && ack.seq > lastGsSeq) ? ack.seq - lastGsSeq - 1 : 0;
    lastGsSeq = ack.seq;
    secStats.gsRx++;
    totalStats.gsRx++;
    secStats.gsLost += lost;
    totalStats.gsLost += lost;

    memcpy(lastGroundPayload, ack.payload, sizeof(ack.payload));
    lastGroundPayload[sizeof(ack.payload)] = '\0';
  }
}

// Send one packet and wait for the result with our own timeout.
// RF24::write() is not used: with FAILURE_HANDLING enabled, RF24 1.6.2 retries forever
// when the radio does not report a result.
// Returns 1 if acknowledged, 0 if all retries failed, -1 if the radio never finished.
int8_t radioSend(const uint8_t* buf) {
  radio.clearStatusFlags();
  radio.startWrite(buf, PAYLOAD_SIZE, false);   // Load the payload and pulse CE

  // Poll the status read-only; clearing while polling could erase a flag set in between
  uint32_t start = millis();
  while (millis() - start < TX_TIMEOUT_MS) {
    uint8_t status = radio.update();
    if (status & RF24_TX_DS) {
      radio.clearStatusFlags(RF24_TX_DS);
      return 1;
    }
    if (status & RF24_TX_DF) {
      radio.clearStatusFlags(RF24_TX_DF);
      radio.flush_tx();
      return 0;
    }
  }
  radio.flush_tx();
  return -1;
}

// Build, encrypt and send one DATA packet
void sendData() {
  DataPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type = PKT_DATA;
  pkt.sessionId = sessionId;
  pkt.seq = txSeq++;
  snprintf(pkt.payload, sizeof(pkt.payload), "UAV %lu", (unsigned long)pkt.seq);
  pkt.timeUs = esp_timer_get_time();

  aesEncryptECB((uint8_t*)&pkt, txBuffer);
  secStats.sent++;
  totalStats.sent++;

  int8_t result = radioSend(txBuffer);

  // The radio never finished the transmission: reconfigure it
  if (result < 0) {
    Serial.println("TX did not complete, reinitializing radio");
    configureRadio();
    return;
  }
  if (result == 0) return;

  secStats.acked++;
  totalStats.acked++;
  lastAckMs = millis();
  gotAck = true;
  readAckPayload();
}

// LINK_UP while ACKs keep arriving, LINK_DOWN otherwise
void updateLinkState() {
  bool up = gotAck && (millis() - lastAckMs) < LINK_TIMEOUT_MS;
  if (up == linkUp) return;

  linkUp = up;
  Serial.println(linkUp ? "Link -> LINK_UP" : "Link -> LINK_DOWN");
}

// Print a status line once per second
void report() {
  if (millis() - lastReportMs < REPORT_INTERVAL_MS) return;
  lastReportMs = millis();

  const Stats& s = secStats;
  const Stats& t = totalStats;
  Serial.printf("[%s] ch %u | ground: %s\n",
                linkUp ? "LINK_UP" : "LINK_DOWN", currentChannel, lastGroundPayload);
  Serial.printf("  UAV->GS 1s: sent %lu, acked %lu (%.1f%% loss) | total: sent %lu, acked %lu (%.1f%% loss)\n",
                (unsigned long)s.sent, (unsigned long)s.acked, lossPct(s.acked, s.sent),
                (unsigned long)t.sent, (unsigned long)t.acked, lossPct(t.acked, t.sent));
  Serial.printf("  GS->UAV 1s: rx %lu, lost %lu (%.1f%% loss) | total: rx %lu, lost %lu (%.1f%% loss)\n",
                (unsigned long)s.gsRx, (unsigned long)s.gsLost, lossPct(s.gsRx, s.gsRx + s.gsLost),
                (unsigned long)t.gsRx, (unsigned long)t.gsLost, lossPct(t.gsRx, t.gsRx + t.gsLost));
  secStats = {0, 0, 0, 0};
}

// Initialize the module and apply the RF configuration; waits until the module answers
void configureRadio() {
  while (!radio.begin(&SPI) || !radio.isChipConnected()) {
    Serial.println("ERROR! nRF24L01 not detected!");
    delay(1000);
  }

  // Apply the RF configuration
  radio.setPALevel(PA_LEVEL);
  radio.setDataRate(RF24_1MBPS);
  radio.setCRCLength(RF24_CRC_16);
  radio.setAutoAck(true);
  radio.enableDynamicPayloads();
  radio.enableAckPayload();
  radio.setRetries(2, 3);   // 750 us between retries, 3 retries

  // The master only transmits; ACKs come back on pipe 0 automatically
  radio.openWritingPipe(LINK_ADDRESS);
  radio.stopListening();

  // Force the current slot's channel to be set again
  currentChannel = 255;
}

void setup() {
  // Start the serial port
  Serial.begin(115200);
  delay(500);

  // Initialize SPI on the defined pins
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);
  configureRadio();

  // New session id on every boot so the ground station can detect a restart
  sessionId = (uint16_t)(esp_random() ^ (uint32_t)esp_timer_get_time());
  Serial.printf("UAV started, session %04X\n", sessionId);
}

void loop() {
  int64_t nowUs = esp_timer_get_time();

  updateChannel(nowUs);
  updateLinkState();
  report();

  // Skip transmission near slot boundaries to tolerate clock drift
  int64_t inSlot = nowUs % HOP_INTERVAL_US;
  if (inSlot < GUARD_US || inSlot > HOP_INTERVAL_US - GUARD_US) return;

  // Send at a fixed rate
  if (millis() - lastSendMs < SEND_INTERVAL_MS) return;
  lastSendMs = millis();

  sendData();
}
