from pathlib import Path
import subprocess
import struct
import unittest
from analyze_world_scaling import analyze_world_scaling
from analyze_render_passes import PASSES
from analyze_cpu import pose_upload_timings,ordinary_world_lighting


def bits(value):return struct.unpack('<I',struct.pack('<f',value))[0]


class WorldScalingTests(unittest.TestCase):
    def fixture(self):
        rows=[dict(type='recording-health',dropped=0,ioFailed=False)]
        def add(name,a,b=0,t=3000000,seq=0):
            rows.append(dict(name=name,value0=a,value1=b,startUs=t,generation=1,sequence=seq))
        for name in ('gpu-anchor-origin-xy','gpu-anchor-origin-z-yaw','gpu-anchor-angles','gpu-anchor-source-ready'):
            add(name,1 if name.endswith('ready') else 0,1 if name.endswith('ready') else 0,t=2000001 if name.endswith('ready') else 1)
        next(r for r in rows if r.get('name')=='gpu-anchor-origin-xy').update(value0=bits(164),value1=bits(-591))
        next(r for r in rows if r.get('name')=='gpu-anchor-origin-z-yaw').update(value0=bits(220),value1=bits(90))
        add('cpu-diagnostic-begin',37)
        for phase in range(1,8):
            size=(480,272) if phase==6 else (720,408)
            for i in range(6):
                seq=phase*100+i;t=3000000+(phase-1)*20000000+i*100000
                def frame(name,a,b=0):add(name,a,b,t,seq)
                frame('render-probe-frame',phase,127);frame('render-probe-kind',34)
                duration=100000 if phase in (2,4,6) else 170000
                frame('render-probe-total-us',duration);frame('render-probe-swap-us',duration-30000,duration-31000)
                frame('render-probe-viewports',7)
                frame('render-probe-camera-xy',bits(164),bits(-591));frame('render-probe-camera-z',bits(253.2668),1)
                frame('render-world-scale-sky',1,1 if phase==2 else 0)
                frame('render-world-scale-draws',10 if phase==4 else 0,10)
                frame('render-world-scale-valid',0,3)
                frame('render-world-scale-targets',7 if phase==4 else 0,7)
                frame('render-world-scale-world',20,1 if phase==6 else 0)
                frame('render-world-scale-size',*size)
                for v in range(3):frame(f'render-world-scale-viewport-{v}',*(size if v<2 else (960,544)))
                for p in PASSES:frame('render-pass-'+p,1000);frame('render-pass-'+p+'-geometry',3,20)
        add('cpu-diagnostic-end',37,t=143000000);add('cpu-diagnostic-capture-complete',37,t=143000001)
        return rows

    def test_complete(self):
        r=analyze_world_scaling(self.fixture())[0]
        self.assertTrue(r['captureComplete'],r)
        self.assertEqual([x['frameReductionVsBeforeUs'] for x in r['comparisons']],[70000]*3)
        self.assertEqual([x['retainedFrames'] for x in r['phases']],[4]*7)

    def test_missing_and_duplicate_evidence(self):
        for name in ['render-world-scale-sky','render-world-scale-viewport-2','render-world-scale-size','render-world-scale-targets','cpu-diagnostic-capture-complete','gpu-anchor-source-ready']:
            rows=self.fixture();r=next(x for x in rows if x.get('name')==name);rows.remove(r)
            self.assertFalse(analyze_world_scaling(rows)[0]['captureComplete'],name)
            rows=self.fixture();rows.append(next(x for x in rows if x.get('name')==name))
            self.assertFalse(analyze_world_scaling(rows)[0]['captureComplete'],name)

    def test_camera_height_is_not_player_origin(self):
        result=analyze_world_scaling(self.fixture())[0]
        self.assertTrue(result['captureComplete'])
        self.assertEqual(result['diagnosticAnchor'],[164,-591,220])
        self.assertAlmostEqual(result['diagnosticCamera'][2],253.2668,places=4)

    def test_invalid_or_moving_camera(self):
        for name,field,value in [('render-probe-camera-xy','value0',bits(165)),
                                 ('render-probe-camera-z','value0',bits(254)),
                                 ('render-probe-camera-z','value1',0),
                                 ('render-probe-camera-z','value0',bits(float('nan'))),
                                 ('render-probe-camera-xy','value0',-1),
                                 ('gpu-anchor-origin-z-yaw','value0',bits(float('inf'))),
                                 ('gpu-anchor-angles','value0',bits(float('nan')))]:
            rows=self.fixture()
            # A later frame must be rejected even if movement stays inside
            # the spawn-to-eye tolerance.
            next(r for r in rows if r.get('name')==name and
                 (name.startswith('gpu-') or r['sequence']==400))[field]=value
            self.assertFalse(analyze_world_scaling(rows)[0]['captureComplete'],(name,field,value))
        rows=self.fixture()
        for r in rows:
            if r.get('name')=='render-probe-camera-z':r['value0']=bits(400)
        self.assertFalse(analyze_world_scaling(rows)[0]['captureComplete'])

    def test_failed_or_unused_changes(self):
        for name,field,value in [('render-world-scale-draws','value0',0),('render-world-scale-valid','value1',0),('render-world-scale-targets','value0',3),('render-world-scale-sky','value1',0),('render-world-scale-size','value0',720),('render-world-scale-viewport-2','value0',480),('render-probe-viewports','value0',3),('render-pass-world','value1',1)]:
            rows=self.fixture();phase=2 if name.endswith('sky') else 6 if name.endswith('size') else 4
            next(x for x in rows if x.get('name')==name and x.get('sequence')==phase*100)[field]=value
            self.assertFalse(analyze_world_scaling(rows)[0]['captureComplete'],name)

    def test_unavailable_shader_preserves_other_comparisons(self):
        rows=self.fixture()
        for r in rows:
            if r.get('name')=='render-world-scale-valid':r['value1']=0
            if r.get('name') in ('render-world-scale-draws','render-world-scale-targets'):r['value0']=0
        result=analyze_world_scaling(rows)[0]
        self.assertFalse(result['captureComplete'])
        self.assertEqual([c['available'] for c in result['comparisons']],[True,False,True])

    def test_invalid_recording(self):
        rows=self.fixture();rows[0]['dropped']=1
        self.assertFalse(analyze_world_scaling(rows)[0]['captureComplete'])
        self.assertFalse(analyze_world_scaling(self.fixture(),['truncated log'])[0]['captureComplete'])
        self.assertEqual(analyze_world_scaling([]),[])

    def test_native_contracts(self):
        root=Path(__file__).resolve().parents[3]
        target=root/'build/host-world-scaling';target.mkdir(exist_ok=True)
        test=root/'kisakcod/tests/vita_portable/world_scaling_tests.cpp'
        (target/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(WorldScalingTests LANGUAGES CXX)
add_executable(world-scaling "{test.as_posix()}")
target_include_directories(world-scaling PRIVATE "{(root/'kisakcod/src').as_posix()}")
target_compile_features(world-scaling PRIVATE cxx_std_20)
''')
        commands=[['cmake','-S',str(target),'-B',str(target/'build')]]
        for config in ('Debug','Release'):
            commands += [['cmake','--build',str(target/'build'),'--config',config],
                         [str(target/f'build/{config}/world-scaling.exe'),str(root/'kisakcod/src/gfx_vita/gfx_vita_backend.cpp')]]
        for cmd in commands:
            result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            self.assertEqual(result.returncode,0,result.stdout)

    def test_upload_timing_aggregation_and_missing_evidence(self):
        self.assertFalse(pose_upload_timings([])['subdataBreakdownAvailable'])
        rows=[dict(type='scope',role=1,name='render-pose-subdata',value0=2,value1=900,elapsedUs=1000),
              dict(type='scope',role=1,name='render-pose-subdata',value0=1,value1=200,elapsedUs=200),
              dict(type='scope',role=0,name='render-pose-subdata',value0=1,value1=9000,elapsedUs=9000)]
        result=pose_upload_timings(rows)
        self.assertTrue(result['subdataBreakdownAvailable'])
        stage=result['stages']['render-pose-subdata']
        self.assertEqual((stage['calls'],stage['meanCallUs'],stage['maximumCallUs']),(3,400,900))

    def test_ordinary_lighting_actual_evidence(self):
        self.assertFalse(ordinary_world_lighting([])['dispatchEvidencePassed'])
        rows=[dict(name=n,value0=a,value1=b,generation=1,sequence=2) for n,a,b in
              [('render-simple-world-draws',160,160),('render-simple-world-state',1,3),('render-simple-world-invalid',0,0)]]
        self.assertTrue(ordinary_world_lighting(rows)['dispatchEvidencePassed'])
        for index,field,value in [(0,'value0',0),(1,'value1',0),(2,'value0',1)]:
            changed=[dict(r) for r in rows];changed[index][field]=value
            self.assertFalse(ordinary_world_lighting(changed)['dispatchEvidencePassed'])
        self.assertFalse(ordinary_world_lighting(rows[:-1])['dispatchEvidencePassed'])
        self.assertFalse(ordinary_world_lighting(rows+[rows[0]])['dispatchEvidencePassed'])

    def test_target_warmup_is_not_ordinary_play(self):
        group=lambda seq,t:[dict(name=n,value0=a,value1=b,generation=1,sequence=seq,startUs=t+i)
            for i,(n,a,b) in enumerate([('render-simple-world-draws',0 if seq==0 else 160,0 if seq==0 else 160),
                ('render-simple-world-state',1,3),('render-simple-world-invalid',0,0)])]
        warm=group(0,10)+group(0,20)
        marker=dict(name='map-to-first-source-snapshot-us',generation=1,startUs=100)
        result=ordinary_world_lighting(warm+[marker]+group(5,200))
        self.assertTrue(result['dispatchEvidencePassed'])
        self.assertEqual((result['preSourceWarmupRecords'],result['frames']),(6,1))
        self.assertFalse(ordinary_world_lighting(warm+group(5,200))['dispatchEvidencePassed'])
        self.assertFalse(ordinary_world_lighting([marker]+group(0,200)+group(0,210))['dispatchEvidencePassed'])


if __name__=='__main__':unittest.main()
