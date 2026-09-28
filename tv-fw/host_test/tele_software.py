# Televisor NTSC de software: mira la señal del P4 sin tener una tele.
# Hace lo mismo que el circuito de una tele de verdad, sin mirar cómo se generó la señal:
#   1) códigos del DAC → voltios (tabla del DAC) → modelo analógico (retención de 70 ns + paso bajo del cable/entrada)
#   2) separa los sincronismos por umbral (mitad entre la punta de sync y el blanking), mide su ancho y los clasifica
#   3) encuentra el sincronismo vertical (pulsos anchos) y arma el cuadro con los sincronismos horizontales reales
#   4) en cada línea: se engancha al burst (fase y amplitud), demodula U y V, saca la luma y arma RGB
# Uso:
#   python tele_software.py cuadro.bin [tabla.json] [--repetir 3] [--tolerancia 0.05] [--fc 6e6] [--ruido 0.002]
#                           [--png salida.png] [--bits 6] [--carta]
# Entrada: 1 byte por muestra a 14,318181 MHz (el cuadro del host o la captura hecha por el propio P4).
import argparse, json, math, sys
import numpy as np

FS = 315e6 / 88 * 4          # 14,318181 MHz
FSC = FS / 4
OS = 4                        # sobremuestreo del modelo analógico
US = 1e-6

def fir_pasobajo(fc, fs, taps):
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * fc / fs * n) * np.blackman(taps)
    return h / h.sum()

def tabla_dac(args, bits):
    if args.tabla:
        t = json.load(open(args.tabla))
        v = np.array(t["v"], dtype=float)
    else:
        rs = [10000, 4700, 2200, 1000, 510, 270][:bits]
        v = None
    if args.tolerancia > 0:
        # resistencias reales con tolerancia: se recalcula la tabla con valores sorteados (la tele ve ESTOS voltajes)
        rng = np.random.default_rng(args.semilla)
        rs = np.array(args.resistencias, dtype=float) * (1 + rng.uniform(-args.tolerancia, args.tolerancia, len(args.resistencias)))
        g = 1 / (rs + 30.0)
        codes = np.arange(1 << len(rs))
        b = ((codes[:, None] >> np.arange(len(rs))) & 1).astype(float)
        v = 3.3 * (b @ g) / (g.sum() + 1 / 75.0)
        print(f"  (tolerancia ±{args.tolerancia*100:.0f} %: resistencias sorteadas {np.round(rs).astype(int).tolist()})")
    return v

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cuadro")
    ap.add_argument("tabla", nargs="?")
    ap.add_argument("--repetir", type=int, default=1, help="repetir el cuadro (el del host es uno solo)")
    ap.add_argument("--tolerancia", type=float, default=0.0)
    ap.add_argument("--semilla", type=int, default=1)
    ap.add_argument("--resistencias", type=str, default="10000,4700,2200,1000,510,270")
    ap.add_argument("--fc", type=float, default=6e6, help="paso bajo del cable y la entrada de la tele")
    ap.add_argument("--ruido", type=float, default=0.0, help="ruido en voltios rms")
    ap.add_argument("--png", default=None)
    ap.add_argument("--bits", type=int, default=6)
    ap.add_argument("--carta", action="store_true", help="comparar las barras de color con lo esperado")
    ap.add_argument("--setup", type=float, default=7.5, help="nivel de negro que supone la tele (IRE)")
    args = ap.parse_args()
    args.resistencias = [float(x) for x in args.resistencias.split(",")]

    raw = np.fromfile(args.cuadro, dtype=np.uint8)
    codes = (raw & ((1 << args.bits) - 1)).astype(np.int64)
    codes = np.tile(codes, args.repetir)
    v_tab = tabla_dac(args, args.bits)
    if v_tab is None:
        print("falta la tabla del DAC (json)"); sys.exit(1)
    v = v_tab[codes]

    # modelo analógico: cada muestra se sostiene 1/FS (DAC), luego un paso bajo de un polo (cable + entrada)
    x = np.repeat(v, OS)
    fs2 = FS * OS
    a = math.exp(-2 * math.pi * args.fc / fs2)
    from scipy.signal import lfilter
    x = lfilter([1 - a], [1, -a], x)
    if args.ruido > 0:
        x = x + np.random.default_rng(7).normal(0, args.ruido, len(x))

    # ---------- separador de sincronismos ----------
    # como en una tele: se mira la señal suavizada (~0,5 µs), así el tramado o el ruido no disparan pulsos falsos
    nsm = int(0.5 * US * fs2)
    xs = np.convolve(x, np.ones(nsm) / nsm, "same")
    tip = np.percentile(xs, 1.0)
    hist, edges = np.histogram(xs, bins=600, range=(tip, tip + 0.6))
    lo = np.searchsorted(edges, tip + 0.08)
    blank0 = edges[lo + np.argmax(hist[lo:])]
    thr = (tip + blank0) / 2
    bajo = xs < thr
    d = np.diff(bajo.astype(np.int8))
    caidas = np.where(d == 1)[0] + 1
    subidas = np.where(d == -1)[0] + 1
    if len(caidas) < 10:
        print("no encuentro sincronismos: ¿la señal está?"); sys.exit(1)
    # tiempo fino de cada flanco de bajada (interpolado)
    tc = caidas - 1 + (xs[caidas - 1] - thr) / (xs[caidas - 1] - xs[caidas])
    s_idx = np.searchsorted(subidas, caidas)
    ok = s_idx < len(subidas)
    tc, caidas = tc[ok], caidas[ok]
    ts = subidas[s_idx[ok]].astype(float)
    ancho_us = (ts - tc) / fs2 / US
    tipo = np.full(len(tc), "?", dtype=object)
    tipo[(ancho_us >= 1.5) & (ancho_us < 3.5)] = "E"
    tipo[(ancho_us >= 3.5) & (ancho_us < 7.0)] = "H"
    tipo[ancho_us >= 15] = "V"

    tline = 910 * OS
    hs = tc[tipo == "H"]
    dif = np.diff(hs)
    dif_l = dif[(dif > 0.9 * tline) & (dif < 1.1 * tline)]
    fh = fs2 / np.mean(dif_l) if len(dif_l) else float("nan")
    anchos_h = ancho_us[tipo == "H"]

    # ---------- sincronismo vertical: primer pulso ancho de cada grupo ----------
    iv = np.where(tipo == "V")[0]
    grupos = [i for k, i in enumerate(iv) if k == 0 or iv[k - 1] != i - 1]
    if len(grupos) < 1:
        print("no hay sincronismo vertical"); sys.exit(1)
    tv = tc[grupos]
    fv = fs2 / np.mean(np.diff(tv)) if len(tv) > 1 else float("nan")
    print(f"sincronismos: horizontal {fh:.2f} Hz (norma 15734,26), vertical {fv:.3f} Hz (240p: 60,05), "
          f"ancho H {np.mean(anchos_h):.2f} µs (norma 4,7), pulsos anchos por cuadro {len(iv)/max(len(grupos),1):.0f}, "
          f"punta {tip*1000:.0f} mV, blanking {blank0*1000:.0f} mV")

    # ---------- armar un cuadro a partir del primer vertical que tenga un cuadro completo detrás ----------
    t_v = None
    for t in tv:
        if t + 262 * tline < len(x) - tline:
            t_v = t; break
    if t_v is None:
        print("la captura no alcanza para un cuadro completo después del vertical"); sys.exit(1)
    n_act, w_act = 240, 754
    img = np.zeros((n_act, w_act, 3))
    fases, amps, blanks, amp_sync = [], [], [], []
    w = 2 * math.pi * FSC / fs2
    lp_c = fir_pasobajo(1.3e6, fs2, 177)
    lp_y = fir_pasobajo(4.2e6, fs2, 97)
    for a in range(n_act):
        L = 20 + a                                     # línea del cuadro (la primera de pulsos anchos es la 3)
        esperado = t_v + (L - 3) * tline
        k = np.argmin(np.abs(hs - esperado))
        t0 = hs[k]
        if abs(t0 - esperado) > 0.1 * tline:
            continue
        i0 = int(t0)
        seg = lambda u0, u1: x[i0 + int(u0 * US * fs2): i0 + int(u1 * US * fs2)]
        blank = np.mean(seg(8.0, 9.2))
        punta = np.mean(seg(1.0, 4.0))
        # burst: ajuste a·sin(wt) + b·cos(wt) con el tiempo absoluto (la tele se engancha a este oscilador)
        j0, j1 = i0 + int(5.6 * US * fs2), i0 + int(7.6 * US * fs2)
        tt = np.arange(j0, j1)
        M = np.stack([np.sin(w * tt), np.cos(w * tt), np.ones_like(tt, dtype=float)], 1)
        (ca, cb, cc), *_ = np.linalg.lstsq(M, x[j0:j1], rcond=None)
        fb = math.atan2(cb, ca)
        ab = math.hypot(ca, cb)
        fases.append(fb); amps.append(ab); blanks.append(blank); amp_sync.append(blank - punta)
        # área activa: 9,4 µs a 62,06 µs después del sincronismo
        k0, k1 = i0 + int(round(9.4 * US * fs2)), i0 + int(round(9.4 * US * fs2)) + w_act * OS
        tt = np.arange(k0 - 200, k1 + 200)
        s = x[k0 - 200:k1 + 200] - blank
        ru, rv = np.sin(w * tt + fb - math.pi), np.cos(w * tt + fb - math.pi)
        u = np.convolve(2 * s * ru, lp_c, "same")
        vv = np.convolve(2 * s * rv, lp_c, "same")
        yv = np.convolve(s - (u * ru + vv * rv), lp_y, "same")
        ire = 40.0 / (blank - punta)                  # la tele mide la amplitud del sincronismo (40 IRE)
        acc = 20.0 / (ab * ire)                       # control automático de color: el burst vale 20 IRE
        Y = (yv * ire - args.setup) / (100 - args.setup)
        U = u * ire * acc / (100 - args.setup)
        V = vv * ire * acc / (100 - args.setup)
        R = Y + V / 0.877
        B = Y + U / 0.493
        G = (Y - 0.299 * R - 0.114 * B) / 0.587
        rgb = np.stack([R, G, B], 1)[200:200 + w_act * OS:OS]
        img[a] = np.clip(rgb, 0, 1)
    fases = np.unwrap(np.array(fases))
    amps = np.array(amps); asy = np.array(amp_sync)
    burst_ire = np.mean(amps * 2 * 40 / asy)
    print(f"burst: {burst_ire:.1f} IRE p-p (norma 40), fase {math.degrees(np.mean(fases)) % 360:.1f}° "
          f"con dispersión {math.degrees(np.std(fases)):.2f}° entre líneas (0 = subportadora continua), "
          f"sincronismo {np.mean(asy)*1000:.0f} mV (norma 286)")

    if args.carta:
        # centro de cada barra al 75 % (líneas 20..120 de la imagen, muestras de la imagen 152..871 → activa 17..736)
        esperado = [(191,191,191),(191,191,0),(0,191,191),(0,191,0),(191,0,191),(191,0,0),(0,0,191)]
        nombres = ["blanco75","amarillo","cian","verde","magenta","rojo","azul"]
        errs = []
        for i in range(7):
            xc = 17 + int((i + 0.5) * 720 / 7)
            m = img[8:46, xc - 12:xc + 12].reshape(-1, 3).mean(0) * 255   # arriba del texto "BIOMA" (filas 51-99)
            e = np.array(esperado[i])
            errs.append(np.abs(m - e).max())
            print(f"  barra {nombres[i]:9s}: decodificada ({m[0]:5.1f},{m[1]:5.1f},{m[2]:5.1f})  esperada {esperado[i]}  error máx {errs[-1]:4.1f}")
        print(f"  error máximo en las barras: {max(errs):.1f} de 255")
        g = img[185, 17:737].mean(1) * 255
        print(f"  escala de grises (línea 185): negro {g[5]:.0f} · medio {g[360]:.0f} · blanco {g[-6]:.0f} (esperado 0 · 128 · 255)")

    if args.png:
        from PIL import Image
        im = Image.fromarray((img * 255).astype(np.uint8))
        im.save(args.png)
        # vista "como en la tele": 4:3, líneas dobladas
        im.resize((640, 480), Image.BILINEAR).save(args.png.replace(".png", "_4x3.png"))
        print(f"imagen: {args.png} (y {args.png.replace('.png', '_4x3.png')})")

if __name__ == "__main__":
    main()
