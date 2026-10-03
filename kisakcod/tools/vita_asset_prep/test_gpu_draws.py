import unittest
import test_render_passes
from analyze_render_passes import analyze_render_passes

class GpuDrawTests(unittest.TestCase):
    def fixture(self):
        rows=test_render_passes.RenderPassTests().family_fixture()
        for row in rows:
            if row.get('name','').startswith('cpu-diagnostic-'):row['value0']=14
            if row.get('name') in ('cpu-diagnostic-end','cpu-diagnostic-capture-complete'):row['startUs']+=120000000
            if row.get('name')=='render-probe-kind':row['value0']=11
            if row.get('name') in ('render-probe-family-lightmapped','render-probe-family-baked'):row['value0']=row['value1']
        step=0
        for frame_index,frame in enumerate([r for r in rows if r.get('name')=='render-probe-frame']):
            phase=frame['value0']
            def add(name,a,b=0):rows.append({**frame,'name':name,'value0':a,'value1':b})
            if frame_index==0:
                for ident in range(1,17):
                    key=[1 if ident<=8 else 2,100+ident,2,4,30,1]+[0]*18
                    for chunk in range(3):add('gpu-key-'+str(chunk)+'-'+''.join(f'{w:08x}' for w in key[8*chunk:8*chunk+8]),ident)
                    add('gpu-site-TestDraw',ident,100+ident)
            for at in range(0,16,4):add(f'gpu-order-{at:04d}',(at+1)|((at+2)<<32),(at+3)|((at+4)<<32))
            first=(step%2)*8 if phase==2 else 0
            if phase==2:step+=1
            add('gpu-draw-frame',16,16);add('gpu-draw-window',first,8 if phase==2 else 0)
            add('gpu-draw-prefix',1000 if phase==2 else 0,first);add('gpu-draw-empty',100 if phase==2 else 0,8)
            add('gpu-draw-health',0,2048);add('gpu-draw-report-us',20)
            totals=[0]*7
            if phase==2:
                p=1 if first==0 else 2;totals[p]=1100
                for ordinal in range(first,first+8):
                    ident=ordinal+1;k=(ordinal<<32)|ident
                    add('gpu-draw-submit',k,10);add('gpu-draw-wait',k,200+ident);totals[p]+=200+ident
            for p,n in enumerate(totals):add('gpu-draw-pass-wait',p,n)
        return rows

    def result(self,rows):return analyze_render_passes(rows)[0]
    def test_complete_ranked_draws_and_separate_control(self):
        r=self.result(self.fixture());self.assertTrue(r['captureComplete'])
        d=r['drawWaitBreakdown'];self.assertTrue(d['coverageComplete']);self.assertEqual(len(d['draws']),16)
        self.assertEqual(d['draws'][0]['id'],16);self.assertEqual(d['draws'][0]['completionWait']['samples'],3)
        self.assertEqual(d['draws'][0]['completionWait']['meanUs'],216)
        self.assertEqual(d['phases'][1]['emptyControl']['meanUs'],100)
        self.assertEqual(d['phases'][0]['totalProbeWait']['meanUs'],0)
        self.assertIsNone(r['materialFamilyComparison']);self.assertIsNone(r['texturePrefetchComparison'])

    def test_missing_corrupt_or_duplicate_evidence(self):
        for prefix in ('gpu-key-0-','gpu-site-','gpu-order-0000','gpu-draw-wait','gpu-draw-submit','gpu-draw-pass-wait','gpu-draw-empty','gpu-draw-frame','gpu-draw-health'):
            rows=[r for r in self.fixture() if not r.get('name','').startswith(prefix)]
            self.assertFalse(self.result(rows)['captureComplete'],prefix)
        for name,field,value in [('gpu-draw-health','value0',1),('gpu-draw-window','value1',5),('gpu-draw-prefix','value0',999),('gpu-order-0000','value0',99999),('gpu-draw-pass-wait','value1',1)]:
            rows=self.fixture();next(r for r in rows if r.get('name')==name)[field]=value
            self.assertFalse(self.result(rows)['captureComplete'],name)
        rows=self.fixture();rows.append(dict(next(r for r in rows if r.get('name')=='gpu-draw-wait')))
        self.assertFalse(self.result(rows)['captureComplete'])

    def test_recording_loss_and_incomplete_duration(self):
        rows=self.fixture();rows[0]['dropped']=1;self.assertFalse(self.result(rows)['captureComplete'])
        self.assertFalse(analyze_render_passes(self.fixture(),['truncated'])[0]['captureComplete'])
        rows=self.fixture();next(r for r in rows if r.get('name')=='cpu-diagnostic-end')['startUs']-=120000000
        self.assertFalse(self.result(rows)['captureComplete'])

    def test_sparse_coverage_is_not_success(self):
        rows=[r for r in self.fixture() if r.get('sequence')!=205]
        result=self.result(rows)
        self.assertTrue(result['captureComplete'])
        self.assertFalse(result['drawWaitBreakdown']['coverageComplete'])
        self.assertEqual(result['drawWaitBreakdown']['unmeasuredStableSignatures'],[])
        self.assertEqual(result['drawWaitBreakdown']['fewerThanThreeSamples'],list(range(9,17)))

if __name__=='__main__':unittest.main()
