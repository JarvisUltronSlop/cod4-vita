"""Freeze the staged pack header/TOC identities for cheap on-device checks."""
from pathlib import Path
import struct
import zlib
from map_catalog import ORIGINAL_MAPS


def prepare(pack_root, names):
    names=list(names)
    if len(names)!=len(set(names)): raise ValueError('Duplicate staged pack')
    rows=[]
    indexes={f'mp_{name}.kvap': index for index,name in ORIGINAL_MAPS.items()}
    indexes['common.kvap']=0xffffffff
    for name in sorted(names):
        if name not in indexes: raise ValueError('Unknown staged pack: '+name)
        path=Path(pack_root)/name
        with path.open('rb') as stream:
            header=stream.read(64)
            if len(header)!=64 or header[:4]!=b'KVAP': raise ValueError('Bad pack header: '+name)
            offset,count=struct.unpack_from('<QI',header,12)
            if not count or count>256: raise ValueError('Bad pack TOC: '+name)
            stream.seek(offset); toc=stream.read(count*64)
            if len(toc)!=count*64: raise ValueError('Truncated pack TOC: '+name)
        key=name.encode()+b'\0'
        if len(key)>32: raise ValueError('Pack name exceeds wire capacity')
        rows.append(struct.pack('<IQI32s', indexes[name], path.stat().st_size,
                                zlib.crc32(header+toc), key))
    if not rows or len(rows)>17: raise ValueError('Invalid pack count')
    return struct.pack('<4sIII',b'VPKL',1,48,len(rows))+b''.join(rows)
