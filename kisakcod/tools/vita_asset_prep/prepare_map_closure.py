"""Close original-map image dependencies from exact owned COD4 sources.
Outputs are workspace-local, segmented, and independently auditable.
"""
import argparse, hashlib, json, struct, zipfile
from pathlib import Path
import kvap, vertical_slice_packs as packs, shipment_typed_export as typed
from repair_map_image_closures import _material_records, RetailImages, _write_image_records
from map_catalog import map_index, ORIGINAL_MAPS
import vita_environment

FAMILIES={'worldNormal':0x4D49020000000000,'worldSpecular':0x4D49030000000000,
          'modelNormal':0x4D49040000000000,'modelSpecular':0x4D49050000000000,
          'colors':0x4D49080000000000,'hands':0x4D49060000000000}

def lod_materials(path):
    typed._validate_model_lod_sidecar(path)
    data=path.read_bytes();count=struct.unpack_from('<I',data,8)[0];at=20;names=set()
    for _ in range(count):
        n,_,stored=struct.unpack_from('<3I',data,at);at+=32+n
        for _ in range(stored):
            _,surfaces,vertices,indices=struct.unpack_from('<4I',data,at);at+=16
            for _ in range(surfaces):
                n=struct.unpack_from('<I',data,at)[0];at+=24
                names.add(data[at:at+n].decode());at+=n
            at+=vertices*32+indices*2
    assert at==len(data)
    return names

def required_images(bymagic, output):
    """Derive dependencies from source surfaces, retained LODs and active models."""
    names={b'VITW':'vita_world.bin',b'VITL':'vita_materials.bin',b'VITX':'vita_models.bin',
           b'VLOD':'vita_model_lods.bin',b'VITR':'vita_rawfiles.bin',b'VITE':'vita_map_ents.bin',b'VITP':'vita_weapons.bin',b'VFXA':'vita_effects.bin'}
    for magic,name in names.items(): (output/name).write_bytes(bymagic[magic])
    materials=_material_records(bymagic[b'VITL'])
    world=set(typed._world_surface_material_names(output/'vita_world.bin'))
    from source_runtime_profile import profile_entities
    active_entities,entity_changes=profile_entities(bymagic[b'VITE'])
    (output/'vita_map_ents.bin').write_bytes(active_entities)
    requested=typed._entity_model_names(output/'vita_map_ents.bin') | typed._weapon_view_model_names(output/'vita_weapons.bin')
    requested.update({'viewhands_', 'viewmodel_base_viewhands','viewmodel_knife'})
    import vita_effects
    graph=vita_effects._read_v2_graph(output/'vita_effects.bin')
    for effect in graph['graphs'].values():
        for element in effect.get('elements',[]):
            if int(element.get('elemType',0))==5:
                requested.update(str(v).strip() for v in element.get('visuals',[]) if v)
    models=set(typed._model_surface_material_names(output/'vita_models.bin',requested))|lod_materials(output/'vita_model_lods.bin')
    if not world or not models:raise ValueError('Empty required material closure')
    missing_materials=(world|models)-materials.keys()
    if missing_materials:raise ValueError('Missing retained surface materials: '+', '.join(sorted(missing_materials)))
    def aliases(selected,slot):return {materials[n][slot] for n in selected if materials[n].get(slot)}
    required={'worldNormal':aliases(world,'normal'),'worldSpecular':aliases(world,'specular'),
              'modelNormal':aliases(models,'normal'),'modelSpecular':aliases(models,'specular'),
              'colors':aliases(world|models,'color')}
    hand_materials=set(typed._model_surface_material_names(output/'vita_models.bin',{'viewhands_', 'viewmodel_base_viewhands'}))
    required['hands']=aliases(hand_materials,'color')
    return required,entity_changes


def source_streams(source):
    _,chunks=packs.read_source_index(source)
    return {magic:packs.read_raw_chunk(source,chunk)
            for chunk,magic in packs.direct_binary_chunks(source,chunks)}


def validate_closure(required, available):
    for kind,names in required.items():
        missing=names-available.get(kind,set())
        if missing:
            raise ValueError(f'Missing required {kind} images: '+', '.join(sorted(missing)))


def validate_supplement(source, supplement, index, workspace):
    """Recompute source dependencies; a preparation report is not a certificate."""
    import tempfile
    identity,chunks=packs.read_source_index(supplement)
    if identity.hex()!=packs.sha256_file(source):
        raise ValueError('Supplement belongs to a different typed source')
    available={kind:set() for kind in FAMILIES}
    seen=set();segments={kind:[] for kind in FAMILIES};environment=None
    for chunk in chunks:
        if chunk.kind==kvap.CHUNK_TYPES['manifest']:continue
        if chunk.asset_id in seen:raise ValueError('Duplicate supplemental asset')
        seen.add(chunk.asset_id)
        raw=packs.read_raw_chunk(supplement,chunk)
        if chunk.kind==kvap.CHUNK_TYPES['environment']:
            if chunk.asset_id!=(0x4D56000000000000|index) or environment is not None:
                raise ValueError('Invalid map environment supplement')
            environment=raw
            continue
        family=chunk.asset_id&0xFFFFFFFFFFFF0000
        kind=next((k for k,v in FAMILIES.items() if v==family),None)
        if kind is None or chunk.kind!=kvap.CHUNK_TYPES['image']:
            raise ValueError('Unexpected supplemental chunk')
        if chunk.asset_id&255 != (0 if kind=='hands' else index):
            raise ValueError('Supplement contains another map')
        segments[kind].append((chunk.asset_id>>8)&255)
        names=packs.parse_viti_records(raw).keys()
        if available[kind]&names:raise ValueError('Duplicate supplemental image alias')
        available[kind].update(names)
    if environment is None:raise ValueError('Missing map environment supplement')
    for kind,values in segments.items():
        limit=1 if kind=='hands' else 64 if kind=='colors' else 16
        if sorted(values)!=list(range(len(values))) or len(values)>limit:
            raise ValueError('Noncontiguous or excessive image segments: '+kind)
    bymagic=source_streams(source)
    available['colors'].update(packs.parse_viti_records(bymagic[b'VITI']))
    with tempfile.TemporaryDirectory(prefix='closure-check-',dir=workspace/'build') as temporary:
        work=Path(temporary)
        required,_=required_images(bymagic,work)
        vita_environment.build_environment_sidecar(work/'vita_rawfiles.bin',work/'environment.bin',index,ORIGINAL_MAPS[index])
        if environment!=(work/'environment.bin').read_bytes():
            raise ValueError('Environment does not match the selected source map')
    validate_closure(required,available)
    return {kind:len(names) for kind,names in required.items()}


def prepare(source, index, retail_root, output, workspace):
    source=source.resolve();output=output.resolve();output.relative_to(workspace.resolve())
    output.mkdir(parents=True,exist_ok=True)
    bymagic=source_streams(source)
    required,entity_changes=required_images(bymagic,output)
    # Engine-generated utility images retain their exported payloads.
    original=packs.parse_viti_records(bymagic[b'VITI']);retail=RetailImages(retail_root)
    audit={};entries=[]
    def resolve(name):
        if name in original:return original[name]
        record,status=retail.lookup(name);location=retail.source_cache.get(name.lower())
        if record is None or location is None or Path(location[1]).stem.lower()!=name.lower():
            raise ValueError('Missing exact required image '+name+': '+status)
        wire=packs.parse_viti_records(_write_image_records([(name,record)]))[name]
        with zipfile.ZipFile(location[0]) as z:digest=hashlib.sha256(z.read(location[1])).hexdigest()
        audit[name]={'archive':str(location[0]),'entry':location[1],'iwiSha256':digest,'recordSha256':hashlib.sha256(wire[0]).hexdigest()}
        return wire
    for kind,aliases_set in required.items():
        # Existing colors remain in their original startup stream; only missing
        # required colors need the tail. Hand colors have their own map override.
        if kind=='colors':aliases_set=aliases_set-original.keys()
        segment={};size=0;number=0
        def flush():
            nonlocal segment,size,number
            if not segment:return
            limit=1 if kind=='hands' else 64 if kind=='colors' else 16
            if number>=limit:raise ValueError(f'{kind} closure exceeds runtime segment capacity')
            raw=packs.build_viti_records(segment)
            target=output/f'{kind}-{number}.bin';target.write_bytes(raw)
            asset=FAMILIES[kind] | (number<<8) | (0 if kind=='hands' else index)
            entries.append({'type':'image','assetId':asset,'path':str(target)})
            number+=1;segment={};size=0
        for name in sorted(aliases_set):
            record=resolve(name)
            if size+len(record[0])>16*1024*1024:flush()
            segment[name]=record;size+=len(record[0])
        flush()
    environment=output/'environment.bin'
    vita_environment.build_environment_sidecar(output/'vita_rawfiles.bin',environment,index,ORIGINAL_MAPS[index])
    entries.append({'type':'environment','assetId':0x4D56000000000000|index,'path':str(environment)})
    manifest={'sourceHash':hashlib.sha256(source.read_bytes()).hexdigest(),'chunks':entries}
    spec=output/'supplement-spec.json';spec.write_text(json.dumps(manifest,indent=2))
    result=kvap.create_package(spec,output/'supplement.kvap',workspace,workspace/'__owned_unused__')
    report={'map':ORIGINAL_MAPS[index],'typedSha256':packs.sha256_file(source),'required':{k:sorted(v) for k,v in required.items()},'sourceProfileEntityChanges':entity_changes,'resolvedFromOwnedInstall':audit,'package':result,'hardwareQualified':False}
    (output/'closure-report.json').write_text(json.dumps(report,indent=2)+'\n')
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('source','retail-root','output','workspace'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--map',required=True);a=p.parse_args()
    prepare(a.source,map_index(a.map),a.retail_root,a.output,a.workspace)
