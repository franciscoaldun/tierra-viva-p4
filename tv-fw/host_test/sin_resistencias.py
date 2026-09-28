# ¿Se puede sin resistencias? Simulación del "modo 1 cable".
# Un solo pin, con su fuerza más débil, directo al centro del RCA: la tele (75 ohm) hace de divisor y el pin
# sólo da dos voltajes (0 o ~1,2 V). Los niveles intermedios se fabrican con un tramado sigma-delta muy rápido:
# la tele promedia (su ancho de banda es ~4 MHz) y ve el nivel medio.
# Toma el cuadro de 6 bits ya generado, lo pasa a voltaje ideal y lo convierte a 1 bit (orden 1 o 2).
#   python sin_resistencias.py cuadro.bin tabla.json salida_1bit.bin [--orden 2] [--vhigh 1.2] [--sin-burst]
import argparse, json
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("cuadro"); ap.add_argument("tabla"); ap.add_argument("salida")
ap.add_argument("--orden", type=int, default=2)
ap.add_argument("--vhigh", type=float, default=1.2)
ap.add_argument("--sin-burst", action="store_true", help="sin burst: la tele muestra blanco y negro (sin ruido de color)")
a = ap.parse_args()

fb = np.fromfile(a.cuadro, dtype=np.uint8).reshape(262, 910)
v = np.array(json.load(open(a.tabla))["v"])
x = v[fb].astype(float)                        # voltaje deseado de cada muestra
if a.sin_burst:
    x[:, 76:112] = np.median(x[9:20, 120:130])  # el burst pasa a nivel de blanking
d = np.clip(x / a.vhigh, 0, 1).reshape(-1)    # densidad deseada (0..1)
out = np.zeros(len(d), dtype=np.uint8)
# sigma-delta: el error se arrastra muestra a muestra (y de una línea a la siguiente: la señal es continua)
e1 = e2 = 0.0
for k in range(3):                             # 3 vueltas para que el estado se asiente en el cuadro repetido
    for i in range(len(d)):
        if a.orden == 1:
            u = d[i] + e1
            y = 1.0 if u >= 0.5 else 0.0
            e1 = u - y
        else:                                  # orden 2 (ruido empujado más arriba, lejos de lo que ve la tele)
            u = d[i] + 2 * e1 - e2
            y = 1.0 if u >= 0.5 else 0.0
            e2 = e1
            e1 = max(-1.5, min(1.5, u - y))
        out[i] = int(y)
out.tofile(a.salida)
json.dump({"bits": 1, "v": [0.0, a.vhigh]}, open(a.salida + ".json", "w"))
print(f"{a.salida}: 1 bit, orden {a.orden}, densidad media {out.mean():.3f}")
