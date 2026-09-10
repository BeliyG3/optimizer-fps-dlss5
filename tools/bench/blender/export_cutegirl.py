r"""Repack the Sketchfab "CuteGirl G1" head (gltf/scene.gltf) as a single self-contained
``cutegirl.glb`` for the bench: skin (102 joints), the 12.1 s "Emote" action (bone TRS +
morph weights), 152 morph targets on the head mesh, textures embedded.

Nothing is baked -- the armature and the shape keys survive the round trip so the bench
loader does the skinning / morphing itself.

On top of the imported "Emote" this script APPENDS a deliberate facial test segment at
13.0 s - 19.0 s (see ``build_face_test``), so the exported clip is 19.0 s long and the
last 6 s exercise the morph-target path hard (the mouth flapping wide open, then cheek
puffs, then the lips pushed forward into a tube) while the skeleton simply holds the
pose the Emote left it in.

Run headless:
    "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background \
        --factory-startup --python tools/bench/blender/export_cutegirl.py

Add ``-- --check`` to also write Workbench check renders next to the GLB:
    ... --python tools/bench/blender/export_cutegirl.py -- --check
"""

import math
import os
import sys

import bpy
import mathutils

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

# ---------------------------------------------------------------------------
# The two poses the test segment blends between.
#
# MEASURED on this asset (``report_keys`` prints the table): in this Character
# Creator 4 head ``Jaw_Open`` and ``V_Open`` do NOT part the lips -- CC4 keeps the
# lips sealed under the jaw morph and the lower lip is actually rolled *up*
# (inner-lip gap 0.09 -> -0.33 for Jaw_Open=1, -0.13 for V_Open=1).  The keys that
# really open the mouth are the lip movers:
#
#   Mouth_Drop_Lower    0.09 -> 0.98      Mouth_Up_Upper_L    0.09 -> 0.56
#   V_Dental_Lip        0.09 -> 0.87      Mouth_Shrug_Upper   0.09 -> 0.47
#   V_Affricate         0.09 -> 0.87      Mouth_Up_Upper_R    0.09 -> 0.42
#   Mouth_Down_Lower_L  0.09 -> 0.84      Mouth_Drop_Upper    0.09 -> -0.91
#   V_Lip_Open          0.09 -> 0.62      Mouth_Close         0.09 -> -2.74
#   Mouth_Down_Lower_R  0.09 -> 0.62
#
# so the open pose below drops the lower lip (Mouth_Drop_Lower + Mouth_Down_Lower)
# and lifts the upper one (Mouth_Up_Upper), with Jaw_Open kept as the chin/jaw
# drop that goes with it and a touch of Mouth_Stretch for width: gap 1.55 units
# (~18x the closed 0.087) and the upper teeth come into view.
#
# The teeth are separate, morph-less meshes driven by the CC_Base_JawRoot bone, so
# they stay put -- an open mouth on this asset shows the teeth rather than a black
# cavity.  That is expected; the skeleton deliberately holds the Emote's pose here.
# ---------------------------------------------------------------------------

OPEN_POSE = {
    "Mouth_Drop_Lower": 1.00,   # the actual opener: lower lip down
    "Mouth_Down_Lower_L": 0.50,
    "Mouth_Down_Lower_R": 0.50,
    "Mouth_Up_Upper_L": 0.25,   # upper lip up, so the parting reads from the front
    "Mouth_Up_Upper_R": 0.25,
    "Jaw_Open": 0.65,           # chin / jaw drop underneath it
    "Mouth_Stretch_L": 0.12,
    "Mouth_Stretch_R": 0.12,
}

# Cheek_Puff already carries the pursed mouth with it on this head (it moves the
# mouth corners by 1.6 units); the old Mouth_Blow / Mouth_Pucker helpers only added
# chin faceting, so they are gone.  Mouth_Press keeps the lips sealed during the puff.
PUFF_POSE = {
    "Cheek_Puff_L": 1.00,
    "Cheek_Puff_R": 1.00,
    "Mouth_Press_L": 0.20,
    "Mouth_Press_R": 0.20,
}

# ---------------------------------------------------------------------------
# The pucker ("губы трубочкой" -- lips pushed forward into a kiss/whistle tube).
#
# MEASURED on this head (``report_keys`` prints the table; protrusion is the mean
# forward displacement of the 8+8 midline lip-centre vertices along the face's forward
# axis, width is the distance between the two mouth-corner vertices -- the head vertices
# that Mouth_Stretch_L/_R move furthest -- 3.780 units apart in neutral):
#
#   key (weight 1.0)        prot. upper / lower   mouth width      chin
#   V_Tight_O                    0.578 / 0.657     -41.1 %        0.65
#   V_Tight                      0.585 / 0.533     -37.6 %        0.51
#   Mouth_Pucker_Up_L+R          0.594 / 0.150     -14.6 %        0.11
#   Mouth_Pucker_Down_L+R        0.000 / 0.400     -20.9 %        0.44
#   Mouth_Push_Upper_L+R         0.271 / 0.000      -1.2 %        0.07
#   Mouth_Funnel_Up_L+R          0.513 / 0.206      -5.4 %        0.04   (funnels open,
#   Mouth_Funnel_Down_L+R        0.000 / 0.400     -18.9 %        0.59    they do not purse)
#
# V_Tight_O is the single best pucker (the widest narrowing per unit of chin movement,
# and it leaves the small central opening a tube shape wants), the Mouth_Pucker pair adds
# the forward roll of both lips with almost no chin cost, and Mouth_Push_Upper pushes the
# upper lip out the last bit.  Together: protrusion 1.02 / 0.94 units, mouth width
# 3.780 -> 1.616 (-57 %), inner-lip gap 0.20 (closed = 0.087, so the lips stay all but
# shut) and 0.89 units of chin movement -- 3.4 % of the head height, i.e. the chin
# follows the lips without faceting.  The Funnel keys are deliberately unused: they part
# the lips (gap 0.68 / 0.81) instead of pursing them.
# ---------------------------------------------------------------------------
PUCKER_POSE = {
    "V_Tight_O": 0.85,          # the tube itself: lips pursed into a narrow O
    "Mouth_Pucker_Up_L": 0.60,  # both lips rolled forward
    "Mouth_Pucker_Up_R": 0.60,
    "Mouth_Pucker_Down_L": 0.60,
    "Mouth_Pucker_Down_R": 0.60,
    "Mouth_Push_Upper_L": 0.30,  # upper lip the last bit forward
    "Mouth_Push_Upper_R": 0.30,
}

# --- test segment layout (seconds; the imported clip runs at 24 fps) ----------
SEG_START_S = 13.0
FLAP_END_S = 15.0       # 13.0 - 15.0 s: jaw flapping
PUFF_END_S = 17.0       # 15.0 - 17.0 s: cheek puffs
SEG_END_S = 19.0        # 17.0 - 19.0 s: lips puckered into a tube
SETTLE_S = 12.5         # neutralise whatever expression the Emote ended on
FLAP_PERIOD_S = 0.5     # 4 open/close cycles in 2 s
PUFF_PERIOD_S = 1.0     # 2 puffs in 2 s
PUFF_RAMP_S = 0.40
PUFF_HOLD_S = 0.45
PUFF_RELEASE_S = 0.15
PUCKER_PERIOD_S = 1.0   # 2 puckers in 2 s
PUCKER_RAMP_S = 0.40
PUCKER_HOLD_S = 0.45
PUCKER_RELEASE_S = 0.15

KEYWORDS = ("jaw", "mouth", "cheek", "puff", "blow")

# every mouth-region channel is eased to 0 at the start of the test window, so the test
# runs off a clean neutral face whatever expression the Emote happened to end on; only
# the OPEN_POSE / PUFF_POSE members are then keyed per frame
POSE_NAMES = sorted(set(OPEN_POSE) | set(PUFF_POSE) | set(PUCKER_POSE))
RESET_NAMES = sorted(set(POSE_NAMES) | {
    n for n in NAMES_152 if n.startswith("V_") or any(k in n.lower() for k in KEYWORDS)})


def log(*a):
    print("[cutegirl]", *a)


# ---------------------------------------------------------------------------


def morph_meshes():
    """{object: {cc4_name: ShapeKey}} for every imported mesh with named-able morphs."""
    out = {}
    for obj in bpy.data.objects:
        if obj.type != "MESH" or not obj.data.shape_keys:
            continue
        kbs = obj.data.shape_keys.key_blocks
        table = NAME_TABLES.get(len(kbs) - 1)
        if not table:
            continue
        out[obj] = {name: kbs["target_%d" % i] for i, name in enumerate(table)}
    return out


def world_basis(obj):
    """Neutral vertex positions of ``obj`` in world space (the shape keys are local)."""
    m = obj.matrix_world
    return [m @ v.co for v in obj.data.shape_keys.key_blocks["Basis"].data], m


def inner_lip_pair(obj, table, opener="Mouth_Drop_Lower"):
    """(upper, lower) vertex indices of the sealed inner-lip contact at the midline.

    The lower one is the head vertex on the face midline that ``opener`` drags furthest
    down; the upper one is its sealed partner -- the nearest vertex above it that the
    same key leaves in place.  In the neutral pose the two sit 0.087 units apart (the
    closed lip line), so the vertical distance between them IS the mouth opening.
    """
    basis, m = world_basis(obj)
    xs = [c.x for c in basis]
    xc, width = 0.5 * (min(xs) + max(xs)), max(xs) - min(xs)
    slab = [i for i in range(len(basis)) if abs(basis[i].x - xc) < 0.035 * width]
    kb = table[opener]
    dz = {i: (m @ kb.data[i].co).z - basis[i].z for i in slab}
    lo = min(slab, key=lambda i: dz[i])
    ref = abs(dz[lo])
    above = [i for i in slab if basis[i].z > basis[lo].z + 1e-4 and dz[i] > -0.05 * ref]
    up = min(above, key=lambda i: (basis[i] - basis[lo]).length)
    return up, lo


def lip_gap(obj, table, pair, weights):
    """Vertical gap (world units) between the two inner-lip vertices for a weight set."""
    basis, m = world_basis(obj)
    up, lo = pair
    a, b = basis[up].copy(), basis[lo].copy()
    for name, val in weights.items():
        kb = table.get(name)
        if kb is None or val == 0.0:
            continue
        a += ((m @ kb.data[up].co) - basis[up]) * val
        b += ((m @ kb.data[lo].co) - basis[lo]) * val
    return a.z - b.z


def cheek_width(obj, table, pair, weights):
    """Face width across the two cheek vertices the weight set pushes furthest out.

    Only vertices off the midline and around mouth height are considered: a plain bbox
    width is useless (the widest points of the head are the ears, which Cheek_Puff does
    not touch) and so are the mouth corners, which Cheek_Puff pulls *inwards*.
    """
    basis, m = world_basis(obj)
    pos = [c.copy() for c in basis]
    for name, val in weights.items():
        kb = table.get(name)
        if kb is None or val == 0.0:
            continue
        for i in range(len(basis)):
            pos[i] += ((m @ kb.data[i].co) - basis[i]) * val
    xs = [c.x for c in basis]
    zs = [c.z for c in basis]
    xc, width, height = 0.5 * (min(xs) + max(xs)), max(xs) - min(xs), max(zs) - min(zs)
    zm = 0.5 * (basis[pair[0]].z + basis[pair[1]].z)
    cheek = [i for i in range(len(basis))
             if abs(basis[i].x - xc) > 0.2 * width and abs(basis[i].z - zm) < 0.12 * height]
    side = {s: [i for i in cheek if (basis[i].x - xc) * s > 0.0] for s in (-1.0, 1.0)}
    ends = [max(v, key=lambda i: (pos[i].x - basis[i].x) * s) for s, v in side.items()]
    return (abs(basis[ends[0]].x - basis[ends[1]].x),
            abs(pos[ends[0]].x - pos[ends[1]].x))


def face_forward(obj):
    """Unit "out of the face" direction (world XY): head centre -> eye centre.

    Same trick ``check_render`` uses to aim the camera -- the eye meshes sit toward the
    face, so the vector from the head's centroid to theirs points forward.
    """
    basis, _ = world_basis(obj)
    hc = sum(basis, mathutils.Vector()) / len(basis)
    ec, n = mathutils.Vector(), 0
    for name in ("7", "8", "9", "10"):
        eye = bpy.data.objects.get(name)
        if eye is None:
            continue
        m = eye.matrix_world
        cos = [m @ v.co for v in eye.data.vertices]
        ec += sum(cos, mathutils.Vector()) / len(cos)
        n += 1
    if not n:
        return mathutils.Vector((0.0, -1.0, 0.0))
    fwd = mathutils.Vector((ec.x / n - hc.x, ec.y / n - hc.y, 0.0))
    fwd.normalize()
    return fwd


def mouth_corners(obj, table):
    """(left, right) mouth-corner vertex indices = what Mouth_Stretch_L/_R move most."""
    basis, m = world_basis(obj)
    out = []
    for key in ("Mouth_Stretch_L", "Mouth_Stretch_R"):
        kb = table[key]
        out.append(max(range(len(basis)),
                       key=lambda i: ((m @ kb.data[i].co) - basis[i]).length))
    return tuple(out)


def lip_center_verts(obj, table, pair, count=8):
    """The ``count`` frontmost midline vertices just above and just below the lip line.

    These are the lip-centre vertices whose forward displacement IS the protrusion of a
    pucker; upper and lower are kept apart because several CC4 keys move only one of them.
    """
    basis, _ = world_basis(obj)
    fwd = face_forward(obj)
    hc = sum(basis, mathutils.Vector()) / len(basis)
    xs = [c.x for c in basis]
    zs = [c.z for c in basis]
    xc, width, height = 0.5 * (min(xs) + max(xs)), max(xs) - min(xs), max(zs) - min(zs)
    zm = 0.5 * (basis[pair[0]].z + basis[pair[1]].z)
    mid = [i for i in range(len(basis)) if abs(basis[i].x - xc) < 0.03 * width]
    out = []
    for lo, hi in ((zm, zm + 0.03 * height), (zm - 0.03 * height, zm)):
        band = [i for i in mid if lo < basis[i].z < hi]
        band.sort(key=lambda i: -(basis[i] - hc).dot(fwd))
        out.append(band[:count])
    return tuple(out)


def posed(obj, table, weights, idx):
    """World-space positions of the vertices ``idx`` under a shape-key weight set."""
    basis, m = world_basis(obj)
    pos = {i: basis[i].copy() for i in idx}
    for name, val in weights.items():
        kb = table.get(name)
        if kb is None or val == 0.0:
            continue
        for i in idx:
            pos[i] += ((m @ kb.data[i].co) - basis[i]) * val
    return basis, pos


def lip_protrusion(obj, table, lips, weights):
    """(upper, lower) mean forward displacement of the lip-centre vertices, world units."""
    fwd = face_forward(obj)
    up, lo = lips
    basis, pos = posed(obj, table, weights, list(up) + list(lo))
    return tuple(sum((pos[i] - basis[i]).dot(fwd) for i in band) / len(band)
                 for band in (up, lo))


def mouth_width(obj, table, corners, weights):
    """(neutral, posed) distance between the two mouth-corner vertices."""
    basis, pos = posed(obj, table, weights, corners)
    a, b = corners
    return (basis[a] - basis[b]).length, (pos[a] - pos[b]).length


def chin_motion(obj, table, pair, weights):
    """Largest vertex displacement in the chin band below the mouth (distortion check)."""
    basis, _ = world_basis(obj)
    zs = [c.z for c in basis]
    height = max(zs) - min(zs)
    zm = 0.5 * (basis[pair[0]].z + basis[pair[1]].z)
    chin = [i for i in range(len(basis))
            if zm - 0.20 * height < basis[i].z < zm - 0.06 * height]
    basis, pos = posed(obj, table, weights, chin)
    return max((pos[i] - basis[i]).length for i in chin)


def report_keys(meshes):
    head = bpy.data.objects[HEAD_OBJ]
    table = meshes[head]
    log("--- %s (%s): the %d CC4 shape-key names, in glTF target order ---"
        % (HEAD_OBJ, head.data.materials[0].name if head.data.materials else "?",
           len(NAMES_152)))
    for i in range(0, len(NAMES_152), 4):
        log("   " + "  ".join("%3d %-26s" % (j, NAMES_152[j])
                              for j in range(i, min(i + 4, len(NAMES_152)))))
    names = [n for n in NAMES_152 if any(k in n.lower() for k in KEYWORDS)]
    log("   mouth/jaw/cheek keys: %d of %d" % (len(names), len(NAMES_152)))
    for obj, tbl in sorted(meshes.items(), key=lambda kv: kv[0].name):
        hits = [n for n in tbl if any(k in n.lower() for k in KEYWORDS)]
        log("   mesh %-4s (%-16s %5d morphs) matching: %d"
            % (obj.name,
               obj.data.materials[0].name if obj.data.materials else "?",
               len(tbl), len(hits)))

    # Which key actually PARTS the lips?  Measured on the inner-lip vertex pair.
    pair = inner_lip_pair(head, table)
    basis, _ = world_basis(head)
    closed = lip_gap(head, table, pair, {})
    log("   inner-lip pair: upper v%d z=%.3f / lower v%d z=%.3f, closed gap %.3f units"
        % (pair[0], basis[pair[0]].z, pair[1], basis[pair[1]].z, closed))
    cand = ["V_Open", "V_Lip_Open", "V_Wide", "V_Affricate", "V_Dental_Lip", "V_Tight_O",
            "Jaw_Open", "Jaw_Down", "Mouth_Drop_Lower", "Mouth_Drop_Upper",
            "Mouth_Down_Lower_L", "Mouth_Down_Lower_R", "Mouth_Up_Upper_L",
            "Mouth_Up_Upper_R", "Mouth_Shrug_Upper", "Mouth_Shrug_Lower",
            "Mouth_Stretch_L", "Mouth_Stretch_R", "Mouth_Close"]
    log("   lip gap at weight 1.0 (closed = %.3f):" % closed)
    for n in sorted(cand, key=lambda n: -lip_gap(head, table, pair, {n: 1.0})):
        log("      %-22s %6.3f" % (n, lip_gap(head, table, pair, {n: 1.0})))
    log("   OPEN_POSE gap %.3f units (%.1fx the closed lip line)"
        % (lip_gap(head, table, pair, OPEN_POSE),
           lip_gap(head, table, pair, OPEN_POSE) / closed))
    log("   PUFF_POSE gap %.3f units (lips stay shut)"
        % lip_gap(head, table, pair, PUFF_POSE))
    w0, w1 = cheek_width(head, table, pair, PUFF_POSE)
    log("   cheek width under PUFF_POSE: %.2f -> %.2f units (+%.0f%%)"
        % (w0, w1, 100.0 * (w1 / w0 - 1.0)))

    # Which keys actually push the lips forward into a tube?  Measured as the forward
    # displacement of the lip-centre vertices against the mouth-corner distance.
    corners = mouth_corners(head, table)
    lips = lip_center_verts(head, table, pair)
    fwd = face_forward(head)
    mw0, _ = mouth_width(head, table, corners, {})
    log("   face forward axis (%.3f, %.3f, %.3f); mouth corners v%d / v%d, %.3f units apart"
        % (fwd.x, fwd.y, fwd.z, corners[0], corners[1], mw0))
    log("   lip-centre vertices: upper %s / lower %s" % (list(lips[0]), list(lips[1])))
    pk_cand = [("V_Tight_O", 1.0), ("V_Tight", 1.0),
               ("Mouth_Pucker_Up_L+R", 1.0), ("Mouth_Pucker_Down_L+R", 1.0),
               ("Mouth_Funnel_Up_L+R", 1.0), ("Mouth_Funnel_Down_L+R", 1.0),
               ("Mouth_Push_Upper_L+R", 1.0), ("Mouth_Blow_L+R", 1.0),
               ("Mouth_Tighten_L+R", 1.0), ("Mouth_Contract", 1.0)]

    def spread(spec, val):
        if spec.endswith("_L+R"):
            return {spec[:-4] + "_L": val, spec[:-4] + "_R": val}
        return {spec: val}

    log("   pucker candidates (prot = forward lip-centre motion, width = corner distance"
        " %.3f, chin = worst chin vertex):" % mw0)
    log("      %-24s %8s %8s %8s %8s %8s"
        % ("key", "protUp", "protLo", "width", "dW%", "chin"))
    rows = []
    for spec, val in pk_cand:
        w = spread(spec, val)
        if not all(n in table for n in w):
            continue
        pu, pl = lip_protrusion(head, table, lips, w)
        _, w1 = mouth_width(head, table, corners, w)
        rows.append((spec, pu, pl, w1, 100.0 * (w1 / mw0 - 1.0),
                     chin_motion(head, table, pair, w)))
    for r in sorted(rows, key=lambda r: r[4]):
        log("      %-24s %8.3f %8.3f %8.3f %8.2f %8.3f" % r)
    pu, pl = lip_protrusion(head, table, lips, PUCKER_POSE)
    _, w1 = mouth_width(head, table, corners, PUCKER_POSE)
    log("   PUCKER_POSE: protrusion %.3f (upper) / %.3f (lower) units, mouth width"
        " %.3f -> %.3f (%.1f%%), lip gap %.3f, chin %.3f"
        % (pu, pl, mw0, w1, 100.0 * (w1 / mw0 - 1.0),
           lip_gap(head, table, pair, PUCKER_POSE),
           chin_motion(head, table, pair, PUCKER_POSE)))


# ---------------------------------------------------------------------------


def flap(t):
    """0 -> 1 -> 0 sine, ``FLAP_PERIOD_S`` per open/close cycle."""
    return 0.5 - 0.5 * math.cos(2.0 * math.pi * (t - SEG_START_S) / FLAP_PERIOD_S)


def smoothstep(x):
    x = min(1.0, max(0.0, x))
    return x * x * (3.0 - 2.0 * x)


def envelope(u, ramp, hold, release):
    """Ramp up / hold / release, ``u`` seconds into the cycle."""
    if u < ramp:
        return smoothstep(u / ramp)
    if u < ramp + hold:
        return 1.0
    if u < ramp + hold + release:
        return 1.0 - smoothstep((u - ramp - hold) / release)
    return 0.0


def puff(t):
    """Ramp up / hold / release, ``PUFF_PERIOD_S`` per puff."""
    return envelope((t - FLAP_END_S) % PUFF_PERIOD_S,
                    PUFF_RAMP_S, PUFF_HOLD_S, PUFF_RELEASE_S)


def pucker(t):
    """Ramp up / hold / release, ``PUCKER_PERIOD_S`` per pucker."""
    return envelope((t - PUFF_END_S) % PUCKER_PERIOD_S,
                    PUCKER_RAMP_S, PUCKER_HOLD_S, PUCKER_RELEASE_S)


def build_face_test(meshes, fps):
    """Append the 13-19 s facial test to the shape-key channels of the Emote action.

    The armature is left alone: the Emote's last bone keys extrapolate as a constant hold,
    which is exactly the "body stays where the Emote left it" we want. The shape keys stay
    on the same active "Emote" action (the NLA strip of the same name stays muted), so the
    new keys land in the very channels the glTF exporter already writes.
    """
    def f(sec):
        return sec * fps

    seg0, flap1 = f(SEG_START_S), f(FLAP_END_S)
    puff1, seg1 = f(PUFF_END_S), f(SEG_END_S)
    settle = f(SETTLE_S)

    # weights that make up the test, as functions of time: the mouth flaps between the
    # neutral face and OPEN_POSE, then the cheeks blow up towards PUFF_POSE, then the
    # lips purse forward towards PUCKER_POSE
    def channels(t):
        if t <= SEG_START_S:
            w = 0.0
        elif t <= FLAP_END_S:
            w = flap(t)
        else:
            w = 0.0
        p = puff(t) if FLAP_END_S < t <= PUFF_END_S else 0.0
        q = pucker(t) if PUFF_END_S < t <= SEG_END_S else 0.0
        vals = {name: 0.0 for name in RESET_NAMES}
        for name, peak in OPEN_POSE.items():
            vals[name] += peak * w
        for name, peak in PUFF_POSE.items():
            vals[name] += peak * p
        for name, peak in PUCKER_POSE.items():
            vals[name] += peak * q
        return vals

    driven = sorted(channels(0.0))

    # what the Emote leaves each driven channel at -- we ease that out before the test
    bpy.context.scene.frame_set(int(round(f(12.5))))
    held = {}
    for obj, table in meshes.items():
        for name in driven:
            if name in table:
                held[(obj.name, name)] = table[name].value
    used = {}

    frames = [settle, seg0] + [seg0 + i for i in range(1, int(seg1 - seg0) + 1)]
    for fr in frames:
        t = fr / fps
        vals = channels(t)
        for obj, table in meshes.items():
            for name, val in vals.items():
                kb = table.get(name)
                if kb is None:
                    continue
                # the non-pose channels only need the ease-out pair (held -> 0)
                if name not in POSE_NAMES and fr not in (settle, seg0):
                    continue
                if fr == settle:
                    val = held.get((obj.name, name), 0.0)
                kb.value = val
                kb.keyframe_insert("value", frame=fr)
                used.setdefault(name, set()).add(obj.name)

    # LINEAR interpolation on everything we just wrote
    n_new = 0
    for obj in meshes:
        ad = obj.data.shape_keys.animation_data
        if not ad or not ad.action:
            continue
        for layer in ad.action.layers:
            for strip in layer.strips:
                for bag in strip.channelbags:
                    for fc in bag.fcurves:
                        for kp in fc.keyframe_points:
                            if kp.co.x >= settle - 0.5:
                                kp.interpolation = "LINEAR"
                                n_new += 1
                        fc.update()

    log("--- facial test segment ---")
    log("   fps=%d  settle key @%g (%.2f s), window frames %g..%g (%.2f..%.2f s)"
        % (fps, settle, SETTLE_S, seg0, seg1, SEG_START_S, SEG_END_S))
    log("   %g..%g (%.1f..%.1f s): OPEN_POSE sine, %g s per open/close cycle (%d cycles)"
        % (seg0, flap1, SEG_START_S, FLAP_END_S, FLAP_PERIOD_S,
           int((FLAP_END_S - SEG_START_S) / FLAP_PERIOD_S)))
    log("   %g..%g (%.1f..%.1f s): PUFF_POSE ramp %.2f s / hold %.2f s / release %.2f s, "
        "%d puffs" % (flap1, puff1, FLAP_END_S, PUFF_END_S, PUFF_RAMP_S, PUFF_HOLD_S,
                      PUFF_RELEASE_S, int((PUFF_END_S - FLAP_END_S) / PUFF_PERIOD_S)))
    log("   %g..%g (%.1f..%.1f s): PUCKER_POSE ramp %.2f s / hold %.2f s / release %.2f s,"
        " %d puckers" % (puff1, seg1, PUFF_END_S, SEG_END_S, PUCKER_RAMP_S, PUCKER_HOLD_S,
                         PUCKER_RELEASE_S,
                         int((SEG_END_S - PUFF_END_S) / PUCKER_PERIOD_S)))
    for name in POSE_NAMES:
        peak = OPEN_POSE.get(name, PUFF_POSE.get(name, PUCKER_POSE.get(name, 0.0)))
        log("   drives %-22s peak %.2f on meshes %s"
            % (name, peak, sorted(used.get(name, ()))))
    log("   eased to 0 at %g: %s" % (seg0, [n for n in RESET_NAMES if n not in POSE_NAMES]))
    log("   %d keyframe points written / retimed to LINEAR" % n_new)
    return int(seg1)


# ---------------------------------------------------------------------------


def check_render(frames, meshes):
    """Workbench close-ups of the test segment, written next to the GLB.

    ``frames`` is a list of ``(frame, width)`` pairs, ``width`` being how much of the face
    (in fractions of the head height) the frame should span: ~0.25 puts the mouth alone in
    the picture, ~0.45 fits the cheeks in as well.
    """
    head = bpy.data.objects[HEAD_OBJ]
    eyes = [bpy.data.objects[n] for n in ("7", "8", "9", "10") if n in bpy.data.objects]
    pair = inner_lip_pair(head, meshes[head])

    def deformed(obj):
        """World-space vertices of ``obj`` as posed at the current frame."""
        dg = bpy.context.evaluated_depsgraph_get()
        ev = obj.evaluated_get(dg)
        me = ev.to_mesh()
        cos = [obj.matrix_world @ v.co for v in me.vertices]
        ev.to_mesh_clear()
        return cos

    def aim():
        """(mouth centre, front direction, head height) for the current frame.

        Everything is re-read per frame: the Emote still moves the head around, so a camera
        parked on the rest pose would drift off the mouth.
        """
        cos = deformed(head)
        height = max(c.z for c in cos) - min(c.z for c in cos)
        hc = sum(cos, mathutils.Vector()) / len(cos)
        # the eye meshes sit toward the face, so head->eyes gives us "front"
        ec = mathutils.Vector()
        for o in eyes:
            e = deformed(o)
            ec += sum(e, mathutils.Vector()) / len(e)
        ec /= len(eyes)
        fwd = mathutils.Vector((ec.x - hc.x, ec.y - hc.y, 0.0))
        if fwd.length < 1e-6:
            fwd = mathutils.Vector((0.0, -1.0, 0.0))
        fwd.normalize()
        # aim at the lip line itself, dropped a touch so the chin stays in frame
        mouth = (cos[pair[0]] + cos[pair[1]]) * 0.5
        mouth.z -= height * 0.05
        return mouth, fwd, height

    cam_data = bpy.data.cameras.new("check_cam")
    cam_data.lens = 85.0
    cam = bpy.data.objects.new("check_cam", cam_data)
    bpy.context.scene.collection.objects.link(cam)
    bpy.context.scene.camera = cam

    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.render.resolution_x = 960
    sc.render.resolution_y = 960
    sc.render.resolution_percentage = 100
    sc.render.image_settings.file_format = "PNG"
    sh = sc.display.shading
    sh.light = "STUDIO"
    sh.color_type = "SINGLE"
    sh.show_cavity = True

    out = []
    for fr, width in frames:
        sc.frame_set(fr)
        mouth, fwd, height = aim()
        span = height * width                      # world units across the frame
        dist = span * cam_data.lens / cam_data.sensor_width
        cam.location = mouth + fwd * dist
        cam.rotation_euler = (mouth - cam.location).to_track_quat("-Z", "Y").to_euler()
        path = os.path.join(ROOT, "anim_check_%d.png" % fr)
        sc.render.filepath = path
        bpy.ops.render.render(write_still=True)
        out.append(path)
        log("   rendered %s (%.3f s, %.1f units across)" % (path, fr / sc.render.fps, span))
    return out


# ---------------------------------------------------------------------------


def main():
    do_check = "--check" in sys.argv
    bpy.ops.wm.read_homefile(use_empty=True)
    if not os.path.isfile(SRC):
        sys.exit("source not found: %s" % SRC)

    log("importing", SRC)
    bpy.ops.import_scene.gltf(filepath=SRC, import_shading="NORMALS")

    log("--- scene inventory ---")
    for obj in sorted(bpy.data.objects, key=lambda o: o.name):
        parent = obj.parent.name if obj.parent else "-"
        if obj.type == "MESH":
            sk = obj.data.shape_keys
            nmorph = (len(sk.key_blocks) - 1) if sk else 0
            mats = [(m.name, m.blend_method if hasattr(m, "blend_method") else "?")
                    for m in obj.data.materials if m]
            vg = len(obj.vertex_groups)
            mods = [m.type for m in obj.modifiers]
            log("MESH  %-34s verts=%-6d morphs=%-4d groups=%-4d parent=%-16s mods=%s mats=%s"
                % (obj.name, len(obj.data.vertices), nmorph, vg, parent, mods, mats))
        elif obj.type == "ARMATURE":
            log("ARMA  %-34s bones=%-4d parent=%s" % (obj.name, len(obj.data.bones), parent))
            names = [b.name for b in obj.data.bones]
            log("      face bones: %s" % [n for n in names if any(
                k in n.lower() for k in ("head", "jaw", "neck", "eye", "tongue", "teeth"))])
        else:
            log("%-5s %-34s parent=%s" % (obj.type, obj.name, parent))
    log("actions: %s" % [a.name for a in bpy.data.actions])
    log("images:  %s" % [(i.name, tuple(i.size)) for i in bpy.data.images])

    sc = bpy.context.scene
    fps = int(round(sc.render.fps / sc.render.fps_base))
    emote = bpy.data.actions.get("Emote")
    log("fps=%d  Emote frame range %s (%.3f s)"
        % (fps, tuple(emote.frame_range), emote.frame_range[1] / fps))

    meshes = morph_meshes()
    report_keys(meshes)
    end = build_face_test(meshes, fps)

    sc.frame_start = 0
    sc.frame_end = end
    log("scene frame range 0..%d (%.3f s); Emote action now %s"
        % (end, end / fps, tuple(emote.frame_range)))

    os.makedirs(os.path.dirname(DST), exist_ok=True)
    log("exporting", DST)
    bpy.ops.export_scene.gltf(
        filepath=DST,
        export_format="GLB",
        export_animations=True,
        export_skins=True,
        export_morph=True,
        export_morph_normal=False,
        export_apply=False,
        export_yup=True,
        export_image_format="AUTO",
        export_materials="EXPORT",
        export_texcoords=True,
        export_normals=True,
        export_optimize_animation_size=False,
        export_frame_range=False,
    )
    log("done, %d bytes" % os.path.getsize(DST))

    if do_check:
        log("--- check renders ---")
        # (seconds, frame width as a fraction of the head height): the mouth alone for the
        # flap peak / trough and the pucker peak / release, the whole lower face for the
        # puff baseline / peak
        check_render([(int(round(t * fps)), w) for t, w in
                      ((13.25, 0.25), (13.50, 0.25), (15.00, 0.45), (15.50, 0.45),
                       (17.50, 0.25), (18.00, 0.25))],
                     meshes)


if __name__ == "__main__":
    main()
