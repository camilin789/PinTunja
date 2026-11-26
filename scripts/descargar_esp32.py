"""
PIN Tunja - Descarga los datos grabados en el ESP32 y los guarda como CSV.

Uso:
    pip install pyserial
    python descargar_esp32.py            (busca el puerto solo)
    python descargar_esp32.py COM5       (o indicando el puerto)

Crea la carpeta datos_esp32/ con un CSV por sesión:
    t_s, ax_g, ay_g, az_g, total_g, choque, bache, boton
Al final pregunta si quieres borrar la memoria del ESP32.
Cierra el Monitor Serie del IDE de Arduino antes de ejecutarlo.
"""
import csv
import math
import os
import struct
import sys
import time
from datetime import datetime

import serial
import serial.tools.list_ports

BAUD = 921600
FS = 100.0                 # lecturas por segundo
REG = struct.Struct("<hhhH")  # ax, ay, az, marcas (8 bytes)
CUENTAS_G = 16384.0


def buscar_puerto():
    puertos = list(serial.tools.list_ports.comports())
    for p in puertos:
        desc = f"{p.description} {p.hwid}".upper()
        if any(k in desc for k in ("CP210", "CH340", "CH910", "USB SERIAL", "SILICON LABS", "UART")):
            return p.device
    if len(puertos) == 1:
        return puertos[0].device
    print("No encontré el ESP32. Puertos disponibles:")
    for p in puertos:
        print(f"  {p.device}: {p.description}")
    sys.exit("Conéctalo o indica el puerto, por ejemplo: python descargar_esp32.py COM5")


def abrir(puerto):
    ser = serial.Serial()
    ser.port = puerto
    ser.baudrate = BAUD
    ser.timeout = 10
    ser.dtr = False   # evita reiniciar la placa al abrir el puerto
    ser.rts = False
    ser.open()
    time.sleep(0.5)
    ser.reset_input_buffer()
    return ser


def leer_exacto(ser, n):
    datos = bytearray()
    while len(datos) < n:
        trozo = ser.read(n - len(datos))
        if not trozo:
            raise TimeoutError(f"Se cortó la descarga ({len(datos)} de {n} bytes)")
        datos += trozo
    return bytes(datos)


def a_csv(nombre, datos, carpeta, sello):
    ruta = os.path.join(carpeta, f"{sello}_{nombre.strip('/').replace('.bin', '')}.csv")
    n = len(datos) // REG.size
    baches = botones = 0
    with open(ruta, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["t_s", "ax_g", "ay_g", "az_g", "total_g", "choque", "bache", "boton"])
        for i in range(n):
            ax, ay, az, m = REG.unpack_from(datos, i * REG.size)
            gx, gy, gz = ax / CUENTAS_G, ay / CUENTAS_G, az / CUENTAS_G
            total = math.sqrt(gx * gx + gy * gy + gz * gz)
            choque, bache, boton = m & 1, (m >> 1) & 1, (m >> 2) & 1
            baches += bache
            botones += boton
            w.writerow([f"{i / FS:.2f}", f"{gx:.4f}", f"{gy:.4f}", f"{gz:.4f}", f"{total:.4f}",
                        choque, bache, boton])
    print(f"  {ruta}: {n} lecturas (~{n / FS / 60:.1f} min), baches detectados = {baches}")
    return ruta


def main():
    puerto = sys.argv[1] if len(sys.argv) > 1 else buscar_puerto()
    print(f"Conectando a {puerto} ...")
    ser = abrir(puerto)
    ser.write(b"D")

    carpeta = "datos_esp32"
    os.makedirs(carpeta, exist_ok=True)
    sello = datetime.now().strftime("%Y%m%d_%H%M")
    archivos = 0

    while True:
        linea = ser.readline()
        if not linea:
            sys.exit("El ESP32 no respondió. Revisa el puerto y que el Monitor Serie esté cerrado.")
        texto = linea.decode("utf-8", "ignore").strip()
        if texto == "END":
            break
        if texto.startswith("FILE "):
            _, nombre, tam = texto.split()
            print(f"Descargando {nombre} ({int(tam) / 1024:.0f} KB) ...")
            datos = leer_exacto(ser, int(tam))
            a_csv(nombre, datos, carpeta, sello)
            archivos += 1

    print(f"Listo: {archivos} sesión(es) guardadas en la carpeta {os.path.abspath(carpeta)}")
    if archivos and input("¿Borrar la memoria del ESP32 para la próxima toma? (s/n): ").strip().lower() == "s":
        ser.write(b"X")
        time.sleep(2)
        print(ser.read_all().decode("utf-8", "ignore").strip())
        ser.write(b"G")
    ser.close()


if __name__ == "__main__":
    main()
