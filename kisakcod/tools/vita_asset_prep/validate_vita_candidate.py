#!/usr/bin/env python3
"""Validate matching build/content evidence before writing an install bundle.

Development staging is explicit. A successful conversion or link audit never
substitutes for gameplay, image, lifecycle, or physical-device qualification.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from record_vita_build import sha256, validate_vpk


def read(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def checked_evidence(root: Path, entry: dict) -> Path:
    path = (root / entry["path"]).resolve()
    if not path.is_relative_to(root.resolve()) or sha256(path) != entry["sha256"]:
        raise ValueError("Qualification evidence path or hash mismatch")
    return path


def validate_hardware(report_path: Path, build_hash: str, content_hash: str,
                      map_names=("mp_shipment", "mp_crossfire")) -> None:
    report = read(report_path)
    if (report.get("schemaVersion") != 1 or report.get("buildManifestSha256") != build_hash
            or report.get("contentManifestSha256") != content_hash):
        raise ValueError("Qualification report does not match this build and content")
    root = report_path.parent
    for map_name in map_names:
        data = report["maps"][map_name]
        for cache in ("cold", "warm"):
            path = checked_evidence(root, data[cache])
            records = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
            header = records[0]
            if (header.get("format") != "CODVIT-PERF-2" or header.get("deviceType") != "physical-vita"
                    or header.get("stockClocks") is not True or header.get("resolution") != [960, 544]
                    or header.get("buildManifestSha256") != build_hash
                    or header.get("contentManifestSha256") != content_hash
                    or header.get("shaderCache") != cache):
                raise ValueError(f"{map_name}/{cache} lacks matching physical-device identity")
            complete = [record for record in records[1:] if record.get("completeRun") is True]
            if len(complete) != 1:
                raise ValueError(f"{map_name}/{cache} must contain one complete traversal")
            run = complete[0]
            if (run.get("timingValid") is not True or run.get("interrupted") is not False
                    or run.get("recordingOverflow") is not False or run["durationUs"] < 180000000
                    or run["warmupUs"] < 10000000 or run["p95Us"] > 66667
                    or run["maxUs"] > 250000 or run["gameplayStalls"] != 0
                    or run["consecutiveSlowWindows"] != 0):
                raise ValueError(f"{map_name}/{cache} traversal does not meet the performance floor")
            for key, limit in (("cpuPeakBytes", 114 << 20), ("totalPeakBytes", 178 << 20),
                               ("loadCpuPeakBytes", 114 << 20), ("loadTotalPeakBytes", 178 << 20)):
                value = run.get(key)
                if not isinstance(value, int) or isinstance(value, bool) or not 0 < value <= limit:
                    raise ValueError(f"{map_name}/{cache} has missing or excessive {key}")
        soak = read(checked_evidence(root, data["soak"]))
        if soak.get("durationSeconds", 0) < 1800 or soak.get("passed") is not True:
            raise ValueError(f"{map_name} requires a passing 30-minute soak")
    for category in ("gameplayParity", "timingParity", "visuals", "failureHandling", "lifecycle", "cleanInstall"):
        evidence = read(checked_evidence(root, report[category]))
        if (evidence.get("passed") is not True or evidence.get("buildManifestSha256") != build_hash
                or evidence.get("contentManifestSha256") != content_hash):
            raise ValueError(f"Missing or mismatched {category} qualification")
        if not evidence.get("artifacts"):
            raise ValueError(f"{category} must reference its actual traces, captures, or logs")
        for artifact in evidence["artifacts"]:
            checked_evidence(root, artifact)
        if category == "lifecycle" and (evidence.get("mapCycles", 0) < 10
                or evidence.get("suspendCycles", 0) < 10 or evidence.get("memoryDriftBytes", 1 << 30) > 1 << 20):
            raise ValueError("Lifecycle cycles or memory drift fail acceptance")


def validate_content(content: Path) -> dict:
    """Validate a complete set and all copied files before staging an app."""
    from map_catalog import map_index
    from vertical_slice_packs import read_source_index,read_raw_chunk,CPU_BUDGET,GPU_BUDGET,WORKING_BUDGET
    import kvap
    content=content.resolve()
    packs=read(content/'vertical-slice-manifest.json')
    if not {'common.kvap','mp_shipment.kvap'}<=packs['packages'].keys():
        raise ValueError('Missing common or Shipment regression pack')
    maps={};metadata={};headers={}
    for name,entry in packs['packages'].items():
        if Path(name).name!=name or '/' in name or '\\' in name or not name.endswith('.kvap'):
            raise ValueError('Invalid map package name')
        if name!='common.kvap':
            index=map_index(Path(name).stem)
            if not name.startswith('mp_'):raise ValueError('Map pack requires mp_ prefix')
            maps[index]=Path(name).stem[3:]
        path=content/name
        if path.stat().st_size!=entry['bytes'] or sha256(path)!=entry['sha256']:
            raise ValueError('Content hash/size mismatch: '+name)
        identity,chunks=read_source_index(path)
        manifests=[json.loads(read_raw_chunk(path,c)) for c in chunks if c.kind==kvap.CHUNK_TYPES['manifest']]
        role='common' if name=='common.kvap' else 'map'
        matches=[m for m in manifests if m.get('packageRole')==role]
        if len(matches)!=1:raise ValueError('Missing or ambiguous content manifest: '+name)
        data=matches[0];metadata[name]=data;headers[name]=kvap.inspect_package(path)
        if (data.get('schemaVersion') not in (1,2) or data.get('certified') is not True
                or data.get('payloadPolicy')!='runtime-ready-v1'
                or identity.hex()!=packs['sourceInstallHash']
                or data.get('sourceInstallHash')!=packs['sourceInstallHash']):
            raise ValueError('Incompatible content manifest: '+name)
        if data.get('schemaVersion')==2 and data.get('textureResidency')!='paged-world-v1':
            raise ValueError('Missing texture residency contract: '+name)
        if data.get('schemaVersion')!=packs.get('schemaVersion'):
            raise ValueError('Mixed content schema versions: '+name)
        if role=='map' and (data.get('mapIndex')!=index or data.get('mapId')!=maps[index]):
            raise ValueError('Map name/ID mismatch: '+name)
    if 'selectedMaps' in packs and packs['selectedMaps']!=[
            {'index':i,'name':n} for i,n in sorted(maps.items())]:
        raise ValueError('Selected-map catalog does not match the delivery files')
    common=metadata['common.kvap']
    for name,data in metadata.items():
        if name=='common.kvap':continue
        if (data.get('commonPackageId')!=headers['common.kvap']['packageId']
                or data.get('commonSha256')!=packs['packages']['common.kvap']['sha256']):
            raise ValueError('Map requires another common pack: '+name)
        totals=[]
        for field,limit in (('predictedCpuResidentBytes',CPU_BUDGET),('predictedGpuResidentBytes',GPU_BUDGET)):
            values=[common.get(field),data.get(field)]
            if any(type(v) is not int or v<0 for v in values) or sum(values)>limit:
                raise ValueError('Invalid or excessive '+field+': '+name)
            totals.append(sum(values))
        if sum(totals)>WORKING_BUDGET:raise ValueError('Working memory envelope exceeded: '+name)
    for name,expected in packs.get('media',{}).items():
        path=(content/'media'/name).resolve()
        if not path.is_relative_to(content/'media') or sha256(path)!=expected:
            raise ValueError('Content media mismatch: '+name)
    for name,entry in packs.get('streams',{}).items():
        path=(content/name).resolve()
        if (not name.startswith('stream/') or not path.is_relative_to(content/'stream')
                or path.stat().st_size!=entry['bytes'] or sha256(path)!=entry['sha256']):
            raise ValueError('Content stream mismatch: '+name)
    return packs


def validate(build: Path, content: Path, qualification: Path | None, development: bool) -> dict:
    manifest_path = build / "vita-build-manifest.json"
    manifest = read(manifest_path)
    artifacts = {"elf": "KisakCOD-vita-bootstrap", "self": "KisakCOD-vita-bootstrap.self",
                 "vpk": "KisakCOD-vita-bootstrap.vpk", "ownership": "vita-source-ownership.json",
                 "linkMap": "vita-source-link.map"}
    for name, filename in artifacts.items():
        if sha256(build / filename) != manifest["artifacts"][name]:
            raise ValueError(f"Build manifest is stale for {filename}")
    if validate_vpk(build / artifacts["vpk"], build / artifacts["self"]) != manifest["ebootSha256"]:
        raise ValueError("Embedded SELF does not match build manifest")
    ownership = read(build / artifacts["ownership"])
    if (ownership.get("binarySha256") != manifest["artifacts"]["elf"]
            or ownership.get("linkMapSha256") != manifest["artifacts"]["linkMap"]
            or manifest.get("nativeConfigurationValidated") is not True):
        raise ValueError("Build configuration or ownership evidence is inconsistent")
    content_path = content / "vertical-slice-manifest.json"
    packs = validate_content(content)
    qualified = False
    if qualification:
        validate_hardware(qualification, sha256(manifest_path), sha256(content_path),
                          tuple(Path(name).stem for name in packs['packages'] if name!='common.kvap'))
        qualified = True
    if not development:
        if packs.get("sourceSoundClosure") is not True:
            raise ValueError("Final staging requires authored source sound aliases and samples")
        if ownership.get("passed") is not True:
            raise ValueError("Full source implementation/provider ownership has not passed")
        if not {"menu_bg.mp4", "menu_music.at9"}.issubset(packs.get("media", {})):
            raise ValueError("Final staging requires prepared menu video and independent music")
        if not qualified:
            raise ValueError("Physical-Vita and gameplay qualification is missing; use --development for testing")
    return {"hardwareQualified": qualified, "sourceOwnershipPassed": ownership.get("passed") is True,
            "status": "qualified" if qualified and not development else "development-candidate"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--content", type=Path, required=True)
    parser.add_argument("--qualification", type=Path)
    parser.add_argument("--development", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(validate(args.build, args.content, args.qualification, args.development)))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Candidate validation failed: {error}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
