from pathlib import Path
import subprocess
import unittest
from analyze_cpu import render_pipeline_evidence

class RenderPipelineTests(unittest.TestCase):
    def test_observed_dispatch_and_ring_fallback(self):
        rows=[dict(name='render-pipeline-'+n,value0=2,value1=2,generation=1,sequence=5) for n in
              ('family','fast-color','fast-clear','weapon','world-alpha','weapon-prep')]
        rows += [dict(name='render-pipeline-'+n,value0=a,value1=b,generation=1,sequence=5) for n,a,b in
                 [('invalid',0,0),('weapon-pack',2,600),('weapon-bytes',26400,1)]]
        rows += [dict(name='render-pose-ring-use',value0=2,value1=0,generation=1,sequence=5)]
        result=render_pipeline_evidence(rows)
        self.assertTrue(result['ordinaryPipelineObserved'] and result['ringNoFallbackObserved'])
        self.assertFalse(render_pipeline_evidence([])['ordinaryPipelineObserved'])
        self.assertFalse(render_pipeline_evidence(rows[1:])['ordinaryPipelineObserved'])
        rows[-1]['value1']=1;self.assertFalse(render_pipeline_evidence(rows)['ringNoFallbackObserved'])
        rows[0]['value0']=0;self.assertFalse(render_pipeline_evidence(rows)['ordinaryPipelineObserved'])

    def test_policy_and_buffer_lifetimes(self):
        root=Path(__file__).resolve().parents[3]
        target=root/'build/host-render-pipeline';target.mkdir(exist_ok=True)
        (target/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(RenderPipelineTests LANGUAGES CXX)
add_executable(render-pipeline "{(root/'kisakcod/tests/vita_portable/render_pipeline_tests.cpp').as_posix()}")
target_include_directories(render-pipeline PRIVATE "{(root/'kisakcod/src').as_posix()}")
target_compile_features(render-pipeline PRIVATE cxx_std_20)
''')
        commands=[['cmake','-S',str(target),'-B',str(target/'build')]]
        for config in ('Debug','Release'):
            commands += [['cmake','--build',str(target/'build'),'--config',config],
                         [str(target/f'build/{config}/render-pipeline.exe')]]
        for cmd in commands:
            result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            self.assertEqual(result.returncode,0,result.stdout)

if __name__=='__main__':unittest.main()
