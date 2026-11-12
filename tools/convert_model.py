"""Converte um personagem glTF (skinned, com texturas PNG) para o formato .mdl do jogo.

Uso:
  python tools/convert_model.py <entrada.gltf> <saida.mdl> [--tex 1024] [--acc tipo:id:arquivo.gltf ...]

  --acc  acessorio estatico (cabelo, barba) preso ao osso "Head".
         tipo 2 = cabelo (id = numero do estilo), tipo 3 = barba.

Nao depende de nada alem de Python + numpy: o PNG e decodificado aqui mesmo.

Formato .mdl (little-endian):
    char  magic[4] = "MDL2"
    u32   nVerts, nIdx, nJoints, nParts, nTex
    nVerts  * { f32 pos[3], f32 nrm[3], f32 uv[2], u8 joints[4], f32 weights[4] }
    nIdx    * u32
    nJoints * { char name[32], i32 parent, f32 restLocal[16], f32 invBind[16] }   (matrizes column-major)
    nParts  * { u32 firstIndex, u32 indexCount, i32 texture, i32 type, i32 id }
              type: 0 = corpo, 1 = sobrancelha (recebe a cor do cabelo), 2 = cabelo, 3 = barba
    nTex    * { u32 w, u32 h, u8 rgba[w*h*4] }
"""
import json
import os
import struct
import sys
import zlib

import numpy as np

COMPONENT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_accessor(g, buf, idx):
    acc = g["accessors"][idx]
    bv = g["bufferViews"][acc["bufferView"]]
    dtype = np.dtype(COMPONENT[acc["componentType"]])
    n = NCOMP[acc["type"]]
    count = acc["count"]
    start = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", dtype.itemsize * n)
    raw = np.frombuffer(buf, dtype=np.uint8, count=stride * (count - 1) + dtype.itemsize * n, offset=start)
    rows = np.lib.stride_tricks.as_strided(raw, shape=(count, dtype.itemsize * n), strides=(stride, 1))
    return np.ascontiguousarray(rows).view(dtype).reshape(count, n)


def decode_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "nao e PNG: " + path
    pos, idat = 8, []
    while pos < len(data):
        ln, = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, bd, ct, _, _, il = struct.unpack(">IIBBBBB", body)
            assert bd == 8 and ct in (2, 6) and il == 0, "PNG nao suportado (8 bits RGB/RGBA sem interlace)"
        elif typ == b"IDAT":
            idat.append(body)
        pos += 12 + ln
    bpp = 3 if ct == 2 else 4
    stride = w * bpp
    raw = zlib.decompress(b"".join(idat))
    out = np.zeros((h, stride), dtype=np.uint8)
    prev = bytearray(stride)
    for y in range(h):
        off = y * (stride + 1)
        ft = raw[off]
        cur = bytearray(raw[off + 1:off + 1 + stride])
        if ft == 1:       # Sub
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 255
        elif ft == 2:     # Up
            cur = bytearray(((np.frombuffer(cur, np.uint8).astype(np.uint16) + np.frombuffer(prev, np.uint8)) & 255).astype(np.uint8))
        elif ft == 3:     # Average
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((a + prev[i]) >> 1)) & 255
        elif ft == 4:     # Paeth
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
        out[y] = np.frombuffer(cur, np.uint8)
        prev = cur
    img = out.reshape(h, w, bpp)
    if bpp == 3:
        img = np.concatenate([img, np.full((h, w, 1), 255, np.uint8)], axis=2)
    return img


def shrink(img, max_size):
    while img.shape[0] > max_size or img.shape[1] > max_size:
        h, w = img.shape[0] // 2 * 2, img.shape[1] // 2 * 2
        a = img[:h, :w].astype(np.uint16)
        img = ((a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2]) // 4).astype(np.uint8)
    return img


def node_matrix(n):
    if "matrix" in n:
        return np.array(n["matrix"], dtype=np.float64).reshape(4, 4).T
    t = n.get("translation", [0, 0, 0])
    q = n.get("rotation", [0, 0, 0, 1])
    s = n.get("scale", [1, 1, 1])
    x, y, z, w = q
    r = np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])
    m = np.eye(4)
    m[:3, :3] = r * np.array(s)[None, :]
    m[:3, 3] = t
    return m


class Builder:
    def __init__(self, main_base):
        self.main_base = main_base
        self.pos, self.nrm, self.uv, self.jts, self.wts, self.idx = [], [], [], [], [], []
        self.nverts = 0
        self.nidx = 0
        self.parts = []          # (first, count, tex, type, id)
        self.tex_paths = []      # caminhos dos PNGs, sem repetir

    def tex_index(self, gfile_dir, uri):
        for base in (gfile_dir, self.main_base):
            p = os.path.join(base, uri)
            if os.path.exists(p):
                key = os.path.abspath(p)
                if key not in self.tex_paths:
                    self.tex_paths.append(key)
                return self.tex_paths.index(key)
        raise FileNotFoundError(uri)

    def add_prim(self, g, buf, gdir, prim, xform, ptype, pid, head_joint):
        a = prim["attributes"]
        pos = read_accessor(g, buf, a["POSITION"]).astype(np.float64)
        nrm = read_accessor(g, buf, a["NORMAL"]).astype(np.float64)
        uv = read_accessor(g, buf, a["TEXCOORD_0"]).astype(np.float32)
        ind = read_accessor(g, buf, prim["indices"]).astype(np.uint32).reshape(-1)
        n = len(pos)
        if head_joint is None:   # corpo: ja vem com pesos de esqueleto
            jts = read_accessor(g, buf, a["JOINTS_0"]).astype(np.uint8)
            wts = read_accessor(g, buf, a["WEIGHTS_0"]).astype(np.float32)
            wts /= np.maximum(wts.sum(axis=1, keepdims=True), 1e-8)
        else:                    # acessorio rigido: 100% preso ao osso da cabeca
            jts = np.zeros((n, 4), dtype=np.uint8)
            jts[:, 0] = head_joint
            wts = np.zeros((n, 4), dtype=np.float32)
            wts[:, 0] = 1.0
        if xform is not None:
            m3 = xform[:3, :3]
            pos = pos @ m3.T + xform[:3, 3]
            nrm = nrm @ np.linalg.inv(m3)          # (M^-T n) em forma de linha
            nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-8)
        mat = g["materials"][prim["material"]]
        ti = mat["pbrMetallicRoughness"]["baseColorTexture"]["index"]
        uri = g["images"][g["textures"][ti]["source"]]["uri"]
        tex = self.tex_index(gdir, uri)
        self.parts.append((self.nidx, len(ind), tex, ptype, pid))
        self.pos.append(pos.astype(np.float32)); self.nrm.append(nrm.astype(np.float32)); self.uv.append(uv)
        self.jts.append(jts); self.wts.append(wts)
        self.idx.append(ind + self.nverts)
        self.nverts += n
        self.nidx += len(ind)


def main():
    args = sys.argv[1:]
    src, dst = args[0], args[1]
    max_tex = 1024
    accs = []
    i = 2
    while i < len(args):
        if args[i] == "--tex":
            max_tex = int(args[i + 1]); i += 2
        elif args[i] == "--acc":
            t, pid, path = args[i + 1].split(":", 2)
            accs.append((int(t), int(pid), path)); i += 2
        else:
            raise SystemExit("argumento desconhecido: " + args[i])

    base = os.path.dirname(os.path.abspath(src))
    g = json.load(open(src))
    buf = open(os.path.join(base, g["buffers"][0]["uri"]), "rb").read()
    nodes = g["nodes"]
    skin = g["skins"][0]

    # --- esqueleto
    joint_nodes = skin["joints"]
    parent_node = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent_node[c] = i
    jindex = {nd: i for i, nd in enumerate(joint_nodes)}
    inv = read_accessor(g, buf, skin["inverseBindMatrices"]).astype(np.float32)
    joints = []
    for i, nd in enumerate(joint_nodes):
        p = parent_node.get(nd)
        pj = jindex.get(p, -1)
        local = node_matrix(nodes[nd])
        if pj < 0:    # raiz: incorpora os nos ancestrais que nao sao ossos
            while p is not None:
                local = node_matrix(nodes[p]) @ local
                p = parent_node.get(p)
        assert pj < i, "ossos precisam vir depois do pai"
        joints.append((nodes[nd]["name"], pj, local, inv[i]))
    names = [j[0] for j in joints]
    head = names.index("Head")

    # --- corpo
    b = Builder(base)
    for mesh in g["meshes"]:
        for prim in mesh["primitives"]:
            mat = g["materials"][prim["material"]]
            ti = mat["pbrMetallicRoughness"]["baseColorTexture"]["index"]
            uri = g["images"][g["textures"][ti]["source"]]["uri"]
            ptype = 1 if uri.startswith("T_Hair") else 0     # sobrancelhas usam a textura de cabelo
            b.add_prim(g, buf, base, prim, None, ptype, 0, None)

    # --- acessorios (cabelo / barba)
    for ptype, pid, path in accs:
        ag = json.load(open(path))
        adir = os.path.dirname(os.path.abspath(path))
        abuf = open(os.path.join(adir, ag["buffers"][0]["uri"]), "rb").read()
        for n in ag["nodes"]:
            if "mesh" not in n:
                continue
            xf = node_matrix(n)
            for prim in ag["meshes"][n["mesh"]]["primitives"]:
                start = len(b.pos)
                b.add_prim(ag, abuf, adir, prim, xf, ptype, pid, head)
                p = b.pos[-1]
                print("  acessorio", os.path.basename(path), "y:", p[:, 1].min().round(2), "->", p[:, 1].max().round(2),
                      " z:", p[:, 2].min().round(2), "->", p[:, 2].max().round(2))

    pos = np.concatenate(b.pos)
    nrm = np.concatenate(b.nrm)
    uv = np.concatenate(b.uv)
    jts = np.concatenate(b.jts)
    wts = np.concatenate(b.wts)
    indices = np.concatenate(b.idx).astype(np.uint32)

    out = bytearray()
    out += b"MDL2" + struct.pack("<5I", len(pos), len(indices), len(joints), len(b.parts), len(b.tex_paths))
    vd = np.zeros(len(pos), dtype=[("p", "<f4", 3), ("n", "<f4", 3), ("uv", "<f4", 2), ("j", "u1", 4), ("w", "<f4", 4)])
    vd["p"], vd["n"], vd["uv"], vd["j"], vd["w"] = pos, nrm, uv, jts, wts
    out += vd.tobytes()
    out += indices.tobytes()
    for name, pj, local, ib in joints:
        out += name.encode("ascii")[:31].ljust(32, b"\0")
        out += struct.pack("<i", pj)
        out += local.astype(np.float32).T.reshape(-1).tobytes()    # column-major
        out += np.asarray(ib, dtype=np.float32).reshape(-1).tobytes()  # ja esta column-major no glTF
    for first, count, tex, ptype, pid in b.parts:
        out += struct.pack("<IIiii", first, count, tex, ptype, pid)
    for path in b.tex_paths:
        img = shrink(decode_png(path), max_tex)
        out += struct.pack("<II", img.shape[1], img.shape[0]) + img.tobytes()
        print("textura", os.path.basename(path), img.shape[1], "x", img.shape[0])

    open(dst, "wb").write(out)
    print("ok:", dst, len(out) // 1024, "KB;", len(pos), "vertices,", len(indices) // 3, "triangulos,",
          len(joints), "ossos,", len(b.parts), "partes")


main()
