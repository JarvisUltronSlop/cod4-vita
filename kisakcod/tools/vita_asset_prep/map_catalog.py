"""Read the runtime's authoritative stable map catalog."""
from pathlib import Path
import re
CATALOG = Path(__file__).resolve().parents[2] / 'src/vita/vita_map_catalog.inc'
ROWS = tuple((int(i),name,title,key,enabled=='true') for i,name,title,key,enabled in
    re.findall(r'VITA_MAP\((\d+), "([^"]+)", "([^"]+)", "([^"]+)", (true|false)\)', CATALOG.read_text()))
if len(ROWS)!=20 or [r[0] for r in ROWS]!=list(range(20)):
    raise ValueError('Invalid stable COD4 map catalog')
ORIGINAL_MAPS = {i:name for i,name,_,_,enabled in ROWS if enabled}
def map_index(name):
    name=name.removeprefix('mp_')
    for i,n in ORIGINAL_MAPS.items():
        if name==n:return i
    raise ValueError('Not an original COD4 multiplayer map: '+name)
def load_inputs(path):
    import json
    path=Path(path).resolve()
    value=json.loads(path.read_text());result={}
    if not isinstance(value,dict) or not value:
        raise ValueError('Map inputs must be a nonempty object')
    for name,entry in value.items():
        i=map_index(name)
        if i in result:raise ValueError('Duplicate map input: '+name)
        if not isinstance(entry,dict) or 'source' not in entry:raise ValueError('Missing typed map source: '+name)
        if set(entry)-{'source','soundClosure','supplement'}:
            raise ValueError('Unknown map input field: '+name)
        if any(not isinstance(val,str) or not val for val in entry.values()):
            raise ValueError('Map input paths must be nonempty strings: '+name)
        result[i]={key:(path.parent/val).resolve() for key,val in entry.items()}
    if 0 not in result:raise ValueError('Shipment is required for the common frontend closure')
    return dict(sorted(result.items()))
