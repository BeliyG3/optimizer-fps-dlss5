"""Facecap hole filling: caps every open boundary loop with new faces and keeps the shape keys intact."""

import bmesh
import numpy as np

from export_log import make_log

log = make_log("[facecap]")


def count_boundary_loops(edges):
    """Number of connected components in the boundary-edge graph."""
    remaining = set(edges)
    loops = 0
    while remaining:
        seed = remaining.pop()
        loops += 1
        stack = [seed]
        while stack:
            e = stack.pop()
            for v in e.verts:
                for ne in v.link_edges:
                    if ne in remaining:
                        remaining.discard(ne)
                        stack.append(ne)
    return loops


def snapshot_shape_keys(me):
    """Return [(name, value, slider_min, slider_max, relative_key, coords)]."""
    sk = me.shape_keys
    if not sk:
        return None
    out = []
    n = len(me.vertices)
    for kb in sk.key_blocks:
        arr = np.empty(n * 3, dtype=np.float32)
        kb.data.foreach_get("co", arr)
        out.append(
            {
                "name": kb.name,
                "value": kb.value,
                "min": kb.slider_min,
                "max": kb.slider_max,
                "relative_key": kb.relative_key.name if kb.relative_key else None,
                "co": arr,
            }
        )
    return out


def restore_shape_keys(obj, snap):
    """Rebuild shape keys on `obj` from a snapshot (only if Blender lost them)."""
    me = obj.data
    for entry in snap:
        kb = obj.shape_key_add(name=entry["name"], from_mix=False)
        kb.data.foreach_set("co", entry["co"])
    sk = me.shape_keys
    for entry in snap:
        kb = sk.key_blocks[entry["name"]]
        kb.value = entry["value"]
        kb.slider_min = entry["min"]
        kb.slider_max = entry["max"]
    for entry in snap:
        if entry["relative_key"]:
            sk.key_blocks[entry["name"]].relative_key = sk.key_blocks[
                entry["relative_key"]
            ]


def fill_holes(obj, cavity_mat):
    """Cap every open boundary loop of `obj`.  Returns (holes, faces_added)."""
    me = obj.data
    n_verts_before = len(me.vertices)
    n_polys_before = len(me.polygons)
    snap = snapshot_shape_keys(me)
    n_keys_before = len(snap) if snap else 0

    # material slot for the generated faces
    slot = -1
    for i, m in enumerate(me.materials):
        if m is cavity_mat:
            slot = i
            break
    if slot < 0:
        me.materials.append(cavity_mat)
        slot = len(me.materials) - 1

    bm = bmesh.new()
    bm.from_mesh(me)
    bm.verts.ensure_lookup_table()
    bm.edges.ensure_lookup_table()

    boundary = [e for e in bm.edges if e.is_boundary]
    if not boundary:
        bm.free()
        log("mesh %-20s no boundary edges, already closed" % obj.name)
        return 0, 0

    holes = count_boundary_loops(boundary)
    v_in_bm = len(bm.verts)

    new_faces = []
    try:
        res = bmesh.ops.holes_fill(bm, edges=boundary, sides=0)
        new_faces = [f for f in res.get("faces", []) if f.is_valid]
    except Exception as exc:  # pragma: no cover - fallback path
        log("holes_fill failed on %s (%s), trying edgenet_fill" % (obj.name, exc))
        res = bmesh.ops.edgenet_fill(bm, edges=boundary)
        new_faces = [f for f in res.get("faces", []) if f.is_valid]
    if not new_faces:
        log("holes_fill produced nothing on %s, trying triangle_fill" % obj.name)
        res = bmesh.ops.triangle_fill(bm, edges=boundary, use_beauty=True)
        new_faces = [f for f in res.get("geom", []) if isinstance(f, bmesh.types.BMFace)]

    if new_faces:
        tri = bmesh.ops.triangulate(bm, faces=new_faces)
        new_faces = [f for f in tri.get("faces", new_faces) if f.is_valid]
        for f in new_faces:
            f.material_index = slot
            f.smooth = False

    added = len(new_faces)
    v_in_bm_after = len(bm.verts)
    if v_in_bm_after != v_in_bm:
        log(
            "WARNING: %s gained %d verts during fill (shape keys will be padded)"
            % (obj.name, v_in_bm_after - v_in_bm)
        )

    bm.to_mesh(me)
    bm.free()
    me.update()

    n_verts_after = len(me.vertices)
    sk = me.shape_keys
    n_keys_after = len(sk.key_blocks) if sk else 0

    if snap and n_verts_after == n_verts_before:
        if n_keys_after != n_keys_before:
            log(
                "shape keys lost on %s (%d -> %d), restoring from snapshot"
                % (obj.name, n_keys_before, n_keys_after)
            )
            restore_shape_keys(obj, snap)
        else:
            # re-stamp the coordinates; to_mesh can shuffle them
            for entry in snap:
                kb = me.shape_keys.key_blocks.get(entry["name"])
                if kb is not None:
                    kb.data.foreach_set("co", entry["co"])
    elif snap:
        log(
            "WARNING: vertex count changed on %s (%d -> %d); shape keys NOT restamped"
            % (obj.name, n_verts_before, n_verts_after)
        )

    sk = me.shape_keys
    log(
        "mesh %-20s holes=%-3d faces_added=%-6d polys %d->%d verts %d->%d keys %d->%d"
        % (
            obj.name,
            holes,
            added,
            n_polys_before,
            len(me.polygons),
            n_verts_before,
            n_verts_after,
            n_keys_before,
            len(sk.key_blocks) if sk else 0,
        )
    )
    return holes, added
