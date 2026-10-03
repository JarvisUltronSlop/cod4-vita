import unittest
from analyze_benchmark import analyze
from benchmark_audio_analysis import name_hash
from test_benchmark_geometry_analysis import geometry_fixture

def fixture():
    rows,identity=geometry_fixture()
    identity['round']=rows[0]['round']='audio-6'
    names=('native-a','no-background-streams','native-b','ambient-pcm','native-c')
    for r in list(rows):
        if r['type'] not in ('case','frame'): continue
        i=r.get('case',r.get('index')); mode=1 if i==1 else 2 if i==3 else 0
        r.update(audioMode=mode,geometryMode=0)
        if r['type']=='case': r['id']=names[i]; continue
        r.update(audioErrors=0,audioStreams=1,eligibleStreams=1,pcmStreams=int(mode==2),
                 suppressedStreams=int(mode==1),draws=130,triangles=8000)
        rows.append({'type':'audio-stream-frame','case':i,'n':r['n'],'channel':40,
            'nameHash':name_hash('ambient/amb_middle_east1v7_lr.mp3'),'rate':44100,'channels':2,
            'eligible':True,'pcm':mode==2,'suppressed':mode==1,
            'readUs':0 if mode==1 else 1000,'readBytes':0 if mode==1 else 8192,
            'readCalls':0 if mode==1 else 2,'decodeIncludingReadUs':0 if mode==1 else 5000,
            'decodedFrames':0 if mode==1 else 8192,'refillCalls':0 if mode==1 else 1,
            'processedBuffers':0 if mode==1 else 1,'uploads':0 if mode==1 else 1,
            'stoppedWithQueuedBuffers':0})
    return rows,identity

class AudioAnalysisTests(unittest.TestCase):
    def test_valid_round(self):
        rows,id=fixture(); r=analyze(rows,id)
        self.assertTrue(r['validComparison'],r['errors'])
        self.assertTrue(all(c['passesScreen'] for c in r['comparisons']))

    def test_audio_interventions_and_format_must_match(self):
        for key,value in [('pcm',False),('rate',24000),('channels',1),('suppressed',True),
                          ('nameHash',123),('eligible',False),('readCalls',-1)]:
            rows,id=fixture()
            row=next(r for r in rows if r['type']=='audio-stream-frame' and r['case']==3)
            row[key]=value
            self.assertFalse(analyze(rows,id)['validComparison'],key)

    def test_disabled_work_and_missing_stream_logs_rejected(self):
        rows,id=fixture(); row=next(r for r in rows if r['type']=='audio-stream-frame' and r['case']==1)
        row['readBytes']=1
        self.assertFalse(analyze(rows,id)['validComparison'])
        rows,id=fixture(); rows.pop()
        self.assertFalse(analyze(rows,id)['validComparison'])
        rows,id=fixture(); rows.append(next(r for r in rows if r['type']=='audio-stream-frame'))
        self.assertFalse(analyze(rows,id)['validComparison'])

    def test_audio_error_and_changed_graphics_rejected(self):
        for field,value in [('audioErrors',1),('audioMode',2),('geometryMode',1),('noNormalMapDraws',5)]:
            rows,id=fixture(); next(r for r in rows if r['type']=='frame')[field]=value
            self.assertFalse(analyze(rows,id)['validComparison'],field)

    def test_missing_additional_audio_identity_rejected(self):
        rows,id=fixture(); id['actualFiles']['audio-file']='extra'
        self.assertFalse(analyze(rows,id)['validComparison'])
        rows.append({'type':'identity','path':'audio-file','sha256':'extra'})
        self.assertTrue(analyze(rows,id)['validComparison'])

if __name__=='__main__': unittest.main()
