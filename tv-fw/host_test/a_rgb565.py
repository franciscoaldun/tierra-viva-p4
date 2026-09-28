# Convierte una imagen (jpg/png) a RGB565 crudo de 480x270, como el cuadro chico de BIOMA (s_small).
import sys
import numpy as np
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB").resize((480, 270), Image.BOX)
a = np.asarray(im).astype(np.uint16)
c = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
c.astype("<u2").tofile(sys.argv[2])
print(sys.argv[2], "480x270 RGB565")
