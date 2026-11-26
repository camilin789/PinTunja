# PIN Tunja — Detección y aviso de huecos en las vías de Tunja

Proyecto Integrador · Ingeniería de Datos e Inteligencia Artificial · Universidad Santo Tomás, Seccional Tunja · 2026-2

**Integrantes:** Camilo Arias · Nataly Sofía Forero Aparicio · Alan Duwua Galvis Aguirre

**Materias integradas:**
- Sistemas de Adquisición y Procesamiento de Datos — Docente Zalma Valentina Moreno Galeano
- Arquitectura de Computadores — Docente Fernando Jiménez Díaz
- Estructura de Datos — Docente Yashin Alberto Rueda Matos

---

## 1. Problemática

Los huecos de las vías de Tunja elevan los costos de mantenimiento de suspensión (bujes, hojas de resorte) y llantas de los buses de Cotrascol, y el problema se extiende a taxis y carros particulares. PIN Tunja registra los huecos con sensores (celular y ESP32), los ubica con el GPS del celular y avisa por voz al conductor antes de llegar a ellos, con una velocidad segura sugerida. Además cotiza los repuestos afectados mediante web scraping.

## 2. Componentes del repositorio

```
PinTunja-repo/
├── app/                        App Android (Kotlin + Jetpack Compose) — se agrega con agregar_app.ps1
├── firmware/
│   ├── esp32-ble-mpu6050-choque/   Firmware con Bluetooth (BLE) hacia la app
│   └── esp32-logger-sin-ble/       Firmware que graba en la Flash del ESP32
├── scripts/
│   └── descargar_esp32.py          Descarga las sesiones del ESP32 a CSV
├── notebooks/                  Cuaderno .ipynb de procesamiento y EDA
├── datos/                      CSV del web scraping y capturas de sensores
├── docs/                       Documentación técnica de la app
└── README.md
```

## 3. Hardware (Arquitectura de Computadores)

| Elemento | Detalle |
|---|---|
| Placa | ESP32 DevKit V1 (ESP32-WROOM-32), 2 núcleos a 240 MHz, Wi-Fi y BLE |
| Acelerómetro | MPU6050 por I2C (dirección 0x68, 400 kHz), rango ±2 g (16.384 cuentas = 1 g) |
| Sensor de choque | Módulo de 3 pines, señal S en GPIO18 (interrupción) |
| Energía | Power bank por micro USB |

**Conexiones**

| ESP32 | MPU6050 / Choque | Función |
|---|---|---|
| 3V3 | VCC / (+) | Energía 3,3 V |
| GND | GND / (−) | Tierra común |
| D21 | SDA | Datos I2C |
| D22 | SCL | Reloj I2C |
| D18 | S (choque) | Señal digital del golpe |

**Funcionamiento del firmware:** muestreo a 100 Hz con `micros()`, cola circular de 100 lecturas (1 s) en SRAM, detección de golpe cuando |a| − 1 g ≥ 0,30 g y el sensor de choque se activó en los últimos 200 ms. Se envían los datos al celular por BLE (servicio tipo Nordic UART) o se graban en la memoria Flash (LittleFS), según el firmware cargado.

## 4. App Android (Adquisición + Estructura de Datos)

Pestañas principales: **Conducir · Grabar · Mapa · Repuestos/Precios · Comunidad · Asistente · Proyecto**.

**Sistemas de Adquisición y Procesamiento de Datos**
- Adquisición en vivo con el acelerómetro del celular (~200 Hz) y del ESP32 por BLE (100 Hz), GPS a 1 Hz.
- Procesamiento: retiro de la gravedad (filtro exponencial), promedio móvil, umbral ±3 m/s², pico, RMS, espectro con DFT y ventana de Hann.
- Dataset propio etiquetado (bache / normal / reductor) con partición por recorrido y versiones congeladas.
- **Web scraping de precios de repuestos** en Falabella (JSON incrustado `__NEXT_DATA__`), Éxito (API pública VTEX) y MercadoLibre (HTML; si la tienda pide inicio de sesión, la app lo informa y no lo evade). Limpieza con filtro de relevancia, eliminación de atípicos con IQR, mediana e historial con fecha.

**Estructura de Datos** (implementaciones propias)
- `ChainedHashTable`: tabla hash con encadenamiento, llave = celda geohash ~150 m → búsqueda de huecos cercanos en O(1).
- `MaxHeap`: cola de prioridad para decidir qué hueco avisar primero (O(log n)) y ranking top-10.
- Cola FIFO de voz, buffer circular del sensor, pila de "Me equivoqué".
- `PartTree`: árbol binario de búsqueda balanceado para autocompletar repuestos (altura 6 con 57 piezas).
- Merge sort de ofertas y búsqueda binaria para comparar el precio de un taller.

**Funciones de conducción (v12)**
- Aviso por voz de huecos adelante, con velocidad segura y "frena ya" cuando hace falta.
- Aviso de vía despejada (solo si al menos el 70 % del kilómetro siguiente ya fue grabado).
- Copiloto persistente con servicio en primer plano; ruta con menos huecos (OSRM) y botón "Ver en Maps".
- Directorio propio de lugares de Tunja con búsqueda tolerante a errores.

Repositorio: https://github.com/camilin789/PinTunja · Más detalle en la carpeta [`docs/`](docs/) (informe técnico y documentación de la app).

## 5. Instrucciones de ejecución

### 5.1 App Android
1. Instalar **Android Studio** (Koala o superior) con JDK 17.
2. `File → Open` y seleccionar la carpeta `app/` (proyecto Gradle `PinTunja`).
3. Esperar la sincronización de Gradle (descarga las dependencias automáticamente).
4. Conectar un celular Android 8.0+ con depuración USB o inalámbrica (`Pair devices using Wi-Fi`).
5. Ejecutar con ▶ **Run 'app'**. Conceder permisos de ubicación, sensores y micrófono.
6. Opcional (asistente IA): en la app, **Ajustes → Conexiones**, escribir una clave propia de la API de Gemini. Sin clave, el asistente responde con su motor de reglas.
7. Pruebas unitarias: `./gradlew test` (en Windows `gradlew.bat test`).

Para probar sin salir a la calle: **Conducir → Simular recorrido**.

### 5.2 Firmware del ESP32
1. IDE de Arduino 2.x con el paquete **esp32 de Espressif 3.x** (Gestor de tarjetas).
2. Placa: **ESP32 Dev Module**.
3. Partition Scheme:
   - Firmware BLE: `Huge APP (3MB No OTA/1MB SPIFFS)`
   - Firmware registrador: `No OTA (1MB APP/3MB SPIFFS)`
4. Abrir el `.ino` correspondiente de `firmware/` y subirlo. No requiere librerías externas.
5. Verificar en el Monitor Serie (115200 para BLE, 921600 para el registrador): en reposo `Total ≈ 1,00 g`.
6. Con el firmware BLE, en la app conectar el dispositivo **"PIN-Tunja-ESP32"**.

### 5.3 Descarga de datos del ESP32 (firmware registrador)
```bash
pip install pyserial
python scripts/descargar_esp32.py          # detecta el puerto
python scripts/descargar_esp32.py COM5     # o indicando el puerto
```
Crea `datos_esp32/` con un CSV por sesión: `t_s, ax_g, ay_g, az_g, total_g, choque, bache, boton`. Cerrar el Monitor Serie antes de ejecutarlo.

### 5.4 Cuaderno de procesamiento
```bash
pip install pandas numpy matplotlib jupyter
jupyter notebook notebooks/
```
También se puede abrir en Google Colab subiendo el `.ipynb` y los CSV de `datos/`.

## 6. Datos

- `datos/scraping_precios_amortiguador_delantero.csv`: resultado crudo del web scraping (Falabella y Éxito, 26-nov-2026), con tienda, técnica, nombre, precio, vendedor, disponibilidad y URL.
- `datos/precios_limpios.csv` y `datos/precios.json`: ofertas limpias (filtro de relevancia + IQR) e historial con mediana, mínimo, máximo, Q1 y Q3.
- `notebooks/PinTunja_adquisicion_procesamiento.ipynb`: cadena ETL del scraping, merge sort, búsqueda binaria, EDA de precios y procesamiento de la señal del ESP32 (ventanas + clasificador por umbrales).
- `docs/`: informe técnico en español e inglés (PDF).
- Captura de referencia: 24-nov-2026, Avenida Oriental, ~1,02 km, acelerómetro a ~200,9 Hz, GPS a 1 Hz.

## 7. Limitaciones

- La detección automática usa umbrales; no se entrenó un modelo de aprendizaje automático.
- El scraping depende de que las tiendas no cambien sus páginas; MercadoLibre bloquea consultas automáticas.
- La duración de batería mostrada es una estimación con el consumo del momento.

## 8. Declaración de uso de IA

El equipo usó asistentes de IA (Claude, ChatGPT, Gemini y GitHub Copilot) como apoyo en la depuración de código y en la estructuración de la documentación técnica. El diseño, las pruebas, la toma de datos y el informe son responsabilidad del equipo.

## 9. Licencia

Uso académico — Universidad Santo Tomás, Seccional Tunja, 2026.
