# Genera tv-fw/main/letra.h: letra proporcional nítida para los textos de TIERRA VIVA (al tamaño real de la
# pantalla, sin achicar). Glifos A8 (alfa 0-255) de alto fijo y ancho variable.
# Uso: python gen_letra.py
from PIL import Image, ImageDraw, ImageFont
import os

H, SIZE = 16, 13
FUENTE = r"C:\Windows\Fonts\seguisb.ttf"          # Segoe UI Semibold
EXTRA = "°áéíóúñÑ¿¡·→ÁÉÍÓÚü"
chars = [chr(c) for c in range(32, 127)] + list(EXTRA)
font = ImageFont.truetype(FUENTE, SIZE)
asc, desc = font.getmetrics()
top = (H - (asc + desc)) // 2 + 1

anchos, datos, offs = [], [], []
for ch in chars:
    adv = int(round(font.getlength(ch)))
    w = max(adv, 1) + 1
    img = Image.new("L", (w, H), 0)
    ImageDraw.Draw(img).text((0, top), ch, font=font, fill=255)
    offs.append(len(datos))
    datos.extend(img.getdata())
    anchos.append(w)

out = ["// Generado por tierra/gen_letra.py - Segoe UI Semibold %dpx, alto %d, ancho variable, A8\n" % (SIZE, H),
       "#pragma once\n#include <stdint.h>\n",
       "#define LETRA_H %d\n#define LETRA_N %d\n" % (H, len(chars)),
       "static const uint32_t letra_cp[LETRA_N] = {%s};\n" % ",".join(str(ord(c)) for c in chars),
       "static const uint8_t letra_w[LETRA_N] = {%s};\n" % ",".join(map(str, anchos)),
       "static const uint32_t letra_off[LETRA_N] = {%s};\n" % ",".join(map(str, offs)),
       "static const uint8_t letra_a8[%d] = {\n" % len(datos)]
for i in range(0, len(datos), 40):
    out.append("  " + ",".join(map(str, datos[i:i + 40])) + ",\n")
out.append("};\n")
dst = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tv-fw", "main", "letra.h")
open(dst, "w", encoding="utf-8").write("".join(out))
print("letra.h:", len(chars), "glifos, alto", H, ",", len(datos), "bytes")
