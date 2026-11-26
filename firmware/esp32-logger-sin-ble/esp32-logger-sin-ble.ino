/*
 * PIN Tunja – Registrador de datos SIN Bluetooth (ESP32 DevKit V1 + MPU6050 + sensor de choque)
 *
 * Al recibir energía (power bank) empieza a grabar solo, en la memoria Flash del ESP32:
 *   - 100 lecturas por segundo del MPU6050 (ax, ay, az crudos, ±2 g → 16384 = 1 g)
 *   - Marcas por lectura: sensor de choque activado, bache detectado (MPU + choque), botón BOOT presionado
 * Cada vez que se enciende crea una sesión nueva: /s001.bin, /s002.bin, ...
 * Con ~3 MB de memoria caben unos 60 minutos de grabación en total.
 *
 * LED azul:
 *   - parpadeo corto cada 2 s  → está grabando
 *   - destello más largo       → detectó un bache
 *   - encendido fijo           → memoria llena (hay que descargar y borrar)
 * Botón BOOT: presiónalo cuando pasen por un hueco para marcarlo a mano (sirve como etiqueta).
 *
 * Descargar los datos: conecta el ESP32 al PC y ejecuta scripts/descargar_esp32.py (guarda un CSV por sesión).
 * Comandos por el Monitor Serie (921600 baudios): L = listar, G = nueva sesión, X = borrar todo.
 *
 * IDE de Arduino: placa "ESP32 Dev Module",
 *   Partition Scheme: "No OTA (1MB APP/3MB SPIFFS)"  ← importante para tener ~60 min de memoria.
 *
 * Conexiones:
 *   MPU6050:          VCC -> 3V3 | GND -> GND | SDA -> D21 | SCL -> D22
 *   Sensor de choque: (+) centro -> 3V3 | (-) -> GND | S -> D18
 */
#include <Wire.h>
#include <LittleFS.h>

// ---------------- Pines ----------------
const int PIN_SDA = 21, PIN_SCL = 22;
const int PIN_CHOQUE = 18;
const int PIN_BOTON = 0;     // botón BOOT de la placa
const int LED = 2;
const uint8_t MPU = 0x68;

// ---------------- Ajustes ----------------
const uint32_t PERIOD_US = 10000;        // 100 Hz
const float UMBRAL_G = 0.30;             // golpe del MPU: |a| - 1 g >= 0,30 g
const uint32_t VENTANA_CHOQUE_MS = 200;  // el sensor de choque debe coincidir
const uint32_t ESPERA_MS = 1000;         // no contar el mismo hueco dos veces
const size_t MARGEN_BYTES = 32 * 1024;   // espacio que se deja libre

// Registro de 8 bytes por lectura
struct __attribute__((packed)) Muestra {
  int16_t ax, ay, az;
  uint16_t marcas;   // bit0 = choque, bit1 = bache detectado, bit2 = botón BOOT
};

volatile uint32_t ultimoChoque = 0;
volatile bool choqueFlag = false;
void IRAM_ATTR alChoque() { ultimoChoque = millis(); choqueFlag = true; }

File archivo;
bool grabando = false, llena = false;
char nombre[24];
uint32_t muestras = 0, baches = 0, ultimoBache = 0, ultimoFlush = 0, ledHasta = 0;
uint32_t nextTick = 0;

// ---------------- Sensor ----------------
void writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}

bool readAccel(int16_t &ax, int16_t &ay, int16_t &az) {
  Wire.beginTransmission(MPU); Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MPU, 6) != 6) return false;
  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
  return true;
}

// ---------------- Archivos ----------------
size_t libre() { return LittleFS.totalBytes() - LittleFS.usedBytes(); }

void nuevaSesion() {
  if (grabando) { archivo.close(); grabando = false; }
  if (libre() < MARGEN_BYTES) { llena = true; Serial.println("Memoria llena: descarga y borra"); return; }
  for (int n = 1; n < 1000; n++) {
    snprintf(nombre, sizeof(nombre), "/s%03d.bin", n);
    if (!LittleFS.exists(nombre)) break;
  }
  archivo = LittleFS.open(nombre, "w");
  grabando = (bool)archivo;
  llena = false;
  muestras = 0; baches = 0;
  Serial.printf("Grabando en %s (libre: %u KB, ~%u min)\n", nombre,
                (unsigned)(libre() / 1024), (unsigned)(libre() / (sizeof(Muestra) * 100) / 60));
}

void detener() {
  if (grabando) { archivo.close(); grabando = false; Serial.printf("Sesion %s cerrada\n", nombre); }
}

void listar() {
  detener();
  File raiz = LittleFS.open("/");
  for (File f = raiz.openNextFile(); f; f = raiz.openNextFile()) {
    Serial.printf("  %s  %u bytes  (~%u s)\n", f.name(), (unsigned)f.size(),
                  (unsigned)(f.size() / sizeof(Muestra) / 100));
  }
  Serial.printf("Libre: %u KB de %u KB\n", (unsigned)(libre() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
}

// Envía cada archivo como: "FILE <nombre> <bytes>\n" + bytes crudos. Al final "END\n".
void descargar() {
  detener();
  uint8_t buf[512];
  File raiz = LittleFS.open("/");
  for (File f = raiz.openNextFile(); f; f = raiz.openNextFile()) {
    if (f.size() == 0) continue;
    Serial.printf("FILE %s %u\n", f.name(), (unsigned)f.size());
    size_t n;
    while ((n = f.read(buf, sizeof(buf))) > 0) Serial.write(buf, n);
    Serial.flush();
  }
  Serial.println("END");
}

void borrarTodo() {
  detener();
  File raiz = LittleFS.open("/");
  String nombres[200]; int k = 0;
  for (File f = raiz.openNextFile(); f && k < 200; f = raiz.openNextFile()) nombres[k++] = String("/") + f.name();
  raiz.close();
  for (int i = 0; i < k; i++) LittleFS.remove(nombres[i]);
  Serial.printf("BORRADO (%d archivos). Libre: %u KB\n", k, (unsigned)(libre() / 1024));
}

void atender(char c) {
  if (c == 'L' || c == 'l') listar();
  else if (c == 'D' || c == 'd') descargar();
  else if (c == 'X' || c == 'x') borrarTodo();
  else if (c == 'G' || c == 'g') nuevaSesion();
  else return;
  nextTick = micros();   // retomar el ritmo de muestreo después del comando
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(921600);
  delay(300);
  pinMode(LED, OUTPUT);
  pinMode(PIN_BOTON, INPUT_PULLUP);
  pinMode(PIN_CHOQUE, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_CHOQUE), alChoque, CHANGE);

  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  writeReg(0x6B, 0x00);   // despertar
  writeReg(0x1C, 0x00);   // ±2 g
  writeReg(0x1A, 0x03);   // filtro interno ~44 Hz
  Wire.beginTransmission(MPU); Wire.write(0x75); Wire.endTransmission(false);
  Wire.requestFrom((int)MPU, 1);
  int who = Wire.available() ? Wire.read() : -1;
  if (who < 0) Serial.println("No encuentro el MPU6050: revisa VCC, GND, SDA (D21) y SCL (D22)");
  else Serial.printf("MPU6050 OK (WHO_AM_I = 0x%02X)\n", who);

  if (!LittleFS.begin(true)) {   // la primera vez formatea la memoria
    Serial.println("Error al montar la memoria: revisa el Partition Scheme");
    digitalWrite(LED, HIGH);
    while (true) delay(1000);
  }
  nuevaSesion();
  nextTick = micros();
}

// ---------------- Loop ----------------
void loop() {
  if (Serial.available()) atender(Serial.read());

  uint32_t ms = millis();
  // LED: destello de bache / parpadeo de grabación / fijo si está llena
  if (ledHasta) { if (ms > ledHasta) { digitalWrite(LED, LOW); ledHasta = 0; } }
  else if (llena) digitalWrite(LED, HIGH);
  else digitalWrite(LED, (grabando && (ms % 2000) < 50) ? HIGH : LOW);

  uint32_t now = micros();
  if ((int32_t)(now - nextTick) < 0) return;
  nextTick += PERIOD_US;
  if ((int32_t)(micros() - nextTick) > 200000) nextTick = micros();   // si se atrasó mucho, no recuperar en ráfaga

  int16_t ax, ay, az;
  if (!readAccel(ax, ay, az)) return;

  float gx = ax / 16384.0f, gy = ay / 16384.0f, gz = az / 16384.0f;
  float total = sqrtf(gx * gx + gy * gy + gz * gz);
  float exceso = fabsf(total - 1.0f);

  uint16_t marcas = 0;
  if (choqueFlag) { choqueFlag = false; marcas |= 1; }
  bool choqueReciente = ultimoChoque != 0 && (ms - ultimoChoque) < VENTANA_CHOQUE_MS;
  if (exceso >= UMBRAL_G && choqueReciente && (ms - ultimoBache) >= ESPERA_MS) {
    ultimoBache = ms; baches++;
    marcas |= 2;
    digitalWrite(LED, HIGH); ledHasta = ms + 300;
    Serial.printf("BACHE #%lu  pico = %.2f g\n", (unsigned long)baches, exceso);
  }
  if (digitalRead(PIN_BOTON) == LOW) marcas |= 4;

  if (grabando) {
    Muestra s = {ax, ay, az, marcas};
    archivo.write((uint8_t *)&s, sizeof(s));
    muestras++;
    if (ms - ultimoFlush >= 2000) {
      archivo.flush();
      ultimoFlush = ms;
      if (libre() < MARGEN_BYTES) {
        archivo.close(); grabando = false; llena = true;
        Serial.println("Memoria llena: descarga y borra");
      }
    }
  }

  if (muestras % 100 == 0 && grabando)
    Serial.printf("%s  %lu s  baches=%lu  Total=%.2f g\n", nombre, (unsigned long)(muestras / 100),
                  (unsigned long)baches, total);
}
