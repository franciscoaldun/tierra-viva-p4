# Convierte las texturas de la NASA (dominio público) al formato que lee el P4 (1024x512):
#   dia565.bin  - Blue Marble (tierra + océanos de día), RGB565 little-endian
#   luces8.bin  - Black Marble 2012 (luces de ciudades de noche), gris 0..255
#   mar8.bin    - máscara de océano (brillo del sol reflejado), gris 0..255
#   nubes8.bin  - nubes de respaldo (las reales se bajan por internet cada 3 h), gris 0..255
# Además deja vistas previas PNG para revisar a ojo.
import numpy as np
from PIL import Image
import os

W, H = 1024, 512
aqui = os.path.dirname(os.path.abspath(__file__))
tx = os.path.join(aqui, "texturas")


def cargar(nombre):
    return np.asarray(Image.open(os.path.join(tx, nombre)).convert("RGB").resize((W, H), Image.LANCZOS)).astype(np.float32)


dia = cargar("dia.jpg")
r, g, b = dia[..., 0], dia[..., 1], dia[..., 2]
rgb565 = ((r.astype(np.uint16) >> 3) << 11) | ((g.astype(np.uint16) >> 2) << 5) | (b.astype(np.uint16) >> 3)
rgb565.astype("<u2").tofile(os.path.join(aqui, "dia565.bin"))

# océano: azul que domina al rojo (en Blue Marble el mar es azul oscuro y la tierra verde/café/blanca)
mar = np.clip((b - r - 12) * 6.0, 0, 255)
mar = np.asarray(Image.fromarray(mar.astype(np.uint8)).filter(__import__("PIL.ImageFilter", fromlist=["x"]).GaussianBlur(1.2)))
mar.astype(np.uint8).tofile(os.path.join(aqui, "mar8.bin"))

noche = cargar("noche.jpg")
lum = 0.5 * noche[..., 0] + 0.4 * noche[..., 1] + 0.1 * noche[..., 2]
luces = np.clip((lum - 38.0) * 1.6, 0, 255)          # el fondo de tierra/mar oscuro queda en 0
luces = 255.0 * (luces / 255.0) ** 0.8
luces.astype(np.uint8).tofile(os.path.join(aqui, "luces8.bin"))

nub = np.asarray(Image.open(os.path.join(tx, "nubes.jpg")).convert("L").resize((W, H), Image.LANCZOS))
nub.astype(np.uint8).tofile(os.path.join(aqui, "nubes8.bin"))

for nombre, arr in (("prev_mar.png", mar), ("prev_luces.png", luces), ("prev_nubes.png", nub)):
    Image.fromarray(np.asarray(arr).astype(np.uint8)).save(os.path.join(tx, nombre))
Image.fromarray(dia.astype(np.uint8)).save(os.path.join(tx, "prev_dia.png"))
print("listo:", [(f, os.path.getsize(os.path.join(aqui, f))) for f in ("dia565.bin", "luces8.bin", "mar8.bin", "nubes8.bin")])
