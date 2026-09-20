"""Downloads the CC0 Poly Haven assets of the lab bench scene into assets/external/polyhaven/.

    python fetch_polyhaven.py            # everything listed below that is not on disk yet
    python fetch_polyhaven.py --list     # what would be fetched, with sizes

Models come as glTF at 1k with their textures (each in its own folder, relative paths kept), surface
textures as 2k JPG (colour, GL normals, roughness), the HDRI as 2k .hdr. The folder is not tracked:
the assets are CC0 but large, and this script is the record of what the scene is built from.
"""
import json
import os
import sys
import urllib.request

API = "https://api.polyhaven.com/files/"
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "external", "polyhaven")

MODELS = [
    # light fixtures (made emissive by the scene script)
    "mounted_fluorescent_lights", "hanging_industrial_lamp",
    # lab furniture and clutter
    "metal_office_desk", "metal_toolbox", "metal_tool_chest", "tool_cart", "industrial_storage_cart",
    "worn_metal_rack", "drawer_cabinet", "plastic_crate_01", "plastic_crate_02", "old_military_crate",
    "industrial_microscope", "chemistry_set", "classic_laptop", "circuit_board", "SchoolChair_01",
    "mid_century_lounge_chair", "portable_generator", "modular_electric_cables", "WetFloorSign_01",
    "Drill_01", "small_plastic_torch", "sledgehammer_01", "garden_hose_wall_mounted_01", "service_pistol",
    # landmarks and shell
    "korean_fire_extinguisher_01", "korean_public_payphone_01", "rollershutter_door",
    "modular_industrial_pipes_01",
]
TEXTURES = ["smooth_concrete_floor", "concrete_wall_008", "corrugated_iron_02"]
TEXTURE_MAPS = ["Diffuse", "nor_gl", "Rough"]
HDRIS = ["empty_warehouse_01"]


def api(asset):
    request = urllib.request.Request(API + asset, headers={"User-Agent": "pw-bench-assets"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def plan():
    """(url, local path, size) for every file of every asset."""
    files = []
    for asset in MODELS:
        entry = api(asset)["gltf"]["1k"]["gltf"]
        folder = os.path.join(ROOT, "models", asset)
        files.append((entry["url"], os.path.join(folder, asset + ".gltf"), entry["size"]))
        for relative, included in entry.get("include", {}).items():
            files.append((included["url"], os.path.join(folder, *relative.split("/")), included["size"]))
    for asset in TEXTURES:
        maps = api(asset)
        for name in TEXTURE_MAPS:
            entry = maps[name]["2k"]["jpg"]
            files.append((entry["url"], os.path.join(ROOT, "textures", asset, os.path.basename(entry["url"])), entry["size"]))
    for asset in HDRIS:
        entry = api(asset)["hdri"]["2k"]["hdr"]
        files.append((entry["url"], os.path.join(ROOT, "hdris", os.path.basename(entry["url"])), entry["size"]))
    return files


def main():
    files = plan()
    total = sum(size for _, _, size in files)
    print(f"{len(files)} files, {total / 1e6:.1f} MB -> {ROOT}")
    if "--list" in sys.argv:
        for url, path, size in files:
            print(f"{size / 1e6:7.2f} MB  {os.path.relpath(path, ROOT)}")
        return
    fetched = 0
    for url, path, size in files:
        if os.path.exists(path) and os.path.getsize(path) == size:
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        request = urllib.request.Request(url, headers={"User-Agent": "pw-bench-assets"})
        with urllib.request.urlopen(request, timeout=120) as response, open(path + ".part", "wb") as out:
            out.write(response.read())
        os.replace(path + ".part", path)
        fetched += 1
    print(f"fetched {fetched}, already present {len(files) - fetched}")


if __name__ == "__main__":
    main()
