"""CuteGirl facial test segment: jaw flap, cheek puffs and lip pucker keyed onto the Emote action."""

import math

import bpy

from cutegirl_poses import (FLAP_END_S, FLAP_PERIOD_S, OPEN_POSE, POSE_NAMES, PUCKER_HOLD_S, PUCKER_PERIOD_S,
                            PUCKER_POSE, PUCKER_RAMP_S, PUCKER_RELEASE_S, PUFF_END_S, PUFF_HOLD_S,
                            PUFF_PERIOD_S, PUFF_POSE, PUFF_RAMP_S, PUFF_RELEASE_S, RESET_NAMES, SEG_END_S,
                            SEG_START_S, SETTLE_S)
from export_log import make_log

log = make_log("[cutegirl]")


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
