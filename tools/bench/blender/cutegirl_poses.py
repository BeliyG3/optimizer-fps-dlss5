"""CuteGirl facial test: the three poses, the 13-19 s segment timing and the channel name sets."""

from cutegirl_asset import NAMES_152

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
# The pucker (lips pushed forward into a kiss/whistle tube).
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
