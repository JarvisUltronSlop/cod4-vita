import unittest
import struct
import test_world_alpha
from analyze_render_passes import analyze_render_passes

class PostFilmTests(unittest.TestCase):
    def fixture(self):
        rows=test_world_alpha.WorldAlphaTests().fixture()
        for row in rows:
            name=row.get('name','')
            if name.startswith('cpu-diagnostic-'):row['value0']=25
            if name=='render-probe-kind':row['value0']=22
            if name in ('render-probe-world-alpha','render-probe-world-alpha-target'):row['value0']=row['value1']
            if name=='render-pass-post-geometry':row['value0'],row['value1']=1,2
        bits=lambda f:struct.unpack('<I',struct.pack('<f',f))[0]
        for row in list(rows):
            if row.get('name')=='render-probe-frame':
                for name,a,b in [('shader',int(row['value0']==2),1),('valid',0,1),('state',1,1),('gamma-exposure',bits(.8),bits(1)),('size',960,544)]:
                    rows.append({**row,'name':'render-probe-post-'+name,'value0':a,'value1':b})
        return rows
    def test_film_variant_with_retained_world_weapon(self):
        r=analyze_render_passes(self.fixture())[0]
        self.assertTrue(r['captureComplete'],r['invalidFrames'])
        self.assertTrue(r['postFilmComparison']['candidateActuallyUsed'])
        self.assertIsNone(r['worldAlphaComparison']);self.assertIsNone(r['drawWaitBreakdown'])
        for p in r['phases']:
            self.assertEqual(p['worldAlpha']['draws'],p['worldAlpha']['eligible'])
            self.assertEqual(p['weaponSpecular']['draws'],p['weaponSpecular']['eligible'])
            self.assertEqual(p['postFilm']['draws'],p['postFilm']['eligible'] if p['phase']==2 else 0)
    def test_missing_corrupt_or_wrong_dispatch(self):
        for name in ('shader','valid','state','gamma-exposure','size'):
            rows=[r for r in self.fixture() if r.get('name')!='render-probe-post-'+name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        for name,field,value in [('shader','value0',1),('shader','value1',0),('valid','value0',1),('valid','value1',0),('state','value0',3),('state','value1',0),('gamma-exposure','value0',0x7fc00000),('size','value0',720)]:
            rows=self.fixture();next(r for r in rows if r.get('name')=='render-probe-post-'+name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)
        rows=self.fixture();rows.append(dict(next(r for r in rows if r.get('name')=='render-probe-post-valid')))
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
    def test_unsupported_effect_state_remains_generic_and_unqualified(self):
        for state in (0,2,3):
            rows=self.fixture()
            for row in rows:
                if row.get('name')=='render-probe-post-state':row['value0']=state
                if row.get('name')=='render-probe-post-shader':row['value0']=row['value1']=0
            r=analyze_render_passes(rows)[0]
            self.assertFalse(r['captureComplete']);self.assertEqual(r['invalidFrames'],[])
            self.assertFalse(r['postFilmComparison']['candidateActuallyUsed'])

if __name__=='__main__':unittest.main()
