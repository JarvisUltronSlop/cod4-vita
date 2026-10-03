"""Compile the actual registry allocation/teardown methods in host tests."""
import argparse
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();text=a.source.read_text()
declarations=('VitaAssetRegistry::~VitaAssetRegistry()',
    'void VitaAssetRegistry::ReleaseScriptStringsTo(', 'void VitaAssetRegistry::ReleaseScriptStrings()',
    'bool VitaAssetRegistry::Initialize(', 'void VitaAssetRegistry::Destroy()',
    'void VitaAssetRegistry::Reset(', 'void* VitaAssetRegistry::Allocate(',
    'VitaAssetRegistryStats VitaAssetRegistry::Stats() const')
parts=[]
for declaration in declarations:
    start=text.index(declaration);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    parts.append(text[start:end])
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text('namespace kisak::vita {\n'+'\n'.join(parts)+'\n}\n')
