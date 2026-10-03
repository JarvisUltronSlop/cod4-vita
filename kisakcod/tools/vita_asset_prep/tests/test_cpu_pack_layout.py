import struct
import zlib
import pytest
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from cpu_pack_layout import prepare


def pack(path):
    header=bytearray(64)
    header[:4]=b'KVAP'
    struct.pack_into('<QI',header,12,64,1)
    toc=bytes(range(64))
    path.write_bytes(header+toc+b'payload')
    return zlib.crc32(header+toc)


def test_layout_matches_stable_map_ids_and_metadata_only(tmp_path):
    crc=pack(tmp_path/'mp_shipment.kvap')
    pack(tmp_path/'common.kvap')
    result=prepare(tmp_path,['mp_shipment.kvap','common.kvap'])
    assert struct.unpack_from('<4sIII',result)==(b'VPKL',1,48,2)
    assert struct.unpack_from('<IQI',result,16)==(0xffffffff,135,crc)
    assert struct.unpack_from('<IQI',result,64)==(0,135,crc)
    assert result[80:112].split(b'\0')[0]==b'mp_shipment.kvap'
    (tmp_path/'mp_shipment.kvap').write_bytes((tmp_path/'mp_shipment.kvap').read_bytes()[:-7]+b'changed')
    assert prepare(tmp_path,['mp_shipment.kvap','common.kvap'])==result
    # This check supplements the runtime's full chunk CRC; it cannot replace it.


def test_layout_rejects_invalid_inputs(tmp_path):
    pack(tmp_path/'common.kvap')
    for names in ([],['common.kvap','common.kvap'],['mp_carentan.kvap'],['../common.kvap']):
        with pytest.raises(ValueError):prepare(tmp_path,names)
    (tmp_path/'common.kvap').write_bytes(b'KVAP')
    with pytest.raises(ValueError):prepare(tmp_path,['common.kvap'])


def test_layout_rejects_truncated_toc(tmp_path):
    pack(tmp_path/'common.kvap')
    data=(tmp_path/'common.kvap').read_bytes()
    (tmp_path/'common.kvap').write_bytes(data[:127])
    with pytest.raises(ValueError):prepare(tmp_path,['common.kvap'])
