#!/usr/bin/env python3
"""Software-render a .pingu model to PNG, for checking the converters by eye.

Usage: python3 -I tools/preview.py FILES_DIR MODEL.pingu OUT.png [GUN.pingu] [yaw pitch]

FILES_DIR is the extracted IMP root (textures are decoded from .bog directly).
If GUN.pingu is given it is drawn at every F*/R* mount point of the model's
.pts file. Flat-shaded by the vertex normals, z-buffered, nearest texel.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bog import Bog, write_png  # noqa: E402
from pingu import Pingu, Points  # noqa: E402

W, H = 960, 480


def load_textures(files_dir, names):
    index = {}
    for root, _, fs in os.walk(os.path.join(files_dir, "textures")):
        for f in fs:
            if f.endswith(".bog"):
                index[f[:-4].lower()] = os.path.join(root, f)
    out = []
    for n in names:
        p = index.get(n.lower())
        if p:
            with open(p, "rb") as f:
                out.append(Bog(f.read()).level(0))
        else:
            out.append(None)
    return out


def instances(model, transform):
    """Yield (triangle world positions, uvs, normal, texture) for a model."""
    for mesh in model.meshes:
        for a, b, c in mesh.triangles():
            pts = [transform(mesh.positions[i]) for i in (a, b, c)]
            uvs = [mesh.uvs[i] for i in (a, b, c)]
            yield pts, uvs, mesh.material


def render(tris, textures_by_model, yaw, pitch, out):
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)

    def view(p):
        x, y, z = p
        x, z = x * cy - z * sy, x * sy + z * cy
        y, z = y * cp - z * sp, y * sp + z * cp
        return x, y, z

    vt = [([view(p) for p in pts], uvs, mat, key) for pts, uvs, mat, key in tris]
    xs = [p[0] for t in vt for p in t[0]]
    ys = [p[1] for t in vt for p in t[0]]
    scale = 0.9 * min(W / (max(xs) - min(xs)), H / (max(ys) - min(ys)))
    cx, cyy = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2
    img = bytearray(b"\x20\x28\x38\xff" * (W * H))
    zbuf = [1e30] * (W * H)
    light = (0.4, 0.8, -0.45)
    for pts, uvs, mat, key in vt:
        s = [((p[0] - cx) * scale + W / 2, H / 2 - (p[1] - cyy) * scale, p[2]) for p in pts]
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = s
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            continue
        # face normal in view space for shading (two-sided)
        ux, uy, uz = [pts[1][k] - pts[0][k] for k in range(3)]
        wx, wy, wz = [pts[2][k] - pts[0][k] for k in range(3)]
        nx, ny, nz = uy * wz - uz * wy, uz * wx - ux * wz, ux * wy - uy * wx
        nl = math.sqrt(nx * nx + ny * ny + nz * nz) or 1
        shade = 0.35 + 0.65 * abs((nx * light[0] + ny * light[1] + nz * light[2]) / nl)
        tex = textures_by_model[key][mat] if mat is not None else None
        for py in range(max(0, int(min(y0, y1, y2))), min(H, int(max(y0, y1, y2)) + 1)):
            for px in range(max(0, int(min(x0, x1, x2))), min(W, int(max(x0, x1, x2)) + 1)):
                fx, fy = px + 0.5, py + 0.5
                w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area
                w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w0 * z0 + w1 * z1 + w2 * z2
                i = py * W + px
                if z >= zbuf[i]:
                    continue
                zbuf[i] = z
                if tex:
                    tw, th, rgba = tex
                    u = w0 * uvs[0][0] + w1 * uvs[1][0] + w2 * uvs[2][0]
                    v = w0 * uvs[0][1] + w1 * uvs[1][1] + w2 * uvs[2][1]
                    t = ((int(v * th) % th) * tw + int(u * tw) % tw) * 4
                    r, g, b = rgba[t], rgba[t + 1], rgba[t + 2]
                else:
                    r = g = b = 128
                img[i * 4: i * 4 + 4] = bytes((int(r * shade), int(g * shade),
                                               int(b * shade), 255))
    write_png(out, W, H, bytes(img))


def main(files_dir, model_path, out, gun_path=None, yaw=-0.9, pitch=-0.45):
    with open(model_path, "rb") as f:
        model = Pingu(f.read())
    tris = [(p, u, m, 0) for p, u, m in instances(model, lambda p: p)]
    textures = {0: load_textures(files_dir, model.materials)}
    pts_path = model_path[:-6] + ".pts"
    if gun_path and os.path.exists(pts_path):
        with open(gun_path, "rb") as f:
            gun = Pingu(f.read())
        textures[1] = load_textures(files_dir, gun.materials)
        with open(pts_path, "rb") as f:
            mounts = Points(f.read()).points
        for name, pos, r in mounts:
            if name[0] not in "FR":
                continue

            def xf(p, pos=pos, r=r):
                x, y, z = p
                return (r[0] * x + r[1] * y + r[2] * z + pos[0],
                        r[3] * x + r[4] * y + r[5] * z + pos[1],
                        r[6] * x + r[7] * y + r[8] * z + pos[2])
            tris += [(p, u, m, 1) for p, u, m in instances(gun, xf)]
    render(tris, textures, float(yaw), float(pitch), out)


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5, 7):
        sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
