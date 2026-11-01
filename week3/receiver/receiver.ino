#include <SPI.h>
#include <RF24.h>
#include <string.h>
#include "mbedtls/aes.h"

// Pin definitions
#define PIN_SCK   18
#define PIN_MISO  19
#define PIN_MOSI  23
#define PIN_CSN   5
#define PIN_CE    22

// Communication configuration
#define RF_CHANNEL   12          // 2412 MHz = 2400 + 12
const byte RX_ADDRESS[6] = "1Node";
const uint8_t PAYLOAD_SIZE = 32; // 32 bytes = 2 AES blocks

// AES-128 key
const uint8_t AES_KEY[16] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

// RF24 radio instance: CE, CSN, 2 MHz SPI clock
RF24 radio(PIN_CE, PIN_CSN, 2000000UL);

uint8_t rxBuffer[PAYLOAD_SIZE];            // Encrypted payload
uint8_t decrypted[PAYLOAD_SIZE + 1];       // Decrypted payload (+1 for string terminator)

// Decrypt payload with AES-128 ECB
void aesDecryptECB(const uint8_t* in, uint8_t* out) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, AES_KEY, 128);
  for (uint8_t i = 0; i < PAYLOAD_SIZE; i += 16) {
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, in + i, out + i);
  }
  mbedtls_aes_free(&aes);
}

void setup() {
  // Start the serial port
  Serial.begin(115200);
  delay(500);

  // Initialize SPI on the defined pins
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);

  // Check the module connection
  if (!radio.begin(&SPI) || !radio.isChipConnected()) {
    Serial.println("ERROR! nRF24L01 not detected!");
    while (1) delay(1000);
  }

  // Apply the RF configuration
  radio.setPALevel(RF24_PA_MAX);
  radio.setDataRate(RF24_1MBPS);
  radio.setCRCLength(RF24_CRC_16);
  radio.setPayloadSize(PAYLOAD_SIZE);
  radio.setAutoAck(false);

  radio.setChannel(RF_CHANNEL);
  radio.openReadingPipe(1, RX_ADDRESS);
  radio.startListening();

  Serial.println("Listening on 2412 MHz (AES-128 ECB)...");
}

void loop() {
  // Check whether a valid packet is available in the RX FIFO
  while (radio.available()) {
    // Read the encrypted packet from the module
    radio.read(rxBuffer, PAYLOAD_SIZE);

    // Decrypt the payload
    aesDecryptECB(rxBuffer, decrypted);
    decrypted[PAYLOAD_SIZE] = '\0';

    // Print the encrypted payload in hex and the decrypted message
    Serial.print("Encrypted (hex): ");
    for (uint8_t i = 0; i < PAYLOAD_SIZE; i++) {
      if (rxBuffer[i] < 0x10) Serial.print('0');
      Serial.print(rxBuffer[i], HEX);
      Serial.print(' ');
    }
    Serial.print(" | Decrypted: ");
    Serial.println((char*)decrypted);
  }
}