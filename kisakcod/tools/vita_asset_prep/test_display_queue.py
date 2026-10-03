import unittest
import test_render_passes
from analyze_render_passes import analyze_render_passes


class DisplayQueueTests(unittest.TestCase):
    def fixture(self):
        rows=test_render_passes.RenderPassTests().family_fixture()
        rows=[r for r in rows if not r.get('name','').startswith('render-probe-family-')]
        for r in rows:
            if r.get('name','').startswith('cpu-diagnostic-'):r['value0']=12
            if r.get('name')=='render-probe-kind':r['value0']=9
        for row in list(rows):
            if row.get('name')!='render-probe-frame':continue
            ident=row['sequence'];phase=row['value0'];start=row['startUs'];wait=100 if phase==2 else 500
            fields=[('submit-id',ident,phase),('submit-start',ident,start),('submit-end',ident,start+wait),
                    ('submit-finish-us',ident,400 if phase==2 else 0),('submit-status',ident,0),('submit-context',ident,1),
                    ('probe-config',0,2),('probe-health',1,0),('cb-enter',ident,start+wait-50),
                    ('cb-exit',ident,start+wait-10),('cb-setbuf-us',ident,10),('cb-vblank-us',ident,0),
                    ('cb-calls',1,0),('cb-status',ident,0),('cb-thread',77,0x70000),('cb-priority',ident,64)]
            for name,a,b in fields:rows.append({**row,'name':'display-'+name,'value0':a,'value1':b})
        rows.append(dict(name='display-probe-drained',generation=5,startUs=64000000,value0=0,value1=0))
        return rows

    def test_display_split_and_no_false_optimization(self):
        result=analyze_render_passes(self.fixture())[0]
        self.assertTrue(result['captureComplete']);self.assertIsNone(result['opaqueShaderComparison'])
        q=result['displayQueueProbe'];self.assertTrue(q['captureComplete'])
        self.assertEqual(q['phases'][0]['addEntryUs']['meanUs'],500)
        self.assertEqual(q['phases'][1]['addEntryUs']['meanUs'],100)
        self.assertEqual(q['phases'][1]['finishUs']['meanUs'],400)
        self.assertEqual(q['phases'][1]['callbackUs']['meanUs'],40)
        self.assertEqual(q['phases'][1]['vblankCalls'],0)

    def test_missing_callbacks_bad_status_and_pairing(self):
        for name in ['cb-enter','cb-exit','cb-setbuf-us','cb-vblank-us','cb-status','submit-id','probe-drained']:
            rows=[r for r in self.fixture() if r.get('name')!='display-'+name]
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)
        for name,field,value in [('cb-enter','value0',9999),('cb-status','value1',1),('probe-health','value1',1),
                                 ('submit-context','value1',0),('probe-health','value0',0),('submit-finish-us','value1',100),
                                 ('cb-setbuf-us','value1',100),('cb-exit','value1',0),('submit-end','value1',0),
                                 ('submit-id','value1',2),('submit-status','value1',0xffffffff),('cb-calls','value0',0)]:
            rows=self.fixture();next(r for r in rows if r.get('name')=='display-'+name)[field]=value
            self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'],name)
        rows=self.fixture();rows.append(next(r.copy() for r in rows if r.get('name')=='display-cb-enter'))
        self.assertFalse(analyze_render_passes(rows)[0]['captureComplete'])
        self.assertFalse(analyze_render_passes(self.fixture(),['truncated'])[0]['captureComplete'])

if __name__=='__main__':unittest.main()
