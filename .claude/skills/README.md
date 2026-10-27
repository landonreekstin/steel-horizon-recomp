# Vendored game-dev skills

Engine-agnostic "disciplines" skills from
https://github.com/gamedev-skills/awesome-gamedev-agent-skills
(commit 184b72be02884f782a0058f2eeae2ce4277453e4, Apache-2.0, see LICENSE/NOTICE).

Left out on purpose: the router (it routes by engine and falls back to Godot, which is
wrong for this C11/SDL2/GL engine), the per-engine skill sets (Godot/Unity/Unreal/...),
genres, workflows, and dialogue-systems. When a skill shows engine API examples, treat
them as concepts and implement them in `engine/` in C.

Update: re-clone the repo and copy `skills/disciplines/<name>/` over these folders.
