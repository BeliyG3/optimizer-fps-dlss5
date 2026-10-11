# Builds lab_scene_garden.blend and .glb: the outdoor lab (build_lab_outdoor.py) with a garden in its yard, for
# bench12's foliage, wind and HDRI tests:
#
#   "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python-exit-code 1 ^
#       tools\bench\assets\lab_scene_outdoor.blend --python tools\bench\blender\build_lab_garden.py -- --out tools\bench\assets
#
# lab_scene_outdoor.blend is opened and saved under the new name before anything is exported; it and its .glb are
# never written. Added, in a collection `Garden`, under an empty `zz_garden` (the node order, see ROOT):
#   - garden_path (garden_path.py), garden_soil (garden_soil.py): a 2.5 m path along the combo camera's route and a
#     soil bed under all the grass with a ragged edge onto the concrete (static, baked), textured from Poly Haven's
#     grass_ground and grass_path_2 (garden_ground.py: garden_ground_*.jpg and garden_path_*.jpg are written next to
#     the scene and packed);
#   - veg_grass_NN (garden_grass.py): 2000-7000 card-grass clumps of 4-5 quads (at most GRASS_TRIANGLES triangles); the
#     atlas garden_grass_atlas.png (garden_grass_atlas.py) is rendered first;
#   - veg_tree_N, veg_fern_NN (garden_plants.py): 4 island_tree_02 and ~15 fern_02 from tools/bench/assets/polyhaven/.
#   Positions: garden_layout.py. bench12's wind (wind.cpp) moves the veg_ nodes; the bake skips them.
#   - prop_* (garden_props.py, garden v2.2/v2.3): benches, tables, rocks, a boulder, a dead trunk, branches, pots,
#     barrels and yard tools from tools/bench/assets/polyhaven/ (textures at most 1024, written to garden_props/ and
#     packed): 31 placed by hand (garden_props_layout.py) and about 120 scattered in clusters along the path, at the
#     entrance, by the walls and under the trees (garden_props_scatter.py), all checked against the path, the combo
#     eye, the buildings, trunks, ferns and each other; each stands on the surface under its feet
#     (garden_props_ground.py, a build check); the bench kit is assembled and every template's parts checked
#     (garden_props_assembly.py). One mesh per template, shared by its props (glTF: one mesh, a node per
#     prop). Lit dynamically like the foliage: not baked (no lightmap UVs, hidden in the bakes), not moved by the wind;
#     the grass keeps 0.35 m away from them.
#   - people_* (garden_people.py, garden_people_layout.py, garden_people_seat.py, garden v2.4, v2.5b): 17 copies of the lab's
#     Olivia, Chad and X Bot (lab_characters.blend) under an empty `zz_people`: 5 walkers along the path and the yard,
#     3 talking pairs, 2 dancers, 4 sitting on benches (v2.5b: one pair talking); checked against the props, plants, walls, the
#     combo eye and each other; the props keep out of their way and the grass leaves their feet. Skinned, so animated
#     and never baked; glb_share_skins.py makes each copy's mesh nodes use the lab character's glTF mesh.
#   The grass stands on the surface under it (garden_grass._ground, v2.4), the props scatter into the distant yard too.
#   v2.5: the sitters loop a seated idle (garden_people_sit_motion.py): Mixamo sitting clips from --mixamo (default
#   tools/bench/assets/mixamo/out, README_CHARACTERS.md) when present, else a procedural idle (garden_people_idle.py).
# The .glb is export_bench_scene.py's export (characters appended, tinted base colours composed), checked against
# lab_scene_outdoor.glb: its nodes first, in the same order and unchanged, the garden's after them, the animations
# unchanged (the people's channels and skins come on top); the foliage's materials alphaMode MASK; the props' count, shared meshes, triangles and materials. Writes
# lab_scene_garden.layout.json (route, plants, props, counts).
# The bake: bake_lab_lightmaps.py on lab_scene_garden.blend with --hdri (tools/bench12/RASTER_BAKED.md).
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bpy
import numpy as np

import garden_grass as grass
import garden_ground as ground
import garden_layout as layout
import garden_path
import garden_people
import garden_people_mixamo_clip
import garden_people_layout as people_layout
import garden_plants as plants
import garden_props as props
import garden_props_ground as prop_ground
import garden_props_layout as props_layout
import garden_props_scatter as prop_scatter
import garden_soil as soil
import lab_outdoor_geometry as yard
from build_lab_outdoor import check_against_lab, combo_exit, combo_eye
from garden_grass_atlas import render_atlas
from character_export import append_characters, validate_glb
from lab_lightmap_check import read_glb
from scene_export import compose_tinted_base_colors, export_glb

NAME = "lab_scene_garden"
COLLECTION = "Garden"
# The glTF exporter writes root objects sorted by name (natural, case-insensitive), so the garden's objects hang under
# an empty whose name sorts after every lab and yard node: their nodes then come after lab_scene_outdoor.glb's.
ROOT = "zz_garden"
POLYHAVEN = Path("polyhaven")
CLUMPS = (2000, 7000)
GRASS_TRIANGLES = 60000
PROP_COUNT, PROP_TRIANGLES = (230, 290), 2800000  # v2.4: flattened by bench12 until it instances the shared meshes


def obstacles():
    """(footprints of the yard's buildings, crates and poles; footprints of the walls: buildings and the hall's east
    wall beside the opening)."""
    objects = [o for o in bpy.data.collections[yard.COLLECTION].objects if o.name not in ("yard_ground", "yard_fence")]
    boxes = layout.footprints(objects)
    buildings = layout.footprints([o for o in objects if o.name.startswith("yard_building")])
    opening = json.loads((Path(bpy.data.filepath).with_suffix('.opening.json')).read_text())['opening']['y']
    hall = np.array([(yard.WALL_INNER, yard.WALL_Y[0], yard.WALL_OUTER, opening[0]),
                     (yard.WALL_INNER, opening[1], yard.WALL_OUTER, yard.WALL_Y[1])])
    return boxes, np.concatenate([buildings, hall])


def check_foliage(glb):
    """The veg_ nodes' materials: alphaMode MASK at 0.5 and double-sided for grass, leaves and fern; the trunk
    material as bench12's wind expects; triangles per node. Returns a summary."""
    doc, _ = read_glb(glb)
    summary = {}
    for node in doc['nodes']:
        name = node.get('name', '')
        if not name.startswith('veg_'):
            continue
        mesh = doc['meshes'][node['mesh']]
        for primitive in mesh['primitives']:
            material = doc['materials'][primitive['material']]
            mname = material.get('name')
            triangles = doc['accessors'][primitive['indices']]['count'] // 3
            entry = summary.setdefault(mname, dict(alphaMode=material.get('alphaMode', 'OPAQUE'),
                                                   alphaCutoff=material.get('alphaCutoff'),
                                                   doubleSided=material.get('doubleSided', False), triangles=0, nodes=0))
            entry['triangles'] += triangles
            entry['nodes'] += 1
            if 'TEXCOORD_1' in primitive['attributes']:
                raise RuntimeError(f"{name}: foliage with TEXCOORD_1")
    for mname, entry in summary.items():
        cut = mname.startswith('veg_grass') or mname.endswith('_leaves') or mname == 'fern_02'
        if cut and (entry['alphaMode'] != 'MASK' or entry['alphaCutoff'] not in (None, 0.5) or not entry['doubleSided']):
            raise RuntimeError(f"{mname}: expected alphaMode MASK, cutoff 0.5, double-sided; got {entry}")
    if 'island_tree_02' not in summary:
        raise RuntimeError("no trunk material island_tree_02 on the trees")
    print("[garden] foliage materials:", json.dumps(summary), flush=True)
    return summary


def check_props(glb, expected, templates):
    """The prop_ mesh nodes: exactly the `expected` garden props (the lab's own prop_* nodes are empties: none of them may
    carry a mesh, or the bake would leave it out) sharing `templates` meshes (one per template used), within the owner's
    limits, no TEXCOORD_1 (never baked), no emission (bench12 would make a lamp of it), the potted plant's leaves cut out
    (MASK). Returns a summary."""
    doc, _ = read_glb(glb)
    nodes, triangles, materials, meshes = 0, 0, {}, set()
    for node in doc['nodes']:
        if not node.get('name', '').startswith('prop_') or 'mesh' not in node:
            continue
        nodes += 1
        meshes.add(node['mesh'])
        for primitive in doc['meshes'][node['mesh']]['primitives']:
            material = doc['materials'][primitive['material']]
            triangles += doc['accessors'][primitive['indices']]['count'] // 3
            materials[material['name']] = material.get('alphaMode', 'OPAQUE')
            if 'TEXCOORD_1' in primitive['attributes']:
                raise RuntimeError(f"{node['name']}: a prop with TEXCOORD_1")
            if any(material.get('emissiveFactor', [0, 0, 0])):
                raise RuntimeError(f"{material['name']}: an emissive prop material")
    leaves = [name for name in materials if name.startswith('potted_plant_02_leaves')]
    if not leaves or any(materials[name] != 'MASK' for name in leaves):
        raise RuntimeError(f"the potted plant's leaves are not alphaMode MASK: {materials}")
    if nodes != expected or not PROP_COUNT[0] <= nodes <= PROP_COUNT[1] or triangles > PROP_TRIANGLES:
        raise RuntimeError(f"{nodes} prop mesh nodes (expected {expected}) of {triangles} triangles "
                           f"(limits {PROP_COUNT}, {PROP_TRIANGLES})")
    if len(meshes) != templates:
        raise RuntimeError(f"{len(meshes)} meshes for the props, expected {templates} shared ones (one per template)")
    unique = sum(doc['accessors'][p['indices']]['count'] // 3 for m in meshes for p in doc['meshes'][m]['primitives'])
    print(f"[garden] props in the glb: {nodes} nodes of {len(meshes)} shared meshes ({unique} unique triangles), "
          f"{triangles} triangles flattened, {len(materials)} materials", flush=True)
    return dict(nodes=nodes, meshes=len(meshes), unique_triangles=unique, triangles=triangles, materials=materials)


def check_people(glb, armatures, shared):
    """The people in the glb: one skinned armature each, their mesh nodes on the lab characters' meshes (glb_share_skins),
    never baked (no TEXCOORD_1), names free of "hero"/"girl" (bench12's follow camera looks for those). Returns a summary."""
    doc, _ = read_glb(glb)
    nodes = doc['nodes']
    mesh_nodes = [n for n in nodes if n.get('name', '').startswith('people_') and 'mesh' in n]
    lab = {n['mesh'] for n in nodes if 'mesh' in n and 'skin' in n and not n.get('name', '').startswith('people_')}
    meshes = {n['mesh'] for n in mesh_nodes}
    names = [n.get('name', '') for n in nodes if n.get('name', '').startswith('people_')]
    if not meshes <= lab or any('hero' in n or 'girl' in n for n in names) or len(armatures) != len(people_layout.PEOPLE):
        raise RuntimeError(f"people: meshes {sorted(meshes)} not all the lab characters' {sorted(lab)}, or a bad name")
    for n in mesh_nodes:
        if any('TEXCOORD_1' in p['attributes'] for p in doc['meshes'][n['mesh']]['primitives']):
            raise RuntimeError(f"{n['name']}: TEXCOORD_1 on a person")
    triangles = sum(doc['accessors'][p['indices']]['count'] // 3 for n in mesh_nodes for p in doc['meshes'][n['mesh']]['primitives'])
    result = dict(people=len(armatures), mesh_nodes=len(mesh_nodes), unique_meshes=len(meshes),
                  skins=len({n['skin'] for n in mesh_nodes}), triangles_drawn=triangles, share=shared)
    print("[garden] people in the glb:", result, flush=True)
    return result


def soil_test(polyline, bounds, boxes, walls, trees):
    """A function telling which points (n x 2) stand on the soil bed (garden_soil.field above 0.5)."""
    xs, ys, values = soil.field(polyline, bounds, boxes, walls, trees)

    def on_soil(points):
        i = np.clip(np.round((points[:, 0] - xs[0]) / soil.CELL).astype(int), 0, len(xs) - 1)
        j = np.clip(np.round((points[:, 1] - ys[0]) / soil.CELL).astype(int), 0, len(ys) - 1)
        return values[i, j] > 0.5
    return on_soil


def main():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    parser = argparse.ArgumentParser(prog="build_lab_garden.py")
    parser.add_argument('--out', type=Path, help="output folder (default: the folder of lab_scene_outdoor.blend)")
    parser.add_argument('--mixamo', type=Path, help="folder of the sitters' Mixamo clips (default: mixamo/out next to "
                        "lab_scene_outdoor.blend); a folder without them gives the procedural idle")
    args = parser.parse_args(argv)
    source = Path(bpy.data.filepath)
    if source.name != 'lab_scene_outdoor.blend':
        raise SystemExit("open tools/bench/assets/lab_scene_outdoor.blend (see the command above)")
    out = (args.out or source.parent).resolve()
    blend, glb = out / f"{NAME}.blend", out / f"{NAME}.glb"
    assets = source.parent

    _, exit_y = combo_exit((yard.WALL_INNER + yard.WALL_OUTER) / 2)
    collection = bpy.data.collections.new(COLLECTION)
    bpy.context.scene.collection.children.link(collection)
    soil_mat, path_mat = ground.materials(assets / POLYHAVEN, out)
    path, polyline, frames = garden_path.build(exit_y, collection, path_mat)
    boxes, walls = obstacles()
    bounds = garden_path.yard_bounds(exit_y)
    trees = layout.trees(polyline, boxes)
    strips = [soil.build(polyline, bounds, boxes, walls, trees, soil_mat, collection), path]
    ferns = layout.ferns(trees, np.random.default_rng(3))
    prop_templates = props.templates(assets / POLYHAVEN, out)
    eyes = np.array([combo_eye(frame)[:2] for frame in range(0, 1400)])
    placed = props_layout.resolve(polyline)
    standing = people_layout.resolve(polyline, placed, sitters=False)  # the props keep out of their way
    site = props_layout.Site({k: v[2] for k, v in prop_templates.items()}, polyline, bounds, boxes, walls, trees, ferns,
                             soil_test(polyline, bounds, boxes, walls, trees), eyes, people_layout.avoid(standing))
    prop_scatter.scatter(site, placed, polyline, trees)
    clearances = props_layout.check(props_layout.name_all(placed), site)
    people = people_layout.resolve(polyline, placed)
    people_clearances = people_layout.check(people, placed, site, eyes)
    template_triangles = {k: v[1] for k, v in prop_templates.items()}
    prop_nodes, prop_triangles = props.build(prop_templates, placed, collection)
    ground_report = prop_ground.settle(prop_nodes, placed, [bpy.data.objects[n] for n in prop_ground.SURFACES])
    prop_ground.check(ground_report, placed)
    low = np.concatenate([props.low_points(prop_nodes), people_layout.grass_points(people)])
    if not PROP_COUNT[0] <= len(prop_nodes) <= PROP_COUNT[1] or prop_triangles > PROP_TRIANGLES:
        raise RuntimeError(f"{len(prop_nodes)} props of {prop_triangles} triangles (limits {PROP_COUNT}, {PROP_TRIANGLES})")

    atlas = out / "garden_grass_atlas.png"
    render_atlas(atlas)
    points, edges = props_layout.clear_grass(*layout.grass_points(polyline, bounds, boxes, walls, trees), low)
    if not CLUMPS[0] <= len(points) <= CLUMPS[1]:
        raise RuntimeError(f"{len(points)} grass clumps, expected {CLUMPS[0]}-{CLUMPS[1]}")
    grass_nodes, quads, grass_ground = grass.build_nodes(points, edges, grass.grass_materials(atlas), collection,
                                                         prop_ground.surface([bpy.data.objects[n] for n in prop_ground.SURFACES], np.nan))
    if 2 * quads > GRASS_TRIANGLES:
        raise RuntimeError(f"{2 * quads} grass triangles, at most {GRASS_TRIANGLES}")
    plant_nodes, tree_triangles, fern_triangles = plants.build(
        assets / POLYHAVEN / "island_tree_02" / "island_tree_02_2k.gltf",
        assets / POLYHAVEN / "fern_02" / "fern_02_2k.gltf", trees, ferns, collection)
    root = bpy.data.objects.new(ROOT, None)
    collection.objects.link(root)
    for obj in strips + grass_nodes + plant_nodes + prop_nodes:
        obj.parent = root  # the root stays at the origin, so the world matrices stay identity
    added = [ROOT] + [o.name for o in strips + grass_nodes + plant_nodes + prop_nodes]
    people_objects, people_built = garden_people.build(people, collection, [bpy.data.objects[n] for n in prop_ground.SURFACES],
                                                       {o.name: o for o in prop_nodes},
                                                       args.mixamo or garden_people_mixamo_clip.default_folder())
    added += garden_people.node_names(people_objects)
    armatures = [o.name for o in people_objects if o.type == 'ARMATURE']

    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    print("[garden] saved", blend, flush=True)
    rigs = append_characters()
    compose_tinted_base_colors()
    shared = export_glb(str(glb), rigs)
    validate_glb(str(glb), rigs + armatures)
    check = check_against_lab(glb, source.with_suffix('.glb'), added, expected_changed=(), tag="garden",
                              extra_people=armatures)
    foliage = check_foliage(glb)
    prop_check = check_props(glb, len(prop_nodes), len({p['template'] for p in placed}))
    people_check = check_people(glb, armatures, shared)
    report = dict(route=dict(frames=[frames[0], frames[-1]], points=polyline.round(3).tolist()),
                  trees=[dict(x=x, y=y, turn=t, scale=plants.TREE_BASE_SCALE * s) for x, y, t, s in trees],
                  ferns=[dict(x=x, y=y, turn=t, scale=s, variant=v) for x, y, t, s, v in ferns],
                  grass=dict(clumps=len(points), nodes=len(grass_nodes), quads=quads, triangles=2 * quads, ground=grass_ground),
                  tree_triangles=tree_triangles, fern_triangles=fern_triangles, nodes=added, check=check,
                  foliage=foliage,
                  props=dict(placed=[{**p, **c, 'ground': g} for p, c, g in zip(placed, clearances, ground_report)],
                             triangles=prop_triangles,
                             template_triangles=template_triangles, glb=prop_check),
                  people=dict(placed=[{**{k: v for k, v in p.items() if k != 'line'}, **c, **b,
                                       **({'line': p['line'][::10].round(3).tolist()} if 'line' in p else {})}
                                      for p, c, b in zip(people, people_clearances, people_built)], glb=people_check))
    (out / f"{NAME}.layout.json").write_text(json.dumps(report, indent=1) + "\n")
    print(f"[garden] written {glb} ({glb.stat().st_size / 1e6:.1f} MB) and {NAME}.layout.json", flush=True)


if __name__ == "__main__":
    main()
