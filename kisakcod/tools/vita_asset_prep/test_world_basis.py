import unittest
import test_world_math
from analyze_render_passes import analyze_render_passes
from analyze_gpu_resources import analyze_resources
class WorldBasisTests(unittest.TestCase):
 def fixture(self):
  rows=test_world_math.WorldMathTests().fixture()
  for r in rows:
   name=r.get('name','')
   if name.startswith('cpu-diagnostic-'):r['value0']=29
   if name=='render-probe-kind':r['value0']=26
   if name.startswith('render-probe-world-math'):r['name']=name.replace('world-math','world-basis')
  for r in list(rows):
   if r.get('name')=='render-probe-frame':
    for name,a,b in [('seen',7,1),('triangles',3,4)]:rows.append({**r,'name':'render-probe-world-basis-'+name,'value0':a,'value1':b})
  return rows
 def resources(self):
  rows=test_world_math.WorldMathTests().resources()
  for r in rows:
   name=r['name']
   if name=='world-math-ready':r['name']='world-basis-ready'
   for old,new in [(4,6),(5,7)]:
    if name.startswith(f'gpu-audit-p{old}-'):
     r['name']=name.replace(f'p{old}-',f'p{new}-')
     if name.endswith('-program'):r['value0']=new+10
   if name.startswith('gpu-audit-s') and name.endswith('-program') and '-p2-' in name:r['value0']+=2;r['value1']+=2
  for r in list(rows):
   if r['name'].startswith('gpu-audit-s') and r['name'].endswith('-program'):
    rows.append({**r,'name':r['name'].replace('-program','-basis'),'value0':1,'value1':1})
  return rows
 def test_complete_and_partial_eligibility(self):
  a=analyze_render_passes(self.fixture())[0];self.assertTrue(a['captureComplete'],a['invalidFrames']);self.assertTrue(a['worldBasisComparison']['candidateActuallyUsed'])
  rows=self.fixture()
  for r in rows:
   if r.get('name')=='render-probe-world-basis-targets':r['value0']=3 if r['value0'] else 0;r['value1']=3
  self.assertTrue(analyze_render_passes(rows)[0]['captureComplete'])
  a=analyze_resources(self.resources());self.assertTrue(a['complete'],a['problems'])
  rows=self.resources()
  for r in rows:
   if '-s588-' in r['name']:
    if r['name'].endswith('-basis'):r['value0']=0
    if r['name'].endswith('-program'):r['value0']=r['value1']=13
  a=analyze_resources(rows);self.assertTrue(a['complete'],a['problems']);self.assertFalse(next(x for x in a['captures'] if x['surface']==588)['constantBasis'])
 def test_incomplete_or_stale_certificate_rejected(self):
  for suffix in ('','-valid','-targets','-seen','-triangles'):
   self.assertFalse(analyze_render_passes([r for r in self.fixture() if r.get('name')!='render-probe-world-basis'+suffix])[0]['captureComplete'])
  for name in ('gpu-audit-s588-p2-basis','gpu-audit-s507-p3-program'):
   rows=self.resources();next(r for r in rows if r['name']==name)['value0']=0
   self.assertFalse(analyze_resources(rows)['complete'])
 def test_unseen_targets_or_zero_coverage_rejected(self):
  for suffix,field,value in [('-targets','value1',0),('-seen','value0',3),('-triangles','value0',0),('-triangles','value0',999),('-valid','value0',1),('','value0',1)]:
   rows=self.fixture();next(r for r in rows if r.get('name')=='render-probe-world-basis'+suffix)[field]=value
   self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
if __name__=='__main__':unittest.main()
