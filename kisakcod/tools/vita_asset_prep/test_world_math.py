import copy
import unittest
import test_world_lifetime
import test_gpu_resources
from analyze_render_passes import analyze_render_passes
from analyze_gpu_resources import analyze_resources

class WorldMathTests(unittest.TestCase):
 def fixture(self):
  rows=test_world_lifetime.WorldLifetimeTests().fixture()
  for r in rows:
   name=r.get('name','')
   if name.startswith('cpu-diagnostic-'):r['value0']=28
   if name=='render-probe-kind':r['value0']=25
   if name.startswith('render-probe-world-lifetime'):
    r['name']=name.replace('world-lifetime','world-math')
    if name.endswith('-valid'):r['value1']=3
    elif name.endswith('-targets'):r['value0']=7 if r['value0'] else 0;r['value1']=7
    else:r['value0']=3 if r['value0'] else 0;r['value1']=3
  return rows
 def resources(self):
  rows=[r for r in test_gpu_resources.ResourceAuditTests().fixture() if not r['name'].startswith('gpu-audit-p2-')]
  for old,new in [(1,4),(3,5)]:
   for r in list(rows):
    if r['name'].startswith(f'gpu-audit-p{old}-'):
     r={**r,'name':r['name'].replace(f'p{old}-',f'p{new}-')}
     if r['name'].endswith('-program'):r['value0']=new+10
     if r['name'].endswith('-f-instructions'):r['value0']-=3
     rows.append(r)
  for r in rows:
   if r['name'].startswith('gpu-audit-s') and r['name'].endswith('-program'):
    r['value0']=r['value1']=(15 if '-s588-' in r['name'] else 14) if '-p2-' in r['name'] else (13 if '-s588-' in r['name'] else 11)
  rows.append(dict(name='world-math-ready',generation=0,value0=3,value1=1))
  return rows
 def test_complete(self):
  a=analyze_render_passes(self.fixture())[0];self.assertTrue(a['captureComplete'],a['invalidFrames'])
  self.assertTrue(a['worldMathComparison']['candidateActuallyUsed']);self.assertIsNone(a['worldLifetimeComparison']);self.assertIsNone(a['lightmapLayoutComparison'])
  b=analyze_resources(self.resources());self.assertTrue(b['complete'],b['problems'])
  for family in ('opaque-world','transparent-world'):
   self.assertEqual(b['worldMathComparison'][family]['f']['deltas']['primaryInstructions'],-3)
   self.assertTrue(b['worldMathComparison'][family]['v']['codeBytesIdentical'])
 def test_fail_closed(self):
  for suffix in ('','-valid','-targets'):
   rows=[r for r in self.fixture() if r.get('name')!='render-probe-world-math'+suffix];self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
  for suffix,field,value in [('', 'value0',1),('', 'value1',999),('-valid','value0',1),('-valid','value1',1),('-targets','value0',1),('-targets','value1',3)]:
   rows=self.fixture();next(r for r in rows if r.get('name')=='render-probe-world-math'+suffix)[field]=value
   self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
 def test_actual_program_and_texture_invariance(self):
  for name,field in [('gpu-audit-s588-p2-program','value0'),('gpu-audit-s261-p3-u4-filter','value0'),('gpu-audit-p4-v-code-hash0','value0')]:
   rows=self.resources();next(r for r in rows if r['name']==name)[field]+=1
   self.assertFalse(analyze_resources(rows)['complete'],name)
  for prefix in ('gpu-audit-p4-','gpu-audit-p5-','gpu-audit-s588-p2-'):
   self.assertFalse(analyze_resources([r for r in self.resources() if not r['name'].startswith(prefix)])['complete'])
if __name__=='__main__':unittest.main()
