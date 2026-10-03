import unittest
import test_world_alpha
from analyze_render_passes import analyze_render_passes
class WorldLifetimeTests(unittest.TestCase):
 def fixture(self):
  rows=test_world_alpha.WorldAlphaTests().fixture()
  for row in rows:
   name=row.get('name','')
   if name.startswith('cpu-diagnostic-'):row['value0']=26
   if name=='render-probe-kind':row['value0']=23
   if name in ('render-probe-world-alpha','render-probe-world-alpha-target'):row['value0']=row['value1']
  for row in list(rows):
   if row.get('name')=='render-probe-frame':
    for name,a,b in [('',2 if row['value0']==2 else 0,2),('-valid',0,1),('-targets',3 if row['value0']==2 else 0,3)]:
     rows.append({**row,'name':'render-probe-world-lifetime'+name,'value0':a,'value1':b})
  return rows
 def test_complete_and_retained_paths(self):
  r=analyze_render_passes(self.fixture())[0];self.assertTrue(r['captureComplete'],r['invalidFrames']);self.assertTrue(r['worldLifetimeComparison']['candidateActuallyUsed'])
  for k in ['postFilmComparison','worldAlphaComparison','drawWaitBreakdown']:self.assertIsNone(r[k])
  for p in r['phases']:
   self.assertEqual(p['worldAlpha']['draws'],p['worldAlpha']['eligible']);self.assertEqual(p['weaponSpecular']['draws'],p['weaponSpecular']['eligible'])
 def test_wrong_missing_or_unseen(self):
  for suffix in ('','-valid','-targets'):
   rows=[r for r in self.fixture() if r.get('name')!='render-probe-world-lifetime'+suffix];self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
  for suffix,field,value in [('', 'value0',1),('', 'value1',1),('', 'value1',999),('-valid','value0',1),('-valid','value1',0),('-targets','value0',1),('-targets','value1',0),('-targets','value1',1)]:
   rows=self.fixture();next(r for r in rows if r.get('name')=='render-probe-world-lifetime'+suffix)[field]=value;self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],suffix)
if __name__=='__main__':unittest.main()
