"""Derive a coherent half-dimension 3D texture set from verified local packs.

Authored mips are copied byte-for-byte. Single-level images are box filtered
offline using the recorded Pillow version; no runtime codecs/dependencies change.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct

import kvap
import vertical_slice_packs as packs
from validate_vita_candidate import validate_content

POLICY = 'half-current-3d-preserve-ui-v1'
DXT = {0x31545844: 'DXT1', 0x33545844: 'DXT3', 0x35545844: 'DXT5'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def ui_names(manifest):
    names = set(packs.COMMON_PRESENTATION_IMAGE_NAMES)
    for report in manifest.get('sourceUiDependencies', {}).values():
        names.update(report['images'])
    return names


def is_ui(name, names):
    return name in names or any(token in name.lower() for token in (
        'font', 'menu', 'hud', 'compass', 'reticle', 'loadscreen', 'loading',
        'scorebar', 'checkbox', 'objective_', 'scrollbar', 'cursor', 'button'))


def dds_header(w, h, fmt, size):
    return (b'DDS ' + struct.pack('<7I', 124, 0x81007, h, w, size, 0, 1)
            + bytes(44) + struct.pack('<8I', 32, 4, fmt, 0, 0, 0, 0, 0)
            + struct.pack('<5I', 0x1000, 0, 0, 0, 0))


def half_single(payload, w, h, fmt):
    from PIL import Image
    target = (w // 2, h // 2)
    if fmt in DXT:
        with Image.open(io.BytesIO(dds_header(w, h, fmt, len(payload)) + payload)) as image:
            image = image.convert('RGBA').resize(target, Image.Resampling.BOX)
            stream = io.BytesIO()
            image.save(stream, format='DDS', pixel_format=DXT[fmt])
            result = stream.getvalue()[128:]
    elif fmt in (21, 50):
        # Filter independent channels, including alpha, rather than treating
        # encoded lightmap coefficients as premultiplied transparency.
        image = (Image.frombytes('L', (w, h), payload) if fmt == 50 else
                 Image.frombytes('RGBA', (w, h), payload, 'raw', 'BGRA'))
        channels = [channel.resize(target, Image.Resampling.BOX) for channel in image.split()]
        image = Image.merge(image.mode, channels)
        result = image.tobytes() if fmt == 50 else image.tobytes('raw', 'BGRA')
    else:
        raise kvap.KvapError(f'unsupported single-level image format {fmt}')
    if len(result) != packs.mip_bytes(fmt, *target):
        raise kvap.KvapError('downsampled image byte count differs')
    return result


def half_record(name, wire, protected):
    nl, size, w, h, depth, levels, fmt, flags = struct.unpack_from('<8I', wire)
    if len(wire) != 32 + nl + size or wire[32:32+nl].decode() != name:
        raise kvap.KvapError('invalid image record')
    report = dict(name=name, before=[w, h, levels, size], inputSha256=sha(wire))
    method = 'preserved-ui' if is_ui(name, protected) else None
    # Constants and sub-block images have no useful lower hardware block size.
    if method is None and min(w, h) < (8 if fmt in DXT else 2):
        method = 'preserved-minimum-size'
    if method is not None:
        report.update(method=method, after=report['before'], outputSha256=sha(wire))
        return wire, report
    if depth != 1 or levels < 1 or fmt not in (*DXT, 21, 50):
        raise kvap.KvapError(f'unsupported 3D image layout: {name}')
    sizes = [packs.mip_bytes(fmt, max(1, w >> n), max(1, h >> n)) for n in range(levels)]
    faces = 6 if flags & 4 else 1
    face_bytes = sum(sizes)
    if face_bytes * faces != size:
        raise kvap.KvapError(f'image payload/mip mismatch: {name}')
    payload = wire[32+nl:]
    if levels > 1:
        output = b''.join(payload[f*face_bytes+sizes[0]:(f+1)*face_bytes] for f in range(faces))
        levels -= 1
        method = 'authored-next-mip'
    else:
        output = b''.join(half_single(payload[f*face_bytes:(f+1)*face_bytes], w, h, fmt)
                          for f in range(faces))
        method = 'box-filter-single-level'
    w //= 2; h //= 2
    result = struct.pack('<8I', nl, len(output), w, h, depth, levels, fmt, flags) + wire[32:32+nl] + output
    report.update(method=method, after=[w, h, levels, len(output)], outputSha256=sha(result))
    return result, report


def half_bundle(raw, protected):
    records = packs.parse_viti_records(raw)
    output = {}; report = []
    for name, (wire, _) in records.items():
        wire, item = half_record(name, wire, protected)
        output[name] = (wire, struct.unpack_from('<I', wire, 4)[0])
        report.append(item)
    return packs.build_viti_records(output), report


def build(base, output, workspace):
    base = base.resolve(); output = output.resolve(); workspace = workspace.resolve()
    if not output.is_relative_to(workspace) or output == workspace or output.exists():
        raise kvap.KvapError('Choose a new output directory inside the workspace')
    old = validate_content(base)
    from PIL import __version__ as pillow_version, DdsImagePlugin, _imaging
    provenance = dict(policy=POLICY, baseManifestSha256=packs.sha256_file(base/'vertical-slice-manifest.json'),
                      toolSha256=packs.sha256_file(Path(__file__)), pillow=pillow_version,
                      pillowDdsSha256=packs.sha256_file(Path(DdsImagePlugin.__file__)),
                      pillowCodecSha256=packs.sha256_file(Path(_imaging.__file__)))
    source_hash = sha(bytes.fromhex(old['sourceInstallHash']) + json.dumps(provenance, sort_keys=True).encode())
    pending = output.with_name(output.name + '.pending')
    if pending.exists():
        raise kvap.KvapError('Pending output already exists')
    pending.mkdir(parents=True)
    work = pending / 'chunks'; work.mkdir()
    manifest = copy.deepcopy(old)
    manifest.update(sourceInstallHash=source_hash, textureReduction=provenance, packages={}, reports={})
    manifest['sourceInputs']['textureReduction'] = sha(json.dumps(provenance, sort_keys=True).encode())
    reports = {}
    try:
        names = ['common.kvap'] + sorted(n for n in old['packages'] if n != 'common.kvap')
        for filename in names:
            source = base / filename
            _, chunks = packs.read_source_index(source)
            entries = []; image_report = []; invariants = []
            for chunk in chunks:
                raw = packs.read_raw_chunk(source, chunk)
                before = sha(raw)
                if chunk.kind == kvap.CHUNK_TYPES['image']:
                    if raw[:4] != b'VITI':
                        raise kvap.KvapError('Unknown image chunk must be audited')
                    raw, records = half_bundle(raw, ui_names(old))
                    image_report.extend(dict(assetId=f'0x{chunk.asset_id:016x}', **r) for r in records)
                elif chunk.kind == kvap.CHUNK_TYPES['manifest']:
                    data = json.loads(raw)
                    if data.get('packageRole') in ('common', 'map'):
                        data['sourceInstallHash'] = source_hash
                        data['sourceInputs'] = manifest['sourceInputs']
                        data['textureReduction'] = dict(policy=POLICY, relativeDimensionScale=0.5,
                            uiUnchanged=True, basePackSha256=old['packages'][filename]['sha256'],
                            memoryEstimates='unchanged conservative upper bounds from base')
                        if 'imagePolicy' in data:
                            data['baseImagePolicy'] = data.pop('imagePolicy')
                        # Old counts describe the full-resolution source set.
                        data.pop('imageReports', None); data.pop('imageMipReports', None)
                        if data['packageRole'] == 'map':
                            data['commonPackageId'] = manifest['reports']['common']['packageId']
                            data['commonSha256'] = manifest['packages']['common.kvap']['sha256']
                        raw = packs.embedded_content_manifest(data).encode()
                else:
                    invariants.append(dict(type=chunk.kind, assetId=f'0x{chunk.asset_id:016x}', sha256=before))
                path = work / f'{chunk.index}.bin'; path.write_bytes(raw)
                entries.append(packs.chunk_entry(chunk.kind, chunk.asset_id, path))
            report = packs.write_pack(dict(sourceHash=source_hash, chunks=entries), work/'package.json', pending/filename, workspace)
            key = 'common' if filename == 'common.kvap' else Path(filename).stem[3:]
            manifest['reports'][key] = report
            manifest['packages'][filename] = dict(sha256=packs.sha256_file(pending/filename), bytes=(pending/filename).stat().st_size)
            reports[filename] = dict(images=image_report, unchangedChunks=invariants,
                inputPayloadBytes=sum(r['before'][3] for r in image_report),
                outputPayloadBytes=sum(r['after'][3] for r in image_report))
            for path in work.iterdir():
                path.unlink()
        work.rmdir()
        for name in old.get('streams', {}):
            dest = pending/name; dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(base/name, dest)
        for name in old.get('media', {}):
            dest = pending/'media'/name; dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(base/'media'/name, dest)
        (pending/'texture-reduction.json').write_text(json.dumps(dict(provenance=provenance, packages=reports), indent=2)+'\n')
        (pending/'vertical-slice-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        validate_content(pending)
        pending.rename(output)
        return {name: {k:v for k,v in report.items() if k.endswith('PayloadBytes')} for name,report in reports.items()}
    except Exception as error:
        (pending/'FAILED.txt').write_text(str(error)+'\n')
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--workspace', type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()
    print(json.dumps(build(args.base, args.output, args.workspace), indent=2))
