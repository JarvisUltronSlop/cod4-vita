"""Compile the native dynamic-buffer create/update path against host GL endpoints."""
import argparse
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();s=a.source.read_text(encoding='utf-8')
parts=[]
for signature in ['void DeleteNativePoseExtra(', 'GfxBufferHandle CreateDynamicVertexBuffer(', 'bool UploadPackedViewmodel(', 'bool UpdateDynamicVertexBuffer(', 'void DestroyBuffer(']:
    start=s.index(signature);end=s.index('{',start)+1;depth=1
    # A forward declaration is not the function body.
    while ';' in s[start:end]:
        start=s.index(signature,start+len(signature));end=s.index('{',start)+1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    parts.append(s[start:end])
a.output.write_text('\n'.join(parts)+'\n',encoding='utf-8')
