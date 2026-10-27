"""Parser for Steel Horizon PSP models (.pingu, magic "gniP PSP").

Layout (see notes/formats.md; checked against all 258 files):
  0x00  char[8]  "gniP PSP"
  0x08  f32      version (2.01)
  0x0c  u32      1
  0x10  f32[4]   bbox min (x, y, z, 0)
  0x20  f32[4]   bbox max
  0x30  f32      bounding radius
  0x34  f32      position scale: vertex pos = s8 / 128 * scale
  0x38  u32      mesh count
  0x3c  char[4]  "MAT ", u32 1, u32 material count M (at 0x44)
  0x6c  M x 0x100 material records, texture name first
  0x70+M*0x100   render-state records, 0x24 bytes each; u8 at +0x1e = material index
                 (0xff = none), u8 at +0x1f flags (0x40 on damaged models)
  ...            mesh records: u16 nverts, u16 0, u16 nprims, u8 render-state index
  ...            GE display lists: BASE, OFFSET, VTYPE, TEXSCALE/OFFSET U/V, VADDR,
                 PRIM..., RET -- one per mesh, then one extra RET
  RET+12         vertex data, all meshes back to back, in display-list order

Every mesh in the game uses VTYPE 0xa1 (u8 texcoord, s8 normal, s8 position;
8 bytes per vertex) and triangle strips (PRIM type 4). VADDR values are not
file offsets; their meaning is an open question (notes/unknowns.md).
"""
import struct

MAGIC = b"gniP PSP"
VTYPE_A1 = 0xa1
PRIM_STRIP = 4
MAT_SIZE = 0x100
RS_SIZE = 0x24
GE_BASE, GE_OFFSET, GE_VTYPE, GE_VADDR, GE_PRIM, GE_RET = 0x10, 0x14, 0x12, 0x01, 0x04, 0x0b
GE_TSU, GE_TSV, GE_TOU, GE_TOV = 0x48, 0x49, 0x4a, 0x4b


def f24(v):
    """GE 24-bit float parameter -> float (the top 24 bits of an IEEE f32)."""
    return struct.unpack("<f", struct.pack("<I", (v & 0xffffff) << 8))[0]


class Mesh:
    def __init__(self):
        self.vtype = None
        self.strips = []          # vertex counts per strip
        self.tex_scale = [1.0, 1.0]
        self.tex_offset = [0.0, 0.0]
        self.material = None      # index into Pingu.materials, or None
        self.positions = []       # (x, y, z) floats
        self.normals = []
        self.uvs = []

    @property
    def vertex_count(self):
        return sum(self.strips)

    def triangles(self):
        """Triangle-list indices from the strips, keeping consistent winding."""
        out = []
        base = 0
        for n in self.strips:
            for i in range(n - 2):
                a, b, c = base + i, base + i + 1, base + i + 2
                if i & 1:
                    a, b = b, a
                if len({a, b, c}) == 3:
                    out.append((a, b, c))
            base += n
        return out


class Pingu:
    def __init__(self, data):
        if data[:8] != MAGIC:
            raise ValueError("not a .pingu file")
        (self.version,) = struct.unpack_from("<f", data, 8)
        self.bbox_min = struct.unpack_from("<3f", data, 0x10)
        self.bbox_max = struct.unpack_from("<3f", data, 0x20)
        self.radius, self.pos_scale = struct.unpack_from("<2f", data, 0x30)
        (nmesh,) = struct.unpack_from("<I", data, 0x38)
        if data[0x3c:0x40] != b"MAT ":
            raise ValueError("missing MAT chunk")
        (nmat,) = struct.unpack_from("<I", data, 0x44)
        self.materials = []
        for i in range(nmat):
            o = 0x6c + i * MAT_SIZE
            self.materials.append(data[o:o + 0x40].split(b"\0")[0].decode("ascii"))
        self._rs_base = 0x70 + nmat * MAT_SIZE
        dl = self._find_display_lists(data, self._rs_base)
        self.meshes, end = self._parse_display_lists(data, dl)
        if len(self.meshes) != nmesh:
            raise ValueError(f"{len(self.meshes)} display lists, header says {nmesh}")
        self._assign_materials(data, dl)
        self._read_vertices(data, end + 12)

    @staticmethod
    def _find_display_lists(data, start):
        for o in range(start, len(data) - 8, 4):
            if struct.unpack_from("<II", data, o) == (GE_BASE << 24, GE_OFFSET << 24):
                return o
        raise ValueError("no display list found")

    @staticmethod
    def _parse_display_lists(data, o):
        meshes = []
        cur = None
        while o < len(data) - 4:
            (v,) = struct.unpack_from("<I", data, o)
            cmd, arg = v >> 24, v & 0xffffff
            o += 4
            if cmd == GE_BASE:
                cur = Mesh()
                meshes.append(cur)
            elif cmd == GE_RET:
                if cur is None:
                    return meshes, o
                cur = None
            elif v == 0 or cur is None:
                continue
            elif cmd == GE_VTYPE:
                if arg != VTYPE_A1:
                    raise ValueError(f"unsupported VTYPE {arg:#x}")
                cur.vtype = arg
            elif cmd == GE_PRIM:
                if (arg >> 16) & 7 != PRIM_STRIP:
                    raise ValueError(f"unsupported PRIM type {(arg >> 16) & 7}")
                cur.strips.append(arg & 0xffff)
            elif cmd == GE_TSU:
                cur.tex_scale[0] = f24(arg)
            elif cmd == GE_TSV:
                cur.tex_scale[1] = f24(arg)
            elif cmd == GE_TOU:
                cur.tex_offset[0] = f24(arg)
            elif cmd == GE_TOV:
                cur.tex_offset[1] = f24(arg)
            elif cmd not in (GE_OFFSET, GE_VADDR):
                raise ValueError(f"unexpected GE command {v:#010x} at {o - 4:#x}")
        raise ValueError("display lists not terminated")

    def _assign_materials(self, data, dl_start):
        # Mesh records sit before the display lists; find each by its
        # (nverts, 0, nprims) signature and read the render-state index after it.
        lo = self._rs_base
        for mesh in self.meshes:
            sig = struct.pack("<HHH", mesh.vertex_count, 0, len(mesh.strips))
            at = data.find(sig, lo, dl_start)
            if at < 0:
                continue
            rs = data[at + 6]
            lo = at + 6
            tex = data[self._rs_base + rs * RS_SIZE + 0x1e]   # 0xff = untextured
            if tex < len(self.materials):
                mesh.material = tex

    def _read_vertices(self, data, o):
        for mesh in self.meshes:
            su, sv = mesh.tex_scale
            ou, ov = mesh.tex_offset
            for _ in range(mesh.vertex_count):
                tu, tv, nx, ny, nz, px, py, pz = struct.unpack_from("<BBbbbbbb", data, o)
                o += 8
                s = self.pos_scale / 128.0
                mesh.positions.append((px * s, py * s, pz * s))
                mesh.normals.append((nx / 127.0, ny / 127.0, nz / 127.0))
                mesh.uvs.append((tu / 128.0 * su + ou, tv / 128.0 * sv + ov))
        if o != len(data):
            raise ValueError(f"vertex data ends at {o:#x}, file at {len(data):#x}")


class Points:
    """Mount points (.pts, magic "POINT 1.00"): turret/gun positions.

    Layout: char[16] magic, u32 count, then count x 0x50-byte records:
    char[32] name, f32[3] position, f32[9] 3x3 matrix M (row-major).

    M is in the authoring tool's axes; the rotation in model space is Q.M with
    Q = rows (-1,0,0)(0,0,1)(0,1,0). With that, 716 of 832 mounts are upright
    at yaw 0 (fore), aft ones at 180, port/starboard (P*/S*) at -/+90, and only
    the wrecked *03 models tilt. Model parts are Y-up with +Z forward.
    """
    Q = (-1, 0, 0, 0, 0, 1, 0, 1, 0)

    def __init__(self, data):
        if not data.startswith(b"POINT 1.00"):
            raise ValueError("not a .pts file")
        (n,) = struct.unpack_from("<I", data, 0x10)
        self.points = []
        for i in range(n):
            o = 0x14 + i * 0x50
            name = data[o:o + 32].split(b"\0")[0].decode("ascii")
            pos = struct.unpack_from("<3f", data, o + 32)
            m = struct.unpack_from("<9f", data, o + 44)
            q = self.Q
            rot = tuple(sum(q[r * 3 + k] * m[k * 3 + c] for k in range(3))
                        for r in range(3) for c in range(3))
            self.points.append((name, pos, rot))   # rot: row-major 3x3, model space
