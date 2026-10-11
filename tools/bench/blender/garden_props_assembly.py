"""How the props' multi-part templates go together, and the build check that they do (garden v2.3).

modular_street_seating is a kit laid out exploded, not a bench: in the glTF the parts stand about 13.7 cm apart along
the bench and 10 cm apart in height (the seat ends short of both legs, the crossbar short of both, the arm rests 10 cm
over the legs' tops, the suspended support 10 cm under the seat). garden v2.2 joined them as authored, so its benches
came apart in the bench ("the slats float, a leg stands apart"). SEATING moves the parts (asset axes, metres, before any
mirroring) into an assembled section, the shared double leg staying at x = 0:
- the seat with its back 13.8 cm towards it (the seat ends 1 cm from the legs); the back supports as much and 5 cm in
  from either end (the kit has them 5 cm off the back's slats too): they hold the back, straddle the seat's ends and
  stand on the legs' tops;
- the crossbar and the suspended support as much, and 10 cm up: the support carries the seat, the crossbar runs from
  leg to leg;
- the single leg 27.4 cm (its inner face meets the crossbar's end) with its arm rest, which, like the double leg's,
  comes 10 cm down onto the leg's top.
The other assets are authored assembled.

check(): a template's parts (after the offsets, mirroring and decimation) form groups of parts whose boxes touch (within
TOUCH); each group must reach the template's base (its lowest point within TOUCH of the template's lowest), so no part
or group floats; two crates side by side are two groups, each on the ground.
"""
import numpy as np

TOUCH = 0.01
_BODY, _BAR, _END = (0.138, 0.0, 0.0), (0.138, 0.0, 0.1), (0.274, 0.0, 0.0)
SEATING = {'seat': _BODY, 'seat_back': _BODY, 'back_support_r': (0.188, 0.0, 0.0), 'back_support_l': (0.088, 0.0, 0.0),
           'crossbar': _BAR, 'suspended_support_01': _BAR, 'legs_single': _END,
           'arm_rest_01': (0.274, 0.0, -0.1), 'arm_rest_02': (0.0, 0.0, -0.1)}


def _box(mesh):
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    return co.min(axis=0), co.max(axis=0)


def check(name, parts):
    """parts: [(node name, mesh)] in template space. Raises on a group of parts off the base; returns the groups'
    part names for the build log."""
    boxes = [_box(mesh) for _, mesh in parts]
    group = list(range(len(boxes)))

    def root(i):
        while group[i] != i:
            group[i] = group[group[i]]
            i = group[i]
        return i
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            if np.maximum(boxes[i][0] - boxes[j][1], boxes[j][0] - boxes[i][1]).max() <= TOUCH:
                group[root(i)] = root(j)
    groups = {}
    for i in range(len(boxes)):
        groups.setdefault(root(i), []).append(i)
    base = min(lo[2] for lo, _ in boxes)
    floating = [[parts[i][0] for i in members] for members in groups.values()
                if min(boxes[i][0][2] for i in members) > base + TOUCH]
    if floating:
        raise RuntimeError(f"prop template {name}: parts off its base (touching no part that reaches it): {floating}")
    return [[parts[i][0] for i in members] for members in groups.values()]
