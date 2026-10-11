# Exports tools/bench/assets/bench_scene.blend to bench_scene.glb for pw_bench (--gltf), in background Blender:
#
#   "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup ^
#       tools\bench\assets\bench_scene.blend --python tools\bench\blender\export_bench_scene.py -- tools\bench\assets\bench_scene.glb
#
# The tinted base colours are composed into packed images before the export (scene_export.py); the .blend on
# disk is not modified. lab_scene.blend also gets the characters of lab_characters.blend (character_export.py).
import bpy, sys, os
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from character_export import append_characters, validate_glb
from scene_export import compose_tinted_base_colors, export_glb

character_rigs = append_characters()

out = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else os.path.splitext(bpy.data.filepath)[0] + ".glb"


if Path(out).suffix.lower() != '.glb':
    raise ValueError('Output must be a .glb file')

compose_tinted_base_colors()
export_glb(out, character_rigs)
validate_glb(out, character_rigs)
