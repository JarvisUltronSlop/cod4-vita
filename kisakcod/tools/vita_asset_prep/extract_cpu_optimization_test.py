"""Compile unchanged source lighting and terrain math in host differential tests."""
import argparse
from pathlib import Path
import re

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--kind',choices=['lighting','terrain','audio','residency','session-stop','initial-join','primary-lights','package-probe'],required=True)
a=p.parse_args();s=a.source.read_text(encoding='utf-8')
def extract(start):
    end=s.index('{',start)+1;depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]
if a.kind=='package-probe':
    parts=[extract(s.index('bool ProbePackage('))]
elif a.kind=='primary-lights':
    parts=[extract(s.index('bool LoadShipmentPrimaryLights('))]
elif a.kind=='initial-join':
    parts=[extract(s.index('void VitaSourceSession::AdvanceInitialJoin()'))]
elif a.kind=='session-stop':
    session=s
    s=(a.source.parent.parent/'sound/snd_driver_openal.cpp').read_text(encoding='utf-8')
    parts=[extract(s.rindex(name)) for name in ['static void SND_ReleaseStreamChannel(',
        'void __cdecl SND_StopStreamChannel(', 'bool __cdecl SND_IsStreamChannelFree(',
        'static void SND_ResumeExistingStream(', 'static void SND_ServiceExistingStreams(']]
    s=(a.source.parent.parent/'sound/snd.cpp').read_text(encoding='utf-8')
    parts.append(extract(s.index('void __cdecl SND_StopSounds(')))
    s=session
    parts.append(extract(s.index('void VitaSourceSession::Stop()')))
elif a.kind=='lighting':
    parts=[extract(s.index('struct '+name+' {'))+';' for name in ['ShipmentWorldLighting','ShipmentLightGridSample']]
    parts += [extract(s.rindex('bool '+name+'(')) for name in ['EvaluateSourceModelLightGrid','EvaluateSourceModelLightResponse','EvaluateSourceModelLightGridResponse']]
elif a.kind=='residency':
    parts=[extract(s.index('struct NativeModelSemanticResidency {'))+';']
    parts += [extract(s.index(name)) for name in ['GfxTextureHandle FindModelTexture(',
        'GfxTextureHandle FindModelSemanticTexture(', 'NativeModelSemanticResidency RebindNativeModelSemantics(']]
elif a.kind=='audio':
    parts=[extract(s.rindex('static void '+name+'(')) for name in ['SND_ResumeExistingStream','SND_ServiceExistingStreams']]
else:
    parts=[extract(m.start()) for m in re.finditer(r'^(?:void|bool|double|SphereEdgeTraceResult) __cdecl [A-Za-z0-9_]+\(',s,re.M)]
    # Count calls in the host harness only; original collision math is untouched.
    for i,part in enumerate(parts):
        if any((' '+name+'(') in part[:part.index('{')] for name in ['CM_TracePointThroughTriangle','CM_TraceCapsuleThroughTriangle','CM_PositionTestCapsuleInTriangle']):
            parts[i]=part.replace('{','{\n    ++terrainTriangleCalls;',1)
    parts=[v[:v.index('{')].strip()+';' for v in parts]+parts
    s=(a.source.parent.parent/'vita/vita_source_registry.cpp').read_text(encoding='utf-8')
    original=extract(s.index('    for (size_t partitionIndex = 0; partitionIndex < partitionCount;'))
    parts.append('void BuildOriginalTerrain(const auto& collision, CollisionPartition* partitions, CollisionAabbTree* aabbTrees) {\n'
                 'const size_t triangleCount=collision.triangles_.size(); const size_t partitionCount=(triangleCount+254u)/255u;\n'
                 +original+'\n}')
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text('\n'.join(parts)+'\n',encoding='utf-8')
