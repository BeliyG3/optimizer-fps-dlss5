"""CuteGirl shape-key report: logs the CC4 names and the measured effect of the mouth / pucker keys."""

import bpy

from cutegirl_asset import HEAD_OBJ, NAMES_152
from cutegirl_measure import (chin_motion, cheek_width, face_forward, inner_lip_pair, lip_center_verts,
                              lip_gap, lip_protrusion, mouth_corners, mouth_width, world_basis)
from cutegirl_poses import KEYWORDS, OPEN_POSE, PUCKER_POSE, PUFF_POSE
from export_log import make_log

log = make_log("[cutegirl]")


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
