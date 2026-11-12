# Gera assets\tiritos.ico: o icone do jogo (uma mira vermelha sobre fundo escuro), em 16, 24, 32, 48, 64 e 256 px.
# So usa a biblioteca padrao do Python. Uso: python tools\make_icon.py
import math
import os
import struct
import zlib

SIZES = [16, 24, 32, 48, 64, 256]


def pixel(x, y, n):
    """Cor RGBA do pixel (x, y) num icone n x n, com bordas suavizadas (4x4 amostras por pixel)."""
    acc = [0.0, 0.0, 0.0, 0.0]
    ss = 4
    for sy in range(ss):
        for sx in range(ss):
            u = (x + (sx + 0.5) / ss) / n * 2.0 - 1.0     # -1..1
            v = (y + (sy + 0.5) / ss) / n * 2.0 - 1.0
            c = sample(u, v)
            a = c[3]
            for i in range(3):
                acc[i] += c[i] * a
            acc[3] += a
    k = ss * ss
    a = acc[3] / k
    if a <= 0.0:
        return (0, 0, 0, 0)
    return tuple(int(round(min(255.0, acc[i] / acc[3] * 255.0))) for i in range(3)) + (int(round(a * 255.0)),)


def sample(u, v):
    # fundo: quadrado de cantos arredondados, cinza-azulado escuro com leve degrade
    r = 0.30
    qx, qy = max(abs(u) - (1.0 - r), 0.0), max(abs(v) - (1.0 - r), 0.0)
    if math.hypot(qx, qy) > r:
        return (0, 0, 0, 0.0)
    shade = 0.13 + 0.05 * (1.0 - (v + 1.0) * 0.5)
    col = (shade, shade + 0.015, shade + 0.04, 1.0)
    d = math.hypot(u, v)
    red = (0.92, 0.16, 0.18, 1.0)
    white = (0.96, 0.96, 0.96, 1.0)
    # anel da mira
    if abs(d - 0.52) < 0.075:
        return red
    # tracos da mira (com um vao no meio)
    if (abs(u) < 0.06 and 0.18 < abs(v) < 0.80) or (abs(v) < 0.06 and 0.18 < abs(u) < 0.80):
        return white
    # ponto central
    if d < 0.085:
        return red
    return col


def png(n):
    rows = []
    for y in range(n):
        row = bytearray([0])                      # filtro 0 em cada linha
        for x in range(n):
            row.extend(pixel(x, y, n))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main():
    images = [png(n) for n in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for n, data in zip(SIZES, images):
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "tiritos.ico")
    with open(out, "wb") as f:
        f.write(header + entries + b"".join(images))
    print("icone gerado:", os.path.normpath(out))


if __name__ == "__main__":
    main()
