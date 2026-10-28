# Steel Horizon RTS

A new native real-time naval battle game for Linux (C11, SDL2, OpenGL 3.3), built
around the battle layer of *Steel Horizon* (PSP, Konami / Climax, 2007). The
original game was studied for its formats and rules (`notes/`, `evidence/`); the
engine in `engine/` is new code that loads the models, textures, sound and data
from your own copy of the game at runtime.

**No game data is included.** You need your own ISO of *Steel Horizon (USA)*
(SHA-256 `6e00ac4ba1f91385b8065c5be142fa73d45a2d58b45aae2cc18634c02d7b3e9c`).
Without one, `--standalone` runs on built-in ship data with generated models.

## Build

```
cmake -S engine -B engine/build -G Ninja && ninja -C engine/build
```

## Game files from the ISO

```
7z x -oassets/iso "Steel Horizon (USA).iso" PSP_GAME/USRDIR
python3 -I tools/imp_extract.py assets/iso/PSP_GAME/USRDIR/GAMEDATA.{XML,IMP} assets/files
python3 -I tools/data2json.py assets/files assets/data
python3 -I tools/at3wav.py assets/files/sound/music assets/music   # needs ffmpeg
```

## Play

```
engine/build/shrts --assets assets        # menu: tutorial, skirmish, custom battle, online
engine/build/shrts --standalone           # no game files
```

Controls are in the in-game tutorial and the controls panel (F1).
