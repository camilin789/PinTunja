/*
 * PIN Tunja – Firmware del sensor (ESP32 DevKit V1 + MPU6050 + sensor de choque)
 * Proyecto integrador · Ingeniería de Datos e IA · USTA Tunja · 2026
 *
 * Qué hace:
 *   1. Lee el acelerómetro MPU6050 por el bus I2C (SDA = D21, SCL = D22, 400 kHz) a 100 Hz exactos.
 *   2. Guarda la magnitud de las últimas 100 lecturas (1 s) en una COLA CIRCULAR en la SRAM.
 *   3. Detecta golpes: si |a| - 1 g pasa de 0,30 g en la última ventana Y el sensor de choque
 *      (S -> D18) se activó en los últimos 200 ms, envía "B,<pico en mg>,<ms>" y prende el LED azul.
 *   4. Envía todas las lecturas al celular por Bluetooth de bajo consumo (BLE), en paquetes de 10.
 *   5. Cada 2 s envía su estado: frecuencia real, tiempo de cada ciclo y de la detección, RAM libre y
 *      tamaño del programa (datos REALES para la tabla R7 de Arquitectura).
 *   6. Por el Monitor Serie (115200) imprime ax, ay, az y Total (quieto ≈ 1,00 g).
 *
 * Conexiones:
 *   MPU6050:          VCC -> 3V3 | GND -> GND | SDA -> D21 | SCL -> D22
 *   Sensor de choque: (+) centro -> 3V3 | (-) -> GND | S -> D18
 *
 * Placa en el IDE de Arduino: "ESP32 Dev Module" (paquete esp32 de Espressif 3.x).
 * Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)". No necesita librerías extra.
 *
 * Protocolo BLE (servicio tipo "Nordic UART"):
 *   Servicio  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   TX (notify, ESP32 → celular) 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX (write,  celular → ESP32) 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
 *   Paquete de datos (binario, 64 bytes): 'D' | seq (uint16 LE) | n (uint8) | n × (ax, ay, az) int16 LE
 *       cuentas crudas del sensor en ±2 g → 16384 cuentas = 1 g
 *   Golpe (texto):  B,<pico_mg>,<millis>
 *   Estado (texto): S,<hz×10>,<ciclo_us>,<deteccion_us>,<heap_libre>,<programa_bytes>,<flash_libre_programa>
 */
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#if __has_include(<BLE2902.h>)
#include <BLE2902.h>
#define PIN_HAS_2902 1
#endif

// ---------------- Pines y sensor ----------------
const int PIN_SDA = 21;
const int PIN_SCL = 22;
const uint8_t MPU = 0x68;            // dirección I2C del MPU6050 (AD0 a tierra)
const float COUNTS_PER_G = 16384.0;  // rango ±2 g

// ---------------- Sensor de choque ----------------
const int PIN_CHOQUE = 18;           // pin S del sensor de choque
const int LED = 2;                   // LED azul de la placa
const bool USAR_CHOQUE = true;       // false = detectar solo con el MPU6050
const uint32_t VENTANA_CHOQUE_MS = 200;  // el choque debe coincidir con el golpe del MPU
volatile uint32_t ultimoChoque = 0;
uint32_t ledHasta = 0;
void IRAM_ATTR alChoque() { ultimoChoque = millis(); }

// ---------------- Tiempos ----------------
const uint32_t PERIOD_US = 10000;    // 100 Hz
const float THRESHOLD_G = 0.30;      // umbral de golpe (≈ 3 m/s², el mismo de la app)
const uint32_t COOLDOWN_MS = 1000;   // no repetir el mismo golpe

// ---------------- BLE ----------------
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
BLECharacteristic *tx = nullptr;
volatile bool connected = false;

// ---------------- Cola circular (1 s de lecturas) ----------------
const int N = 100;
float ring[N];                       // |a| - 1 g, en g
int head = 0;
int filled = 0;

// ---------------- Paquete de datos ----------------
const int PER_PACKET = 10;
uint8_t packet[4 + PER_PACKET * 6];
int inPacket = 0;
uint16_t seq = 0;

// ---------------- Medición de recursos ----------------
uint32_t nextTick = 0, lastStatus = 0, lastBump = 0;
uint32_t samples = 0, samplesAtStatus = 0;
uint32_t loopUsSum = 0, loopCount = 0, detectUs = 0;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override { connected = true; }
  void onDisconnect(BLEServer *s) override {
    connected = false;
    delay(200);
    BLEDevice::startAdvertising();   // vuelve a aparecer para que el celular se reconecte
  }
};

void writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}

bool readAccel(int16_t &ax, int16_t &ay, int16_t &az) {
  Wire.beginTransmission(MPU);
  Wire.write(0x3B);                  // ACCEL_XOUT_H: los 6 bytes siguientes son X, Y, Z
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MPU, 6) != 6) return false;
  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
  return true;
}

void sendText(const char *s) {
  if (!connected || tx == nullptr) return;
  tx->setValue((uint8_t *)s, strlen(s));
  tx->notify();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  // --- Sensor de choque y LED ---
  pinMode(LED, OUTPUT);
  pinMode(PIN_CHOQUE, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_CHOQUE), alChoque, CHANGE);
  Serial.printf("Sensor de choque en GPIO%d (reposo = %d)\n", PIN_CHOQUE, digitalRead(PIN_CHOQUE));

  // --- Sensor por I2C ---
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  writeReg(0x6B, 0x00);              // PWR_MGMT_1: despertar el sensor
  writeReg(0x1C, 0x00);              // ACCEL_CONFIG: rango ±2 g
  writeReg(0x1A, 0x03);              // CONFIG: filtro pasa-bajas interno ~44 Hz (quita ruido del motor)
  Wire.beginTransmission(MPU); Wire.write(0x75); Wire.endTransmission(false);
  Wire.requestFrom((int)MPU, 1);
  int who = Wire.available() ? Wire.read() : -1;
  if (who < 0) Serial.println("No encuentro el MPU6050: revisa VCC, GND, SDA (D21) y SCL (D22)");
  else Serial.printf("MPU6050 encontrado (WHO_AM_I = 0x%02X)\n", who);

  // --- Bluetooth ---
  BLEDevice::init("PIN-Tunja-ESP32");
  BLEDevice::setMTU(247);            // paquetes de hasta 244 bytes (los de datos usan 64)
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService *svc = server->createService(SERVICE_UUID);
  tx = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
#ifdef PIN_HAS_2902
  tx->addDescriptor(new BLE2902());
#endif
  svc->createCharacteristic(RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("Bluetooth listo: busca \"PIN-Tunja-ESP32\" desde la app");

  for (int i = 0; i < N; i++) ring[i] = 0;
  nextTick = micros();
  lastStatus = millis();
}

void loop() {
  // Apagar el LED del último golpe sin bloquear el muestreo
  if (ledHasta && millis() > ledHasta) { digitalWrite(LED, LOW); ledHasta = 0; }

  // Muestreo a 100 Hz con el reloj interno (micros): se espera hasta el siguiente tic
  uint32_t now = micros();
  if ((int32_t)(now - nextTick) < 0) return;
  nextTick += PERIOD_US;
  uint32_t t0 = micros();

  int16_t ax, ay, az;
  if (!readAccel(ax, ay, az)) return;
  samples++;

  // 1) Magnitud y cola circular: la lectura nueva reemplaza a la más vieja (O(1), memoria fija)
  float gx = ax / COUNTS_PER_G, gy = ay / COUNTS_PER_G, gz = az / COUNTS_PER_G;
  float total = sqrtf(gx * gx + gy * gy + gz * gz);
  ring[head] = total - 1.0f;
  head = (head + 1) % N;
  if (filled < N) filled++;

  // 2) Detección: mayor desviación de la última ventana de 1 s + confirmación del sensor de choque
  uint32_t d0 = micros();
  float peak = 0;
  for (int i = 0; i < filled; i++) { float v = fabsf(ring[i]); if (v > peak) peak = v; }
  bool choqueReciente = ultimoChoque != 0 && (millis() - ultimoChoque) < VENTANA_CHOQUE_MS;
  detectUs = micros() - d0;
  if (peak >= THRESHOLD_G && millis() - lastBump > COOLDOWN_MS && fabsf(ring[(head + N - 1) % N]) >= THRESHOLD_G
      && (!USAR_CHOQUE || choqueReciente)) {
    lastBump = millis();
    char msg[40];
    snprintf(msg, sizeof(msg), "B,%d,%lu", (int)(peak * 1000), (unsigned long)lastBump);
    sendText(msg);
    Serial.println(msg);
    digitalWrite(LED, HIGH);
    ledHasta = lastBump + 100;
  }

  // 3) Paquete de datos: 10 lecturas crudas en 64 bytes
  int off = 4 + inPacket * 6;
  packet[off] = ax & 0xFF; packet[off + 1] = ax >> 8;
  packet[off + 2] = ay & 0xFF; packet[off + 3] = ay >> 8;
  packet[off + 4] = az & 0xFF; packet[off + 5] = az >> 8;
  inPacket++;
  if (inPacket == PER_PACKET) {
    packet[0] = 'D'; packet[1] = seq & 0xFF; packet[2] = seq >> 8; packet[3] = PER_PACKET;
    if (connected) { tx->setValue(packet, sizeof(packet)); tx->notify(); }
    seq++;
    inPacket = 0;
  }

  // 4) Monitor Serie a 10 Hz (para la prueba del armado)
  if (samples % 10 == 0) Serial.printf("ax=%.2f ay=%.2f az=%.2f Total=%.2f g\n", gx, gy, gz, total);

  loopUsSum += micros() - t0; loopCount++;

  // 5) Estado cada 2 s: datos reales de recursos
  if (millis() - lastStatus >= 2000) {
    float secs = (millis() - lastStatus) / 1000.0f;
    int hz10 = (int)((samples - samplesAtStatus) / secs * 10);
    char st[96];
    snprintf(st, sizeof(st), "S,%d,%lu,%lu,%lu,%lu,%lu", hz10,
             (unsigned long)(loopCount ? loopUsSum / loopCount : 0), (unsigned long)detectUs,
             (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getSketchSize(), (unsigned long)ESP.getFreeSketchSpace());
    sendText(st);
    lastStatus = millis(); samplesAtStatus = samples; loopUsSum = 0; loopCount = 0;
  }
}
