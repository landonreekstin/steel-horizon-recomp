# Runtime observations (PPSSPP 1.20.4)

The user's ISO (game id **ULUS10215, version 1.03**) run in PPSSPP, driven through
its WebSocket debugger (`tools/ppsspp_dbg.py`). How to set it up:

- `ppsspp-headless ISO --debugger=PORT` boots and runs fine, and memory reads
  work, but no backend can capture frames (software/GLES: "Could not download
  output"; Vulkan crashes headless).
- For screenshots: GUI `ppsspp` on a private `Xvfb :99`, with its own
  `XDG_CONFIG_HOME` whose `ppsspp.ini` sets `[General] RemoteDebuggerOnStartup =
  True` and `FirstRun = False`; the port shows up in `ss -ltnp` (it binds all
  interfaces despite `RemoteDebuggerLocal`, so keep sessions short). Frames come
  from `import -window root` on :99; the debugger's own capture fails there too.
- `cpu.stepping` / `cpu.resume` answer with untagged broadcast events.
- Buttons: `input.buttons.press {button, duration}` (cross, circle, start, up, …).

## Verified
- **Load base 0x08804000**; the `tools/prx2exec.py` image equals RAM after load
  (all 495,628 code words + 3,078 data words; only the 230 syscall stubs and 2
  PPSSPP function-replacement hooks differ).
- **Gun table** (object at `*0x089d2fa8`): 10 shell entries at +0x104 and 7 more at
  +0x74, stride 0x14 = `{key, baseDamage, minRange, maxRange, f32 fireRate}`, values
  exactly those of `maps/gunValues.xml`. Keys: shells 0..9 (18"..3"); 24"T 0,
  18"T 2, 300 lb DC 4, 600 lb DC 5. Aircraft entries and key 6 are not loaded.
- **Ship objects in the 3D battle** (Mission 01 tutorial, 4 US BB vs 4 German DD):
  class from the vtable at +0xc; position f32[3] at +0x40; unit rotation rows at
  +0x4c (right) / +0x5c (up) / +0x64 (forward); **scale vector (1,1,1) at +0x80**;
  hull type +0xb0; max/current HP s16 +0xc0/+0xc2; current max weapon range f32
  +0xf4 (3200 on a BB).
- **Scale**: models are drawn 1:1 in world units. Measured: battleships in a 2×2
  formation **600 units** apart; groups engaged at **~2,600 units** (inside the 3200
  max range of 16" guns). A 195-unit hull → distances are about 9× compressed
  against real gunnery if 1 unit ≈ 1.35 m.
- The tactical battle is **command-driven** (Move / Attack / Not Ready menu, radar)
  and resolved on its own here in under a minute; damage was visible mid-battle
  (DD HP 600 → 510 / 330 / 0).

## Not yet measured
- Ship speed in units/s (`+0xf0` read 41.8 / 36.7 on two stationary BBs — not speed
  as such, meaning unknown; `+0x170` = 49.58 on every ship).
- Live hit/damage rolls against `notes/battle-logic.md` (set breakpoints on
  0x088e94fc / 0x088cb818 in a longer battle).
