# Elige las 6 resistencias del "DAC" (una por pin, IO16..IO21) entre valores comunes de los kits baratos.
# Mide qué tan parejos quedan los 64 voltajes: el peor salto entre niveles vecinos y el error medio al pedir
# un voltaje cualquiera entre 0 y 1,1 V (lo que usa la señal de TV). Mismo modelo que ntsc_core.c.
import itertools, sys
import numpy as np

KIT = [100, 150, 200, 220, 270, 330, 470, 510, 680, 1000, 2000, 2200, 3300, 4700, 5100, 6800, 10000, 20000]
VDD, RG, RL = 3.3, 30.0, 75.0

def niveles(rs):
    g = np.array([1.0 / (r + RG) for r in rs])
    gs = g.sum() + 1.0 / RL
    codes = np.arange(1 << len(rs))
    bits = ((codes[:, None] >> np.arange(len(rs))) & 1).astype(float)
    return VDD * (bits @ g) / gs

def evaluar(rs, vtope=1.10):
    v = np.sort(niveles(rs))
    if v[-1] < vtope:
        return None
    t = np.linspace(0, vtope, 2000)
    idx = np.searchsorted(v, t)
    idx = np.clip(idx, 1, len(v) - 1)
    err = np.minimum(np.abs(v[idx] - t), np.abs(v[idx - 1] - t))
    usados = v[v <= vtope + 0.02]
    salto = np.max(np.diff(usados))
    return salto, float(np.sqrt(np.mean(err ** 2))), v[-1]

def main():
    mejores = []
    # la del bit más pesado entre 150 y 330 ohm (para llegar a ~1,1 V sobre 75 ohm); las demás, más grandes y en orden
    for combo in itertools.combinations(sorted(KIT, reverse=True), 6):
        rs = list(combo)                  # de mayor a menor = bit 0 (IO16) ... bit 5 (IO21)
        if not (150 <= rs[5] <= 330):
            continue
        e = evaluar(rs)
        if e:
            mejores.append((e[1], e[0], e[2], rs))
    mejores.sort()
    print("rms_mV  peor_salto_mV  vmax_V   resistencias IO16..IO21")
    for rms, salto, vmax, rs in mejores[:12]:
        print(f"{rms*1000:6.2f}  {salto*1000:12.1f}  {vmax:6.3f}   {rs}")
    ideal = 1.1 / 63
    print(f"\nreferencia: un DAC perfecto de 6 bits hasta 1,1 V tendría saltos de {ideal*1000:.1f} mV y rms {ideal/np.sqrt(12)*1000:.2f} mV")
    for rs in ([8200, 3900, 2000, 1000, 510, 240], [10000, 4700, 2200, 1000, 470, 220], [10000, 4700, 2200, 1000, 510, 270]):
        e = evaluar(rs)
        print("candidata", rs, "→", None if e is None else f"rms {e[1]*1000:.2f} mV, peor salto {e[0]*1000:.1f} mV, vmax {e[2]:.3f} V")

if __name__ == "__main__":
    main()
