"""Fail-closed evidence checks for broad draw-omission diagnostics, not GPU cycles."""
import math
import struct

def scalar(value):
    if not isinstance(value,int) or not 0<=value<=0xffffffff:return float('nan')
    return struct.unpack('<f',struct.pack('<I',value))[0]

DEPTH_NAMES=['render-probe-composition-copy','render-probe-composition-valid']+['render-composition-depth-'+s for s in ('before','during','after','state')]
ISOLATION_NAMES=DEPTH_NAMES+['render-isolation-health','render-isolation-post','graphics-profile-frame','graphics-profile-scenes','graphics-profile-health','display-probe-health']+['render-isolation-draws-'+str(i) for i in range(7)]+['graphics-scene-'+str(i)+'-'+s for i in range(3) for s in ('begin-cpu','cpu-time','state','result','format','load-store')]

def depth_problem(records,fused):
    def pair(n):return [records[n]['value0'],records[n]['value1']]
    before,during,after=([scalar(v) for v in pair('render-composition-depth-'+s)] for s in ('before','during','after'))
    flags,func=pair('render-composition-depth-state')
    if (not all(math.isfinite(v) for v in before+during+after) or not 0<=before[0]<=before[1]<=1
            or during!=([before[1]]*2 if fused else before) or after!=before
            or flags&15!=15 or bool(flags&16)!=fused or bool(flags&32)!=fused
            or (fused and (not flags&64 or func!=0x0207)) or bool(flags&64)!=(func==0x0207)
            or flags&~127 or pair('render-probe-composition-copy')!=[1,int(fused)]
            or pair('render-probe-composition-valid')!=[0,960*544]):
        return 'composition depth, restored range, masks or actual copy differs'
    return None

def isolation_problem(records,phase):
    problem=depth_problem(records,False)
    if problem:return problem
    def pair(n):return [records[n]['value0'],records[n]['value1']]
    if pair('render-isolation-health')!=[0,phase] or pair('render-isolation-post')!=[int(phase==8),0]:return 'draw gate or post program failed'
    target={2:1,4:2,6:4}.get(phase)
    # The renderer marks Setup before clearing the world target. Its required
    # clear therefore belongs to World, and the draw gate deliberately keeps it.
    # Two fast clears are observed: one world clear and one native-composition
    # clear. Accept exactly that one retained draw, not arbitrary missing coverage.
    if pair('render-probe-fast-clear')!=[2,2]:return 'required target clears differ'
    for i in range(7):
        prepared,suppressed=pair('render-isolation-draws-'+str(i))
        retained_clear=1 if phase==2 and i==1 else 0
        if prepared<0 or suppressed!=((prepared-retained_clear) if i==target else 0) or (i==target and suppressed<=0):return 'missing or unexpected suppressed draws'
        if i in (1,2,4) and prepared<=0:return 'unseen world, prop or weapon category'
        if i in (3,5) and prepared!={3:2,5:1}[i]:return 'fullscreen target coverage differs'
        if prepared!=records['render-pass-'+('setup','world','props-fx','composition','weapon','post','hud')[i]+'-geometry']['value0']:return 'unaccounted draw path'
    if pair('graphics-profile-scenes')!=[3,phase] or pair('graphics-profile-frame')!=[phase,1] or pair('graphics-profile-health')!=[0,1]:return 'native scene coverage or passive timeline failed'
    if pair('display-probe-health')!=[1,0]:return 'display observer failed'
    for i in range(3):
        prefix='graphics-scene-'+str(i)+'-'
        if pair(prefix+'result')!=[0,phase] or pair(prefix+'load-store')[1]!=0:return 'native scene call failed'
        begin,end=pair(prefix+'begin-cpu'),pair(prefix+'cpu-time')
        if not begin[0]<=begin[1]<=end[0]<=end[1]:return 'invalid CPU scene timestamps'
        state=pair(prefix+'state');width,height=(720,408) if i==0 else (960,544)
        if state[0]>>32!=[3,5,7][i] or state[1]!=(width<<32|height) or pair(prefix+'format')[1]!=width:return 'unexpected native scene target or viewport'
    return None
