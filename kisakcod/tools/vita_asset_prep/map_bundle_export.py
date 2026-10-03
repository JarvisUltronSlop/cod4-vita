#!/usr/bin/env python3
"""Build the complete base-game MP map package used by the Vita runtime.

The Windows oracle produces one pointer-free closure per map.  This writer
keeps the common menu/audio/script services once, then puts each map's world,
collision, lightgrid, entities, models, materials, images, and HUD bindings
under a stable numeric asset id.  Vita therefore reads one selected map at a
time while the package still contains the authored closure for every stock MP
map, including Shipment.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kvap  # noqa: E402
import shipment_typed_export as typed  # noqa: E402
import vita_effects  # noqa: E402
import vita_environment  # noqa: E402


from map_catalog import ROWS
MAPS = tuple((name, title, key) for _, name, title, key, _ in ROWS)

# Keep the package residency policy aligned with VitaMapIsEnabled(). These
# rows remain visible in the retail map menu, but their large map closures are
# intentionally deferred until streaming is available. Omitting their binary
# chunks keeps the all-map package below the Vita/Vita3K single-file limit
# while preserving the authored closures for later re-enablement.
DEFERRED_MAP_INDICES = frozenset({1, 3, 9, 13, 14})

FAMILY_IDS = {
    "world": 0x4D50000000000000,
    "collision": 0x4D43000000000000,
    "lightgrid": 0x4D47000000000000,
    "entities": 0x4D45000000000000,
    "models": 0x4D58000000000000,
    "modelLods": 0x4D58010000000000,
    "materials": 0x4D4C000000000000,
    "images": 0x4D49000000000000,
    "reflectionImages": 0x4D49010000000000,
    # Startup colors stay in the bounded images family.  These companion
    # streams carry the complete authored tangent/specular closures and are
    # loaded only after the startup payload has been staged and released.
    "normalImages": 0x4D49020000000000,
    "specularImages": 0x4D49030000000000,
    "hud": 0x4D48000000000000,
    "environment": 0x4D56000000000000,
    "primaryLights": 0x4D4B000000000000,
    # VFXA keeps source loadfx/createfx records separate from the compact
    # VENV atmosphere summary.  The high byte is unique within the map
    # family namespace and the low byte remains the catalog index.
    "effects": 0x4D46000000000000,
    # Exact color images referenced by a matched VFXA graph. This is kept
    # separate from the bounded BSP/model VITI so effect visuals cannot evict
    # authored world surfaces or be replaced with guessed aliases.
    "effectImages": 0x4D49070000000000,
    "colorImages": 0x4D49080000000000,
    # Placed XModel/viewmodel semantic records live in an optional sidecar
    # package. Keeping these families separate lets the base multi-map KVAP
    # remain below the Vita file-offset ceiling while the runtime streams one
    # map's authored model closure at a time.
    "modelNormalImages": 0x4D49040000000000,
    "modelSpecularImages": 0x4D49050000000000,
}

COMMON_FILES = (
    ("animation", "shipment.db.weapon-animations", "vita_animations.bin"),
    ("weapon", "shipment.db.weapon-definitions", "vita_weapons.bin"),
    ("effects", "shipment.db.impact-effects", "vita_impactfx.bin"),
    ("script", "shipment.db.rawfiles", "vita_rawfiles.bin"),
    ("strings", "shipment.db.localized-strings", "vita_localize.bin"),
    ("strings", "shipment.db.stringtables", "vita_stringtables.bin"),
    ("menu", "shipment.db.menus", "vita_menus.bin"),
    ("sound", "shipment.db.decoded-sounds", "vita_sounds.bin"),
    ("font", "shipment.db.font-glyphs", "vita_fonts.bin"),
)

MAP_FILES = (
    ("world", "world", "vita_world.bin"),
    ("collision", "collision", "vita_collision.bin"),
    ("lightgrid", "world", "vita_lightgrid.bin"),
    ("primaryLights", "primary_lights", "vita_primary_lights.bin"),
    ("entities", "world", "vita_map_ents.bin"),
    ("models", "model", "vita_models.bin"),
    ("materials", "material", "vita_materials.bin"),
    ("images", "image", "vita_images.bin"),
    ("hud", "hud", "vita_hud.bin"),
)

# VLOD was added after the original all-map oracle corpus. Preserve the
# ability to inspect those historical exports while automatically including
# authored lower-detail geometry in every newly generated map package.
OPTIONAL_MAP_FILES = (
    ("modelLods", "model", "vita_model_lods.bin"),
)

SEMANTIC_IMAGE_FILES = (
    ("normalImages", "normal", "vita_normal_images.bin"),
    ("specularImages", "specular", "vita_specular_images.bin"),
)
# Keep the first companion package within the Vita file-offset ceiling while
# closing the actual selected BSP world. Placed XModels/viewmodels receive a
# later streamed model-semantic phase; they are not expanded into every map's
# world companion here.
SEMANTIC_IMAGE_SCOPE = "world"
MODEL_SEMANTIC_IMAGE_FILES = (
    ("modelNormalImages", "normal", "vita_model_normal_images.bin"),
    ("modelSpecularImages", "specular", "vita_model_specular_images.bin"),
)


def _effect_image_aliases(effect_path: Path, material_path: Path) -> list[str]:
    """Resolve drawable VFXA visuals through their exact VITL color slots.

    Runner elements carry child effect paths rather than material names. Walk
    those source graph edges as well, so a future retail extraction includes
    the complete child FX image closure instead of silently stopping at the
    top-level weather/decal definition.
    """
    if not effect_path.is_file() or not material_path.is_file():
        return []
    try:
        graph = vita_effects._read_v2_graph(effect_path)
    except (OSError, ValueError):
        return []
    material_records = typed._material_records(material_path)
    materials = {
        record.get("name", ""): record
        for record in material_records
        if record.get("name")
    }
    materials_folded = {
        str(name).casefold(): record
        for name, record in materials.items()
    }
    definitions = list(graph.get("definitions", []))
    graphs = graph.get("graphs", {})
    by_name: dict[str, int] = {}
    by_path: dict[str, int] = {}
    by_name_folded: dict[str, int] = {}
    by_path_folded: dict[str, int] = {}
    for source_index, pair in enumerate(definitions):
        if len(pair) != 2:
            continue
        name, path = str(pair[0]), str(pair[1])
        if name:
            by_name.setdefault(name, source_index)
            by_name_folded.setdefault(name.casefold(), source_index)
        if path:
            by_path.setdefault(path, source_index)
            by_path_folded.setdefault(path.casefold(), source_index)

    def resolve_reference(value: object) -> int | None:
        reference = str(value or "").strip()
        if not reference:
            return None
        source_index = by_path.get(reference)
        if source_index is None:
            source_index = by_name.get(reference)
        if source_index is None:
            source_index = by_path_folded.get(reference.casefold())
        if source_index is None:
            source_index = by_name_folded.get(reference.casefold())
        return source_index

    aliases: list[str] = []
    seen: set[str] = set()
    visited: set[int] = set()

    def add_visual(visual: object) -> None:
        material = materials.get(str(visual))
        if material is None:
            material = materials_folded.get(str(visual).casefold())
        color = material.get("color", "") if material else ""
        if color and color not in seen:
            seen.add(color)
            aliases.append(color)

    def walk(source_index: int) -> None:
        if source_index in visited:
            return
        visited.add(source_index)
        effect_graph = graphs.get(source_index)
        if effect_graph is None:
            return
        for element in effect_graph.get("elements", []):
            elem_type = int(element.get("elemType", 0))
            if elem_type <= 4 or elem_type == 9:
                for visual in element.get("visuals", []):
                    add_visual(visual)
            if elem_type == 10:
                for visual in element.get("visuals", []):
                    child = resolve_reference(visual)
                    if child is not None:
                        walk(child)
            for key in ("effectOnImpact", "effectOnDeath", "effectEmitted"):
                child = resolve_reference(element.get(key, ""))
                if child is not None:
                    walk(child)

    for source_index in graphs:
        walk(int(source_index))
    return aliases


def _effect_model_names(effect_path: Path) -> set[str]:
    """Return exact XModel names referenced by VFXA model elements."""
    if not effect_path.is_file():
        return set()
    try:
        graph = vita_effects._read_v2_graph(effect_path)
    except (OSError, ValueError):
        return set()
    names: set[str] = set()
    for effect in graph.get("graphs", {}).values():
        for element in effect.get("elements", []):
            if int(element.get("elemType", 0)) != 5:
                continue
            for visual in element.get("visuals", []):
                name = str(visual or "").strip()
                if name:
                    names.add(name)
    return names
MODEL_SEMANTIC_IMAGE_SCOPE = "model"


def _map_from_csv(path: Path) -> str | None:
    try:
        rows = typed._read_rows(path)
    except (OSError, ValueError):
        return None
    for row in rows:
        if row["typeName"] == "col_map_mp" and row["name"].startswith("maps/mp/mp_"):
            return Path(row["name"]).stem.removeprefix("mp_").lower()
    return None


def _find_oracle_dirs(runtime_home: Path) -> dict[str, Path]:
    found: dict[str, Path] = {}
    candidates = sorted((p for p in runtime_home.iterdir()
                         if p.is_dir() and p.name.isdigit()),
                        key=lambda p: p.name)
    for candidate in candidates:
        csv_path = candidate / "main" / "vita_assets.csv"
        if not csv_path.is_file():
            continue
        map_name = _map_from_csv(csv_path)
        if map_name in {entry[0] for entry in MAPS}:
            # Timestamped oracle directories sort chronologically.  The
            # latest dump wins, which also selects the synthesized VITI file.
            found[map_name] = candidate / "main"
    missing = [name for name, _, _ in MAPS if name not in found]
    if missing:
        raise FileNotFoundError("missing oracle map dumps: " + ", ".join(missing))
    return found


def _write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Keep the launch-gate fields at the front of the manifest. The Vita
    # database intentionally reads only a bounded prefix before it decides
    # whether a package is runtime-ready.
    path.write_text(json.dumps(value, indent=2, sort_keys=False) + "\n", encoding="utf-8")


def _ensure_render_payloads(source: Path, map_name: str,
                            retail_root: Path | None,
                            reflection_path: Path | None = None,
                            color_tail_path: Path | None = None) -> dict[str, Any]:
    image_path = source / "vita_images.bin"
    expected = {f"loadscreen_mp_{map_name}", f"compass_map_mp_{map_name}"}
    # Oracle dumps may have already been split once: the ordinary VITI stays
    # beside the map dump while its generated reflection records live only in
    # the converted package directory. Preserve that validated source chunk
    # across a retail-IWI rebuild instead of replacing it with the empty
    # result of splitting a VITI that no longer carries the probes.
    previous_reflection: bytes | None = None
    if reflection_path is not None and reflection_path.is_file():
        previous_info = typed._validate_image_sidecar(reflection_path)
        if previous_info and previous_info["records"] > 0:
            previous_reflection = reflection_path.read_bytes()
    try:
        image_names = typed._image_names(image_path)
    except (OSError, ValueError, IndexError):
        image_names = set()
    # Rebuild from the read-only retail IWD when it is available. Existing
    # oracle VITI files may contain the frontend/world closure but predate the
    # selected map's entity-referenced model-material priority pass, leaving
    # large-map props with valid geometry and no authored color IWI.
    if retail_root is not None or not expected.issubset(image_names):
        effect_model_names = _effect_model_names(source / "vita_fxdefs.bin")
        typed._synthesize_images_from_iwd(
            source, retail_root, map_name, reflection_path,
            effect_model_names, color_tail_path)
        if previous_reflection is not None and reflection_path is not None:
            current_reflection = typed._validate_image_sidecar(reflection_path)
            if current_reflection is None or current_reflection["records"] == 0:
                reflection_path.write_bytes(previous_reflection)
    elif reflection_path is not None:
        typed._split_reflection_probe_sidecar(image_path, reflection_path)
    hud_path = typed._build_hud_sidecar(image_path, map_name)
    if hud_path is None:
        raise ValueError(f"{map_name}: synthesized VITI is missing required HUD images")
    image_info = typed._validate_image_sidecar(image_path)
    hud_info = typed._validate_hud_sidecar(hud_path)
    if image_info is None or hud_info is None:
        raise ValueError(f"{map_name}: image/HUD sidecar validation failed")
    reflection_info = (typed._validate_image_sidecar(reflection_path)
                       if reflection_path is not None else None)
    color_segments: list[dict[str, Any]] = []
    if color_tail_path is not None:
        segment_paths = []
        if color_tail_path.is_file():
            segment_paths.append(color_tail_path)
        segment_paths.extend(sorted(
            color_tail_path.parent.glob(f"{color_tail_path.stem}-*.bin"),
            key=lambda path: path.name.lower()))
        for segment_path in segment_paths:
            segment_info = typed._validate_image_sidecar(segment_path)
            if segment_info is None:
                raise ValueError(
                    f"{map_name}: color image segment validation failed: "
                    f"{segment_path}")
            color_segments.append({
                "path": str(segment_path),
                "bytes": segment_info["bytes"],
                "records": segment_info["records"],
                "dataBytes": segment_info["dataBytes"],
            })
    color_info = {
        "path": str(color_tail_path) if color_tail_path is not None else "",
        "bytes": sum(item["bytes"] for item in color_segments),
        "records": sum(item["records"] for item in color_segments),
        "dataBytes": sum(item["dataBytes"] for item in color_segments),
        "segments": color_segments,
        "source": "retail-iwd-material-closure",
    }
    return {
        "imageBytes": image_info["bytes"],
        "imageRecords": image_info["records"],
        "imageDataBytes": image_info["dataBytes"],
        "reflectionProbePath": str(reflection_path) if reflection_path else "",
        "reflectionProbeBytes": reflection_info["bytes"] if reflection_info else 0,
        "reflectionProbeRecords": reflection_info["records"] if reflection_info else 0,
        "reflectionProbeDataBytes": reflection_info["dataBytes"] if reflection_info else 0,
        "colorImages": color_info,
        "hudBytes": hud_info["bytes"],
        "hudRecords": hud_info["records"],
    }


def _validate_map_files(source: Path, map_name: str) -> dict[str, Any]:
    # VITH is regenerated after VITI so its compass binding names the current
    # map even when an oracle directory predates the per-map exporter pass.
    # Older Windows oracle directories predate VITP. Recover only their
    # authored VITW sun record until a fresh DB dump supplies all primary
    # lights; never synthesize local/spot/omni lights or colour constants.
    primary_path = source / "vita_primary_lights.bin"
    if not primary_path.is_file():
        typed._synthesize_primary_light_sidecar(
            source / "vita_world.bin", primary_path)
    required = [filename for _, _, filename in MAP_FILES
                if filename != "vita_hud.bin"]
    missing = [filename for filename in required if not (source / filename).is_file()]
    if missing:
        raise FileNotFoundError(f"{map_name}: missing sidecars: {', '.join(missing)}")
    # Reuse the strict per-sidecar validators from the single-map exporter.
    return {
        "world": typed._validate_geometry(source / "vita_world.bin", "world"),
        "collision": typed._validate_geometry(source / "vita_collision.bin", "collision"),
        "lightgrid": typed._validate_geometry(source / "vita_lightgrid.bin", "lightgrid"),
        "primaryLights": typed._validate_primary_light_sidecar(primary_path),
        "entities": typed._validate_entity_sidecar(source / "vita_map_ents.bin"),
        "models": typed._validate_model_sidecar(source / "vita_models.bin"),
        "modelLods": typed._validate_model_lod_sidecar(
            source / "vita_model_lods.bin"),
        "materials": typed._validate_material_sidecar(source / "vita_materials.bin"),
        "images": typed._validate_image_sidecar(source / "vita_images.bin"),
        "hud": typed._validate_hud_sidecar(source / "vita_hud.bin"),
    }


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_map_package(runtime_home: Path, retail_root: Path | None,
                      workspace: Path, output: Path) -> dict[str, Any]:
    runtime_home = runtime_home.resolve()
    workspace = workspace.resolve()
    output = output.resolve()
    output.relative_to(workspace)
    sources = _find_oracle_dirs(runtime_home)
    converted = workspace / "assets" / "converted" / "mp-map-export"
    converted.mkdir(parents=True, exist_ok=True)

    # Use the newest Shipment DB for the common services.  Those records are
    # shared across all MP maps, while the selected map's closure is isolated.
    common_source = sources["shipment"]
    common_info: list[dict[str, Any]] = []
    for kind, asset_id, filename in COMMON_FILES:
        source = common_source / filename
        if not source.is_file():
            raise FileNotFoundError(f"common sidecar is missing: {source}")
        common_info.append({"type": kind, "assetId": asset_id, "path": str(source),
                            "bytes": source.stat().st_size})

    map_info: list[dict[str, Any]] = []
    chunks: list[dict[str, Any]] = []
    for index, (name, display, localization_key) in enumerate(MAPS):
        source = sources[name]
        reflection_path = (converted / "reflection-probes"
                           / f"{index:02d}-{name}.bin")
        color_tail_path = (converted / "color-images"
                           / f"{index:02d}-{name}-tail.bin")
        render = _ensure_render_payloads(
            source, name, retail_root, reflection_path, color_tail_path)
        semantic_info: dict[str, dict[str, Any]] = {}
        for family, semantic, filename in SEMANTIC_IMAGE_FILES:
            semantic_path = (converted / "semantic-images"
                             / f"{index:02d}-{name}-{semantic}.bin")
            semantic_info[family] = typed._synthesize_semantic_images_from_iwd(
                source, retail_root, name, semantic, semantic_path,
                SEMANTIC_IMAGE_SCOPE)
        environment_path = converted / "environment" / f"{index:02d}-{name}.bin"
        environment = vita_environment.build_environment_sidecar(
            source / "vita_rawfiles.bin", environment_path, index, name)
        effects_path = converted / "effects" / f"{index:02d}-{name}.bin"
        # The rawfile parser remains the compatibility fallback. When the
        # desktop oracle has emitted its VFXA v2 graph, merge exact
        # FxElemDef/material/model references into this map's authored
        # placements; never infer an image from a path family.
        fx_graph_path = source / "vita_fxdefs.bin"
        effects = vita_effects.build_effect_sidecar(
            source / "vita_rawfiles.bin", effects_path, index, name,
            fx_graph_path if fx_graph_path.is_file() else None,
            source / "vita_impactfx.bin")
        effect_images_path = (converted / "effect-images"
                              / f"{index:02d}-{name}.bin")
        effect_images: dict[str, Any] = {
            "path": effect_images_path,
            "bytes": 0,
            "records": 0,
            "dataBytes": 0,
            "requested": 0,
            "missing": 0,
            "missingNames": [],
            "source": "retail-iwd-vfxa",
        }
        # Only a matched VFXA v2 graph can define this companion. The exact
        # visual material names are resolved through VITL before the retail
        # IWI extractor runs; no path-family or map-name image aliases enter
        # the package here.
        if (retail_root is not None
                and effects.get("graphDefinitions", 0) > 0):
            effect_aliases = _effect_image_aliases(
                effects_path, source / "vita_materials.bin")
            effect_images = typed._synthesize_effect_images_from_iwd(
                retail_root, effect_aliases, effect_images_path, name)
        validation = _validate_map_files(source, name)
        # Validate again after optional VITI regeneration and regenerate the
        # map's binding table with its own compass image name.
        validation["images"] = typed._validate_image_sidecar(source / "vita_images.bin")
        validation["hud"] = typed._validate_hud_sidecar(source / "vita_hud.bin")
        if validation["hud"] is None:
            raise ValueError(f"{name}: VITH validation failed")
        entry: dict[str, Any] = {
            "index": index,
            "name": name,
            "displayName": display,
            "localizationKey": localization_key,
            "enabled": index not in DEFERRED_MAP_INDICES,
            "source": str(source),
            "sourceSha256": _sha256(source / "vita_assets.csv"),
            "assetIds": {},
            "geometry": {},
            "render": render,
            "semanticImages": semantic_info,
            "environment": environment,
            "effects": effects,
            "effectImages": effect_images,
        }
        for family, chunk_type, filename in MAP_FILES:
            asset_id = FAMILY_IDS[family] | index
            source_path = source / filename
            entry["assetIds"][family] = f"0x{asset_id:016x}"
            entry["geometry"][family] = validation[family]
            if index not in DEFERRED_MAP_INDICES:
                chunks.append({"type": chunk_type, "assetId": asset_id,
                               "path": str(source_path)})
        for family, chunk_type, filename in OPTIONAL_MAP_FILES:
            info = validation[family]
            if info is None:
                continue
            asset_id = FAMILY_IDS[family] | index
            source_path = source / filename
            entry["assetIds"][family] = f"0x{asset_id:016x}"
            entry["geometry"][family] = info
            if index not in DEFERRED_MAP_INDICES:
                chunks.append({"type": chunk_type, "assetId": asset_id,
                               "path": str(source_path)})
        reflection_asset_id = FAMILY_IDS["reflectionImages"] | index
        entry["assetIds"]["reflectionImages"] = f"0x{reflection_asset_id:016x}"
        entry["geometry"]["reflectionImages"] = {
            "path": reflection_path,
            "bytes": render["reflectionProbeBytes"],
            "records": render["reflectionProbeRecords"],
            "dataBytes": render["reflectionProbeDataBytes"],
        }
        if (index not in DEFERRED_MAP_INDICES
                and render["reflectionProbeRecords"] > 0):
            chunks.append({"type": "image", "assetId": reflection_asset_id,
                           "path": str(reflection_path)})
        color_info = render.get("colorImages", {})
        color_segments = color_info.get("segments", [])
        color_asset_ids: list[str] = []
        for segment_index, segment in enumerate(color_segments):
            color_asset_id = (FAMILY_IDS["colorImages"]
                              | (segment_index << 8) | index)
            color_asset_ids.append(f"0x{color_asset_id:016x}")
            if index not in DEFERRED_MAP_INDICES:
                chunks.append({
                    "type": "image", "assetId": color_asset_id,
                    "path": str(segment["path"]),
                })
        entry["assetIds"]["colorImages"] = (
            color_asset_ids[0] if color_asset_ids else "")
        entry["assetIds"]["colorImagesSegments"] = color_asset_ids
        entry["geometry"]["colorImages"] = color_info
        for family, _semantic, _filename in SEMANTIC_IMAGE_FILES:
            info = semantic_info[family]
            segments = info.get("segments", [info])
            segment_asset_ids: list[str] = []
            for segment_index, segment in enumerate(segments):
                # The low byte remains the map index; the next byte selects a
                # streamed semantic segment for families larger than one VITI
                # parser envelope.
                semantic_asset_id = (FAMILY_IDS[family]
                                     | (segment_index << 8) | index)
                segment_asset_ids.append(f"0x{semantic_asset_id:016x}")
                if index not in DEFERRED_MAP_INDICES:
                    chunks.append({
                        "type": "image", "assetId": semantic_asset_id,
                        "path": str(segment["path"]),
                    })
            entry["assetIds"][family] = segment_asset_ids[0]
            entry["assetIds"][f"{family}Segments"] = segment_asset_ids
            entry["geometry"][family] = info
        environment_asset_id = FAMILY_IDS["environment"] | index
        entry["assetIds"]["environment"] = f"0x{environment_asset_id:016x}"
        entry["geometry"]["environment"] = environment
        if index not in DEFERRED_MAP_INDICES:
            chunks.append({"type": "environment", "assetId": environment_asset_id,
                           "path": str(environment_path)})
        effects_asset_id = FAMILY_IDS["effects"] | index
        entry["assetIds"]["effects"] = f"0x{effects_asset_id:016x}"
        entry["geometry"]["effects"] = effects
        if index not in DEFERRED_MAP_INDICES:
            chunks.append({"type": "effects", "assetId": effects_asset_id,
                           "path": str(effects_path)})
        effect_images_asset_id = FAMILY_IDS["effectImages"] | index
        entry["assetIds"]["effectImages"] = f"0x{effect_images_asset_id:016x}"
        entry["geometry"]["effectImages"] = effect_images
        if (index not in DEFERRED_MAP_INDICES
                and effect_images.get("records", 0) > 0):
            chunks.append({"type": "image", "assetId": effect_images_asset_id,
                           "path": str(effect_images_path)})
        map_info.append(entry)

    manifest = {
        "format": "KisakCOD Vita base-game MP map bundle",
        "version": 2,
        "status": "runtime-ready-v1",
        "playable": True,
        "payloadPolicy": "runtime-ready-v1",
        "recordSchema": "typed-db-map-bundle-v1",
        "mapCount": len(MAPS),
        "maps": [{"index": item["index"], "name": item["name"],
                  "displayName": item["displayName"],
                  "localizationKey": item["localizationKey"],
                  "enabled": item["enabled"],
                  "assetIds": item["assetIds"]} for item in map_info],
        "disabledMapIndices": sorted(DEFERRED_MAP_INDICES),
        "common": [{"type": item["type"], "assetId": item["assetId"],
                    "bytes": item["bytes"]} for item in common_info],
    }
    manifest_path = converted / "manifest.json"
    _write_json(manifest_path, manifest)
    strings_path = converted / "strings.json"
    _write_json(strings_path, {
        "format": "KisakCOD Vita map catalog",
        "version": 1,
        "maps": [{"name": name, "displayName": display,
                   "localizationKey": key} for name, display, key in MAPS],
    })

    package_chunks: list[dict[str, Any]] = [
        {"type": "manifest", "assetId": "shipment.db.manifest", "path": str(manifest_path)},
        {"type": "strings", "assetId": "shipment.db.strings", "path": str(strings_path)},
    ]
    package_chunks.extend({"type": item["type"], "assetId": item["assetId"],
                           "path": item["path"]} for item in common_info)
    package_chunks.extend(chunks)
    package_manifest_path = converted / "package-manifest.json"
    _write_json(package_manifest_path, {
        "sourceHash": hashlib.sha256(
            "".join(item["sourceSha256"] for item in map_info).encode("ascii")
        ).hexdigest(),
        "status": "runtime-ready-v1",
        "playable": True,
        "chunks": package_chunks,
    })
    report = kvap.create_package(package_manifest_path, output, workspace,
                                 workspace / "__cod4_root_not_used__")
    report.update({
        "status": "runtime-ready-v1",
        "playable": True,
        "mapCount": len(MAPS),
        "disabledMapIndices": sorted(DEFERRED_MAP_INDICES),
        "maps": map_info,
        "common": common_info,
        "manifest": str(manifest_path),
        "packageManifest": str(package_manifest_path),
    })
    (output.parent / "mp-map-bundle.report.json").write_text(
        json.dumps(report, indent=2, default=str) + "\n", encoding="utf-8")
    return report


def build_model_semantic_package(runtime_home: Path, retail_root: Path | None,
                                 workspace: Path, output: Path,
                                 source_dirs: dict[str, Path] | None = None) -> dict[str, Any]:
    """Build the optional streamed model normal/specular image package.

    The base map package intentionally carries the complete BSP semantic
    closure but not every placed XModel/viewmodel image. This companion keeps
    those authored IWI records in segmented VITI chunks, one map family at a
    time, so the Vita renderer can upload the exact source material slots
    without expanding the already-near-2 GiB base KVAP.
    """
    runtime_home = runtime_home.resolve()
    workspace = workspace.resolve()
    output = output.resolve()
    output.relative_to(workspace)
    if retail_root is not None:
        retail_root = retail_root.resolve()
    sources = ({name: path.resolve() for name, path in source_dirs.items()}
               if source_dirs is not None else _find_oracle_dirs(runtime_home))
    if not sources or any(name not in {entry[0] for entry in MAPS} for name in sources):
        raise ValueError("model semantic sources must name exported COD4 maps")
    for name, path in sources.items():
        if _map_from_csv(path / "vita_assets.csv") != name:
            raise ValueError(f"model semantic source map mismatch: {name}: {path}")
    converted = (output.parent / (output.stem + "-inputs") if source_dirs is not None
                 else workspace / "assets" / "converted" / "mp-map-model-semantics")
    converted.mkdir(parents=True, exist_ok=True)

    map_info: list[dict[str, Any]] = []
    chunks: list[dict[str, Any]] = []
    source_digest = hashlib.sha256()
    for index, (name, display, localization_key) in enumerate(MAPS):
        if name not in sources:
            continue
        source = sources[name]
        effect_model_names = _effect_model_names(source / "vita_fxdefs.bin")
        source_digest.update(_sha256(source / "vita_assets.csv").encode("ascii"))
        entry: dict[str, Any] = {
            "index": index,
            "name": name,
            "displayName": display,
            "localizationKey": localization_key,
            "enabled": index not in DEFERRED_MAP_INDICES,
            "assetIds": {},
            "semanticImages": {},
        }
        if index not in DEFERRED_MAP_INDICES:
            for family, semantic, _filename in MODEL_SEMANTIC_IMAGE_FILES:
                semantic_path = (converted / "semantic-images"
                                 / f"{index:02d}-{name}-{semantic}.bin")
                info = typed._synthesize_semantic_images_from_iwd(
                    source, retail_root, name, semantic, semantic_path,
                    MODEL_SEMANTIC_IMAGE_SCOPE, effect_model_names)
                entry["semanticImages"][family] = info
                segments = info.get("segments", [info])
                segment_asset_ids: list[str] = []
                for segment_index, segment in enumerate(segments):
                    asset_id = (FAMILY_IDS[family]
                                | (segment_index << 8) | index)
                    segment_asset_ids.append(f"0x{asset_id:016x}")
                    chunks.append({
                        "type": "image",
                        "assetId": asset_id,
                        "path": str(segment["path"]),
                    })
                if segment_asset_ids:
                    entry["assetIds"][family] = segment_asset_ids[0]
                    entry["assetIds"][f"{family}Segments"] = segment_asset_ids
        map_info.append(entry)

    manifest = {
        "format": "KisakCOD Vita optional MP model semantic package",
        "version": 1,
        "status": "optional-runtime-model-semantics-v1",
        "playable": False,
        "packageRole": "streamed-model-normal-specular-companions",
        "recordSchema": "typed-db-map-bundle-v1",
        "mapCount": len(map_info),
        "disabledMapIndices": sorted(DEFERRED_MAP_INDICES),
        "maps": [{
            "index": item["index"],
            "name": item["name"],
            "displayName": item["displayName"],
            "localizationKey": item["localizationKey"],
            "enabled": item["enabled"],
            "assetIds": item["assetIds"],
        } for item in map_info],
    }
    manifest_path = converted / "manifest.json"
    _write_json(manifest_path, manifest)
    package_chunks: list[dict[str, Any]] = [{
        "type": "manifest",
        "assetId": "shipment.db.model-semantics.manifest",
        "path": str(manifest_path),
    }]
    package_chunks.extend(chunks)
    package_manifest_path = converted / "package-manifest.json"
    _write_json(package_manifest_path, {
        "sourceHash": source_digest.hexdigest(),
        "status": "optional-runtime-model-semantics-v1",
        "playable": False,
        "packageRole": "streamed-model-normal-specular-companions",
        "chunks": package_chunks,
    })
    report = kvap.create_package(
        package_manifest_path, output, workspace,
        workspace / "__cod4_root_not_used__")
    report.update({
        "status": "optional-runtime-model-semantics-v1",
        "playable": False,
        "packageRole": "streamed-model-normal-specular-companions",
        "mapCount": len(map_info),
        "disabledMapIndices": sorted(DEFERRED_MAP_INDICES),
        "maps": map_info,
        "manifest": str(manifest_path),
        "packageManifest": str(package_manifest_path),
    })
    (output.parent / "mp-map-model-semantic-bundle.report.json").write_text(
        json.dumps(report, indent=2, default=str) + "\n", encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-home", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--retail-root", type=Path)
    parser.add_argument(
        "--model-semantic-output", type=Path,
        help="Also build the optional segmented XModel/viewmodel semantic package")
    args = parser.parse_args()
    try:
        report = build_map_package(args.runtime_home, args.retail_root,
                                   args.workspace, args.output)
        result = {"output": report["output"], "bytes": report["bytes"],
                  "chunks": len(report["chunks"]),
                  "mapCount": report["mapCount"]}
        if args.model_semantic_output:
            model_report = build_model_semantic_package(
                args.runtime_home, args.retail_root, args.workspace,
                args.model_semantic_output)
            result.update({
                "modelSemanticOutput": model_report["output"],
                "modelSemanticBytes": model_report["bytes"],
                "modelSemanticChunks": len(model_report["chunks"]),
            })
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, KeyError, kvap.KvapError) as exc:
        parser.error(str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
