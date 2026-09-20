"""Rebuild lab_characters.blend; never save the source lab scene."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bpy
from character_assets import ASSETS, CHARACTERS, heroine_materials, import_character, pack_images
from character_motion import FPS, sample_clip, bake, stance_speed
from character_round2 import share_resources, lift_salsa


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = 'METRIC'
    scene.unit_settings.scale_length = 1
    scene.render.fps = FPS
    materials = heroine_materials()
    rigs = [import_character(*config, materials) for config in CHARACTERS]
    share_resources(rigs)
    clips = [sample_clip(rig) for rig in rigs]
    cycle_frames = len(clips[0][1])-1
    total_frames = round((2*19.1/stance_speed(clips[0])+3)*FPS/cycle_frames)*cycle_frames
    scene.frame_start, scene.frame_end = 0, total_frames
    print(f'[build] period {total_frames/FPS:.6f}s; {total_frames} intervals at {FPS} fps', flush=True)
    for rig, clip in zip(rigs, clips):
        bake(rig, clip, total_frames)
    lift_salsa(next(r for r in rigs if r.name == 'salsa_armature'), total_frames)
    scene.frame_set(0)
    pack_images()
    scene['character_period_frames'] = total_frames
    bpy.context.preferences.filepaths.save_version = 0
    bpy.ops.wm.save_as_mainfile(filepath=str(ASSETS / 'lab_characters.blend'))


if __name__ == '__main__':
    main()

