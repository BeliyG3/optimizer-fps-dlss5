"""CuteGirl G1 asset: source / output paths, the head object and the CC4 morph-target name tables."""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
ROOT = os.path.join(BENCH, "assets", "external", "cutegirl")
SRC = os.path.join(ROOT, "gltf", "scene.gltf")
DST = os.path.join(ROOT, "cutegirl.glb")

# ---------------------------------------------------------------------------
# Morph target names.
#
# The Sketchfab glTF carries NO morph-target names: every shape key comes in as
# ``target_<i>`` (glTF has no ``mesh.extras.targetNames`` and no per-primitive extras).
# The real Character Creator 4 names live in the accompanying FBX
# (``source/source/CuteGirl G1.fbx``); its blend-shape channel order matches the glTF
# target order 1:1 per mesh:
#
#   FBX CC_Base_Body        152 keys  ->  glTF meshes "1" (Std_Skin_Head, 4232 v)
#                                        and "2" (Std_Eyelash, 748 v), both 152 targets
#   FBX CC_Base_Tongue       48 keys  ->  glTF mesh "0" (331 v, 48 targets)
#   FBX CC_Base_TearLine    104 keys  ->  glTF meshes "3"/"4" (98 v, 60 targets)
#   FBX CC_Base_EyeOcclusion 106 keys ->  glTF meshes "5"/"6" (91 v, 60 targets)
#   FBX CC_Base_Eye           2 keys  ->  glTF meshes "7".."10" (165 v, 2 targets)
#
# The tearline / eye-occlusion meshes keep only the first 60 channels -- exactly the
# shared face-rig subset; the mesh-private "TL */EO *" channels were dropped by the
# Sketchfab converter -- so an index map is still valid for them.
#
# The tables below are that FBX order, transcribed. ``NAMES_60`` is the common prefix of
# the tearline / eye-occlusion lists.
# ---------------------------------------------------------------------------

NAMES_152 = [
    "V_Open", "V_Explosive", "V_Dental_Lip", "V_Tight_O", "V_Tight", "V_Wide",
    "V_Affricate", "V_Lip_Open",
    "Brow_Raise_Inner_L", "Brow_Raise_Inner_R", "Brow_Raise_Outer_L", "Brow_Raise_Outer_R",
    "Brow_Drop_L", "Brow_Drop_R", "Brow_Compress_L", "Brow_Compress_R",
    "Eye_Blink_L", "Eye_Blink_R", "Eye_Squint_L", "Eye_Squint_R", "Eye_Wide_L", "Eye_Wide_R",
    "Eye_L_Look_L", "Eye_R_Look_L", "Eye_L_Look_R", "Eye_R_Look_R",
    "Eye_L_Look_Up", "Eye_R_Look_Up", "Eye_L_Look_Down", "Eye_R_Look_Down",
    "Eyelash_Upper_Up_L", "Eyelash_Upper_Down_L", "Eyelash_Upper_Up_R", "Eyelash_Upper_Down_R",
    "Eyelash_Lower_Up_L", "Eyelash_Lower_Down_L", "Eyelash_Lower_Up_R", "Eyelash_Lower_Down_R",
    "Ear_Up_L", "Ear_Up_R", "Ear_Down_L", "Ear_Down_R", "Ear_Out_L", "Ear_Out_R",
    "Nose_Sneer_L", "Nose_Sneer_R", "Nose_Nostril_Raise_L", "Nose_Nostril_Raise_R",
    "Nose_Nostril_Dilate_L", "Nose_Nostril_Dilate_R", "Nose_Crease_L", "Nose_Crease_R",
    "Nose_Nostril_Down_L", "Nose_Nostril_Down_R", "Nose_Nostril_In_L", "Nose_Nostril_In_R",
    "Nose_Tip_L", "Nose_Tip_R", "Nose_Tip_Up", "Nose_Tip_Down",
    "Cheek_Raise_L", "Cheek_Raise_R", "Cheek_Suck_L", "Cheek_Suck_R",
    "Cheek_Puff_L", "Cheek_Puff_R",
    "Mouth_Smile_L", "Mouth_Smile_R", "Mouth_Smile_Sharp_L", "Mouth_Smile_Sharp_R",
    "Mouth_Frown_L", "Mouth_Frown_R", "Mouth_Stretch_L", "Mouth_Stretch_R",
    "Mouth_Dimple_L", "Mouth_Dimple_R", "Mouth_Press_L", "Mouth_Press_R",
    "Mouth_Tighten_L", "Mouth_Tighten_R", "Mouth_Blow_L", "Mouth_Blow_R",
    "Mouth_Pucker_Up_L", "Mouth_Pucker_Up_R", "Mouth_Pucker_Down_L", "Mouth_Pucker_Down_R",
    "Mouth_Funnel_Up_L", "Mouth_Funnel_Up_R", "Mouth_Funnel_Down_L", "Mouth_Funnel_Down_R",
    "Mouth_Roll_In_Upper_L", "Mouth_Roll_In_Upper_R", "Mouth_Roll_In_Lower_L",
    "Mouth_Roll_In_Lower_R", "Mouth_Roll_Out_Upper_L", "Mouth_Roll_Out_Upper_R",
    "Mouth_Roll_Out_Lower_L", "Mouth_Roll_Out_Lower_R",
    "Mouth_Push_Upper_L", "Mouth_Push_Upper_R", "Mouth_Push_Lower_L", "Mouth_Push_Lower_R",
    "Mouth_Pull_Upper_L", "Mouth_Pull_Upper_R", "Mouth_Pull_Lower_L", "Mouth_Pull_Lower_R",
    "Mouth_Up", "Mouth_Down", "Mouth_L", "Mouth_R",
    "Mouth_Upper_L", "Mouth_Upper_R", "Mouth_Lower_L", "Mouth_Lower_R",
    "Mouth_Shrug_Upper", "Mouth_Shrug_Lower", "Mouth_Drop_Upper", "Mouth_Drop_Lower",
    "Mouth_Up_Upper_L", "Mouth_Up_Upper_R", "Mouth_Down_Lower_L", "Mouth_Down_Lower_R",
    "Mouth_Chin_Up", "Mouth_Close", "Mouth_Contract",
    "Tongue_Bulge_L", "Tongue_Bulge_R",
    "Jaw_Open", "Jaw_Forward", "Jaw_Backward", "Jaw_L", "Jaw_R", "Jaw_Up", "Jaw_Down",
    "Neck_Swallow_Up", "Neck_Swallow_Down", "Neck_Tighten_L", "Neck_Tighten_R",
    "Head_Turn_Up", "Head_Turn_Down", "Head_Turn_L", "Head_Turn_R",
    "Head_Tilt_L", "Head_Tilt_R", "Head_L", "Head_R", "Head_Forward", "Head_Backward",
    "Eyelid_Outer_Down_L", "Eyelid_Outer_Down_R", "Eyelid_Inner_Down_L", "Eyelid_Inner_Down_R",
]

NAMES_48 = [
    "V_Open", "V_Dental_Lip", "V_Tight_O", "V_Tongue_up", "V_Tongue_Raise", "V_Tongue_Out",
    "V_Tongue_Narrow", "V_Tongue_Lower", "V_Tongue_Curl_U", "V_Tongue_Curl_D",
    "Mouth_Smile_L", "Mouth_Drop_Lower", "Mouth_Down_Lower_L", "Mouth_Close", "Mouth_Contract",
    "Tongue_Out", "Tongue_In", "Tongue_Up", "Tongue_Down", "Tongue_Mid_Up", "Tongue_Tip_Up",
    "Tongue_Tip_Down", "Tongue_Narrow", "Tongue_Wide", "Tongue_Roll", "Tongue_L", "Tongue_R",
    "Tongue_Tip_L", "Tongue_Tip_R", "Tongue_Twist_L", "Tongue_Twist_R",
    "Tongue_Bulge_L", "Tongue_Bulge_R", "Tongue_Extend", "Tongue_Enlarge",
    "Jaw_Open", "Jaw_Forward", "Jaw_L", "Jaw_R", "Jaw_Up", "Jaw_Down",
    "Neck_Tighten_L", "Neck_Tighten_R",
    "Head_Turn_Up", "Head_Turn_Down", "Head_Tilt_L", "Head_L", "Head_Backward",
]

NAMES_60 = [
    "V_Open", "V_Explosive", "V_Dental_Lip", "V_Wide", "V_Affricate",
    "Brow_Raise_Inner_L", "Brow_Raise_Inner_R", "Brow_Raise_Outer_L", "Brow_Raise_Outer_R",
    "Brow_Drop_L", "Brow_Drop_R", "Brow_Compress_L", "Brow_Compress_R",
    "Eye_Blink_L", "Eye_Blink_R", "Eye_Squint_L", "Eye_Squint_R", "Eye_Wide_L", "Eye_Wide_R",
    "Eye_L_Look_L", "Eye_R_Look_L", "Eye_L_Look_R", "Eye_R_Look_R",
    "Eye_L_Look_Up", "Eye_R_Look_Up", "Eye_L_Look_Down", "Eye_R_Look_Down",
    "Nose_Sneer_L", "Nose_Sneer_R", "Nose_Tip_L", "Nose_Tip_R",
    "Cheek_Raise_L", "Cheek_Raise_R", "Cheek_Puff_L", "Cheek_Puff_R",
    "Mouth_Smile_L", "Mouth_Smile_R", "Mouth_Smile_Sharp_L", "Mouth_Smile_Sharp_R",
    "Mouth_Dimple_L", "Mouth_Press_L", "Mouth_Press_R", "Mouth_Down", "Mouth_L", "Mouth_R",
    "Mouth_Drop_Lower", "Mouth_Up_Upper_R", "Mouth_Down_Lower_L", "Mouth_Close", "Jaw_Down",
    "Head_Turn_Up", "Head_Turn_Down", "Head_Turn_L", "Head_Turn_R", "Head_Tilt_L",
    "Head_Tilt_R", "Eyelid_Outer_Down_L", "Eyelid_Outer_Down_R",
    "Eyelid_Inner_Down_L", "Eyelid_Inner_Down_R",
]

NAME_TABLES = {152: NAMES_152, 48: NAMES_48, 60: NAMES_60}

HEAD_OBJ = "1"          # material Std_Skin_Head, 4232 verts, 152 targets
