"""Original-map selection, strict closure, and transactional publication."""
import functools
import json
import struct
import sys
import zipfile
from pathlib import Path

import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import map_catalog as catalog
import vertical_slice_packs as packs
from prepare_map_closure import validate_closure
from repair_map_image_closures import RetailImages


def test_stable_original_ids():
    expected={0:'shipment',1:'crash',2:'crossfire',3:'bog',4:'countdown',5:'farm',
              6:'pipeline',7:'showdown',8:'strike',9:'vacant',10:'cargoship',11:'backlot',
              12:'bloc',15:'citystreets',16:'convoy',19:'overgrown'}
    assert catalog.ORIGINAL_MAPS==expected
    for index,name in expected.items():
        assert catalog.map_index(name)==catalog.map_index('mp_'+name)==index
    for invalid in ('broadcast','chinatown','creek','killhouse','crash_snow','../crash','CRASH','mp_crash;quit'):
        with pytest.raises(ValueError):catalog.map_index(invalid)


def test_inputs_relative_to_manifest(tmp_path):
    path=tmp_path/'batch.json'
    path.write_text(json.dumps({'mp_shipment':{'source':'sources/shipment.kvap'},'crash':{'source':'crash.kvap'}}))
    inputs=catalog.load_inputs(path)
    assert set(inputs)=={0,1} and inputs[0]['source']==tmp_path/'sources/shipment.kvap'
    for value in ([],{}, {'crash':{'source':'x'}}, {'shipment':{'source':'x','typo':'y'}},
                  {'shipment':{'source':3}}, {'shipment':{'source':'x'},'mp_shipment':{'source':'y'}}):
        path.write_text(json.dumps(value))
        with pytest.raises(ValueError):catalog.load_inputs(path)


def test_every_hand_alias_is_required():
    required={'hands':{'viewhands_desert_opfor'},'modelNormal':{'opfor_nml'},
              'modelSpecular':{'opfor_spc','opfor_spc2'}}
    validate_closure(required,required)
    for kind,names in required.items():
        for missing in names:
            incomplete={k:set(v) for k,v in required.items()}
            incomplete[kind].remove(missing)
            with pytest.raises(ValueError,match=missing):validate_closure(required,incomplete)
    with pytest.raises(ValueError):validate_closure(required,{'hands':{'viewhands_marine_gloves_col'}})


def test_texture_residency_preserves_protected_storage_and_layout():
    def record(name,flags=0,width=8):
        payload=bytes(range(32))
        wire=struct.pack('<8I',len(name),len(payload),width,8,1,1,0x31545844,flags)+name.encode()+payload
        return packs.build_viti_records({name:(wire,len(payload))})
    seen=set()
    assert packs.texture_residency(record('brick'),seen)==(0,32,65536,1)
    assert packs.texture_residency(record('same_pixels'),seen)==(0,0,0,0)
    assert packs.texture_residency(record('different_layout',width=16),seen)==(0,32,65536,1)
    # The GPU lease pool segregates protected images from evictable images,
    # even for identical source pixels.
    assert packs.texture_residency(record('*lightmap0_primary'),seen)==(32,0,0,0)
    assert packs.texture_residency(record('weapon_mp5_c'),seen)==(0,0,0,0)
    assert packs.texture_residency(record('sky',flags=4),seen)==(32,0,0,0)
    header=(Path(__file__).resolve().parents[3]/'src/vita/vita_texture_store.h').read_text()
    assert 'kVitaTexturePageGpuBytes=16u*1024u*1024u' in header
    assert packs.PAGED_WORLD_GPU_BUDGET==16*1024*1024


@pytest.mark.parametrize('image_format,payload,expected',[
    (2,b'\x11\x22\x33',b'\x11\x22\x33\xff'),
    (3,b'\x44\x55',b'\x44\x44\x44\x55'),
    (5,b'\x66',b'\xff\xff\xff\x66')])
def test_exact_owned_bitmap_decode(tmp_path,image_format,payload,expected):
    archive=tmp_path/'images.iwd'
    header=struct.pack('<3sBBB3h4i',b'IWi',6,image_format,2,1,1,1,28+len(payload),0,0,0)
    with zipfile.ZipFile(archive,'w') as out:out.writestr('images/exact.iwi',header+payload)
    record,status=RetailImages._decode(archive,'images/exact.iwi')
    assert status=='found' and record[5]==21 and record[7]==expected


def test_failed_set_never_publishes_and_existing_set_is_preserved(tmp_path,monkeypatch):
    original=packs._build
    @functools.wraps(original)
    def failing(*args,**kwargs):
        import inspect
        call=inspect.signature(original).bind(*args,**kwargs)
        output=call.arguments['output'];(output/'common.kvap').write_bytes(b'incomplete')
        raise packs.kvap.KvapError('GPU budget exceeded')
    monkeypatch.setattr(packs,'_build',failing)
    destination=tmp_path/'batch'
    with pytest.raises(packs.kvap.KvapError,match='GPU budget'):
        packs.build(None,destination,tmp_path,None,None,map_inputs={0:{'source':tmp_path/'source'}})
    assert not destination.exists()
    failures=list(tmp_path.glob('batch.failed-*'))
    assert len(failures)==1 and (failures[0]/'FAILED.txt').is_file()
    assert not (failures[0]/'vertical-slice-manifest.json').exists()
    destination.mkdir();(destination/'common.kvap').write_bytes(b'protected')
    with pytest.raises(packs.kvap.KvapError,match='already exists'):
        packs.build(None,destination,tmp_path,None,None,map_inputs={0:{'source':tmp_path/'source'}})
    assert (destination/'common.kvap').read_bytes()==b'protected'


def test_delivery_validator_enumerates_all_original_maps(tmp_path):
    from validate_vita_candidate import validate_content
    identity='ab'*32
    content={'schemaVersion':1,'sourceInstallHash':identity,'packages':{},'selectedMaps':[
        {'index':i,'name':n} for i,n in catalog.ORIGINAL_MAPS.items()]}
    def write(name,data):
        payload=tmp_path/'payload.json';payload.write_text(json.dumps(data))
        spec=tmp_path/'spec.json';spec.write_text(json.dumps({'sourceHash':identity,'chunks':[
            {'type':'manifest','assetId':1,'path':str(payload)}]}))
        report=packs.kvap.create_package(spec,tmp_path/name,tmp_path,tmp_path/'owned')
        content['packages'][name]={'bytes':(tmp_path/name).stat().st_size,'sha256':packs.sha256_file(tmp_path/name)}
        return report
    base={'schemaVersion':1,'certified':True,'payloadPolicy':'runtime-ready-v1','sourceInstallHash':identity,
          'predictedCpuResidentBytes':1024,'predictedGpuResidentBytes':1024}
    common=write('common.kvap',dict(base,packageRole='common'))
    for index,name in catalog.ORIGINAL_MAPS.items():
        write('mp_'+name+'.kvap',dict(base,packageRole='map',mapIndex=index,mapId=name,
            commonPackageId=common['packageId'],commonSha256=content['packages']['common.kvap']['sha256']))
    manifest=tmp_path/'vertical-slice-manifest.json';manifest.write_text(json.dumps(content))
    assert len(validate_content(tmp_path)['packages'])==17
    broken=dict(content,selectedMaps=content['selectedMaps'][:-1]);manifest.write_text(json.dumps(broken))
    with pytest.raises(ValueError,match='Selected-map'):validate_content(tmp_path)
    manifest.write_text(json.dumps(content))
    with (tmp_path/'mp_crash.kvap').open('ab') as stream:stream.write(b'corrupt')
    with pytest.raises(ValueError,match='hash/size'):validate_content(tmp_path)
