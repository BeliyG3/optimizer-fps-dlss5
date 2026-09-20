"""World-space bounds and connected mesh components for lab staging."""
import bpy
import bmesh
from mathutils import Vector


def bounds(obj):
    meshes = [m for m in [obj, *obj.children_recursive] if m.type == 'MESH']
    points = [m.matrix_world @ Vector(p) for m in meshes for p in m.bound_box]
    return tuple(Vector([fn(p[i] for p in points) for i in range(3)]) for fn in (min, max))


def center(obj):
    a, b = bounds(obj)
    return (a+b)/2


def move_bottom(obj, x, y, z=0):
    a, b = bounds(obj)
    obj.location += Vector((x-(a.x+b.x)/2, y-(a.y+b.y)/2, z-a.z))
    bpy.context.view_layer.update()


def components(obj):
    mesh = obj.data
    adjacent = [[] for _ in mesh.vertices]
    for edge in mesh.edges:
        a, b = edge.vertices
        adjacent[a].append(b)
        adjacent[b].append(a)
    remaining = set(range(len(adjacent)))
    while remaining:
        seed = remaining.pop()
        ids, pending = [seed], [seed]
        while pending:
            for j in adjacent[pending.pop()]:
                if j in remaining:
                    remaining.remove(j)
                    ids.append(j)
                    pending.append(j)
        points = [obj.matrix_world @ mesh.vertices[i].co for i in ids]
        lo, hi = (Vector([fn(p[i] for p in points) for i in range(3)]) for fn in (min, max))
        yield ids, lo, hi


def delete_boxes(obj, gaps):
    removed = []
    ids = set()
    for group, lo, hi in components(obj):
        c = (lo+hi)/2
        if any(abs(c.x-x)<0.8 and abs(c.y-y-0.35)<0.2 for x,y in gaps):
            assert len(group) == 8, (obj.name, len(group))
            ids.update(group)
            removed.append(list(c))
    expected = 4 if obj.name == 'screen_stands' else 2
    assert len(removed) == expected, (obj.name, removed)
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.verts.ensure_lookup_table()
    bmesh.ops.delete(bm, geom=[bm.verts[i] for i in ids], context='VERTS')
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    return removed


def distance(xy, lo, hi):
    return sum(max(lo[i]-xy[i], 0, xy[i]-hi[i])**2 for i in range(2))**0.5


def overlaps(a, b, margin=0):
    return all(a[0][i] < b[1][i]+margin and b[0][i] < a[1][i]+margin for i in range(2))


def separate_asset(root, groups):
    """Split multi-instance library assets without changing geometry or world pose."""
    result = []
    for index, meshes in enumerate(groups):
        obj = bpy.data.objects.new(root.name+f'_item_{index+1}', None)
        root.users_collection[0].objects.link(obj)
        obj.matrix_world = root.matrix_world.copy()
        for mesh in meshes:
            world = mesh.matrix_world.copy()
            mesh.parent = obj
            mesh.matrix_world = world
        result.append(obj)
    bpy.data.objects.remove(root, do_unlink=True)
    bpy.context.view_layer.update()
    return result
