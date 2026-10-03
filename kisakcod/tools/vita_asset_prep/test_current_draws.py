import unittest
import struct
import test_world_alpha
import test_gpu_draws
from analyze_render_passes import analyze_render_passes

class CurrentDrawTests(unittest.TestCase):
    def fixture(self):
        rows=test_world_alpha.WorldAlphaTests().fixture()
        for row in rows:
            name=row.get('name','')
            if name.startswith('cpu-diagnostic-'):row['value0']=24
            if name in ('cpu-diagnostic-end','cpu-diagnostic-capture-complete'):row['startUs']+=120000000
            if name=='render-probe-kind':row['value0']=21
            if name in ('render-probe-world-alpha','render-probe-world-alpha-target'):row['value0']=row['value1']
        for row in test_gpu_draws.GpuDrawTests().fixture():
            name=row.get('name','')
            if not name.startswith('gpu-') or name.startswith('gpu-anchor-'):continue
            if name.startswith('gpu-key-'):
                chunk=int(name[8]);words=[int(name[10+i:18+i],16) for i in range(0,64,8)]
                post=row['value0']>8
                if chunk==0 and post:words[0]=5
                if chunk==1:
                    words[3]=0xffffffff if post else row['value0']+580
                    if post:words[4:8]=[22,33,0,0]
                if chunk==2:words[6]=256 if post else 9
                row['name']=f'gpu-key-{chunk}-'+''.join(f'{v:08x}' for v in words)
            if name=='gpu-draw-pass-wait':
                if row['value0']==2:row['value0']=5
                elif row['value0']==5:row['value0']=2
            rows.append(row)
        bits=lambda f:struct.unpack('<I',struct.pack('<f',f))[0]
        for row in list(rows):
            if row.get('name')=='render-probe-frame':
                for name,a,b in [('gpu-post-state',1,3),('gpu-post-gamma-exposure',bits(.8),bits(1)),('gpu-post-size',960,544)]:
                    rows.append({**row,'name':name,'value0':a,'value1':b})
        return rows

    def test_current_paths_and_targets(self):
        r=analyze_render_passes(self.fixture())[0]
        self.assertTrue(r['captureComplete'],r['invalidFrames'])
        self.assertIsNone(r['worldAlphaComparison'])
        self.assertIsNone(r['weaponSpecularComparison'])
        d=r['drawWaitBreakdown']
        self.assertFalse(d['normalFrameImprovementClaimed'])
        self.assertTrue(d['postState']['availableAndValid'])
        self.assertTrue(d['targetCoverage']['opaqueWorld']['sufficient'])
        self.assertTrue(d['targetCoverage']['post']['sufficient'])
        self.assertFalse(d['targetCoverage']['composition']['sufficient']) # Native world has no separate scale copy.
        for p in r['phases']:
            self.assertEqual(p['worldAlpha']['draws'],p['worldAlpha']['eligible'])
            self.assertEqual(p['weaponSpecular']['draws'],p['weaponSpecular']['eligible'])
        for draw in d['draws']:
            if draw['pipeline']=='post':
                self.assertIsNone(draw['sourceSurface'])
                self.assertEqual(draw['textures'],{})
                self.assertEqual(draw['fullscreenInputs'],dict(scene=22,dither=33))

    def test_missing_and_bad_post_evidence(self):
        for name in ('gpu-post-state','gpu-post-gamma-exposure','gpu-post-size','gpu-draw-wait'):
            rows=[r for r in self.fixture() if r.get('name')!=name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)
        for name,field,value in [('gpu-post-state','value0',4),('gpu-post-state','value1',99),('gpu-post-gamma-exposure','value0',0x7fc00000),('gpu-post-size','value0',720),('render-probe-world-alpha','value0',0),('render-probe-weapon-specular','value0',0)]:
            rows=self.fixture();next(r for r in rows if r.get('name')==name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)
        rows=self.fixture();rows.append(dict(next(r for r in rows if r.get('name')=='gpu-post-state')))
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])

if __name__=='__main__':unittest.main()

