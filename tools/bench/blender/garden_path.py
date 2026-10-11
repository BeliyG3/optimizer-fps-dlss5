"""The garden's path (build_lab_garden.py): the combo camera's route through the yard and the static strip along it.

Blender axes, metres, as lab_outdoor_geometry.py. The route is bench12's combo eye (build_lab_outdoor.combo_eye) from
where it leaves the hall (frame ~883, the east wall's outer face) until it leaves the yard; the path is a 2.5 m strip
centred on it (the soil bed under the grass around it: garden_soil.py). `garden_path` is an ordinary static mesh
(baked), not `veg_`, 2 cm above the yard ground, cut to the yard by bisecting. Texture (garden_ground.py): the path
mesh is 3 m wide (its texture's ragged edge blends into the ground texture over the outer 0.25 m and a little inside)
and its UVs are (across + 1.5 m) / 3 m and the arc length along the route / 5 m.
"""
import bmesh
import bpy
import numpy as np
from mathutils import Vector

import lab_outdoor_geometry as yard
from build_lab_outdoor import combo_eye

PATH_WIDTH = 2.5
PATH_MESH_HALF_WIDTH = 1.5  # garden_ground.PATH_ACROSS / 2
PATH_ALONG = 5.0  # garden_ground.PATH_ALONG
FIRST_FRAME, LAST_FRAME, STEP = 880, 1200, 4
FENCE_INSET = 0.35  # the fence runs 0.3 m inside the yard's edges; strips and grass stay inside it


def yard_bounds(exit_y):
    """(x0, y0, x1, y1) of the yard inside the fence (the hall's wall bounds the west side)."""
    return (yard.WALL_OUTER, exit_y - yard.YARD_LENGTH / 2 + FENCE_INSET,
            yard.YARD_X0 + yard.YARD_DEPTH - FENCE_INSET, exit_y + yard.YARD_LENGTH / 2 - FENCE_INSET)


def route(exit_y):
    """The eye's (x, y) every STEP frames while it is in the yard, starting at the wall's outer face: (points, frames)."""
    x0, y0, x1, y1 = yard_bounds(exit_y)
    points, frames = [], []
    for frame in range(FIRST_FRAME, LAST_FRAME + 1, STEP):
        x, y, _ = combo_eye(frame)
        inside = x0 <= x <= x1 and y0 <= y <= y1
        if inside:
            if not points and frame > FIRST_FRAME:  # the crossing of the wall's outer face
                px, py, _ = combo_eye(frame - STEP)
                t = (x0 - px) / (x - px)
                points.append((x0, py + t * (y - py)))
                frames.append(frame - STEP + t * STEP)
            points.append((x, y))
            frames.append(float(frame))
        elif points:
            px, py = points[-1]
            t = min((b - p) / (c - p) for p, c, b in ((px, x, x0), (px, x, x1), (py, y, y0), (py, y, y1))
                    if (c - p) and 0.0 <= (b - p) / (c - p) <= 1.0)
            points.append((px + t * (x - px), py + t * (y - py)))
            frames.append(frames[-1] + t * STEP)
            break
    if len(points) < 4:
        raise RuntimeError(f"the combo route has only {len(points)} points in the yard")
    return np.array(points), frames


def distance_to_route(points, polyline):
    """Distance in the ground plane from each of points (n x 2) to the polyline (m x 2)."""
    a, b = polyline[:-1], polyline[1:]
    ab = b - a
    best = np.full(len(points), np.inf)
    for start in range(0, len(points), 4096):
        p = points[start:start + 4096, None, :]
        t = np.clip(((p - a) * ab).sum(-1) / (ab * ab).sum(-1), 0.0, 1.0)
        d = np.linalg.norm(p - (a + t[..., None] * ab), axis=-1).min(axis=1)
        best[start:start + 4096] = d
    return best


def _normals(polyline):
    tangent = np.gradient(polyline, axis=0)
    tangent /= np.linalg.norm(tangent, axis=1, keepdims=True)
    return np.stack([-tangent[:, 1], tangent[:, 0]], axis=1)


def strip_coordinates(points, line):
    """(across, along) of points (n x 2) on the polyline `line`: signed distance (positive on the right of the travel
    direction) from the nearest point and its arc length; the end segments are extended."""
    a, b = line[:-1], line[1:]
    ab = b - a
    lengths = np.linalg.norm(ab, axis=1)
    starts = np.concatenate([[0.0], np.cumsum(lengths)[:-1]])
    t = ((points[:, None, :] - a) * ab).sum(-1) / (lengths ** 2)
    low = np.zeros_like(t)
    high = np.ones_like(t)
    low[:, 0], high[:, -1] = -np.inf, np.inf
    t = np.clip(t, low, high)
    foot = a + t[..., None] * ab
    best = np.linalg.norm(points[:, None, :] - foot, axis=-1).argmin(axis=1)
    rows = np.arange(len(points))
    d = points - foot[rows, best]
    side = np.sign(ab[best, 0] * d[:, 1] - ab[best, 1] * d[:, 0])  # > 0 on the left
    return -side * np.linalg.norm(d, axis=1), starts[best] + t[rows, best] * lengths[best]


def strip(name, polyline, half_width, z, bounds, material, collection, uv_of, extend=0.0):
    """A strip of quads across the polyline (extended by `extend` at both ends), cut to bounds by bisecting. Its UVs
    are uv_of(vertex xy (n x 2), extended polyline) -> (n x 2)."""
    line = polyline.copy()
    if extend:
        line[0] -= extend * (line[1] - line[0]) / np.linalg.norm(line[1] - line[0])
        line[-1] += extend * (line[-1] - line[-2]) / np.linalg.norm(line[-1] - line[-2])
    normal = _normals(line)
    left, right = line + half_width * normal, line - half_width * normal
    verts = [(x, y, z) for pair in zip(left, right) for x, y in pair]
    faces = [(2 * i, 2 * i + 1, 2 * i + 3, 2 * i + 2) for i in range(len(line) - 1)]
    bm = bmesh.new()
    for v in verts:
        bm.verts.new(v)
    bm.verts.ensure_lookup_table()
    for f in faces:
        face = bm.faces.new([bm.verts[i] for i in f])
        face.normal_update()
        if face.normal.z < 0:
            face.normal_flip()
    x0, y0, x1, y1 = bounds
    for co, no in (((x0, 0, 0), (-1, 0, 0)), ((0, y0, 0), (0, -1, 0)), ((x1, 0, 0), (1, 0, 0)), ((0, y1, 0), (0, 1, 0))):
        geom = bm.verts[:] + bm.edges[:] + bm.faces[:]
        bmesh.ops.bisect_plane(bm, geom=geom, plane_co=Vector(co), plane_no=Vector(no), dist=1e-5, clear_outer=True)
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(material)
    uv = mesh.uv_layers.new(name="UVMap")
    xy = np.array([v.co[:2] for v in mesh.vertices])
    per_vertex = uv_of(xy, line)
    loops = np.array([loop.vertex_index for loop in mesh.loops])
    uv.data.foreach_set("uv", per_vertex[loops].astype(np.float32).ravel())
    mesh.validate()
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def _path_uvs(xy, line):
    across, along = strip_coordinates(xy, line)
    return np.column_stack([np.clip(across / (2 * PATH_MESH_HALF_WIDTH) + 0.5, 0, 1), along / PATH_ALONG])


def build(exit_y, collection, path_mat):
    """garden_path with the given material. Returns (the object, route points, route frames)."""
    polyline, frames = route(exit_y)
    bounds = yard_bounds(exit_y)
    path = strip("garden_path", polyline, PATH_MESH_HALF_WIDTH, 0.02, bounds, path_mat, collection, _path_uvs,
                 extend=1.0)
    length = float(np.linalg.norm(np.diff(polyline, axis=0), axis=1).sum())
    print(f"[garden] route: {len(polyline)} points, frames {frames[0]:.1f}..{frames[-1]:.1f}, {length:.1f} m, "
          f"from {polyline[0].round(2).tolist()} to {polyline[-1].round(2).tolist()}", flush=True)
    return path, polyline, frames
