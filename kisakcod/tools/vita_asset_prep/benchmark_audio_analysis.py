"""Validate driver-level evidence independently of whole-frame performance."""
from collections import defaultdict
import statistics

def name_hash(name):
    h=2166136261
    for b in name.encode(): h=((h^b)*16777619)&0xffffffff
    return h

PCM_HASHES={name_hash('ambient/amb_middle_east1v7_lr.mp3'),name_hash('ambient/amb_middle_east1v7_lsrs.mp3')}

def validate_audio(records, frames, cases, reject):
    details=defaultdict(list)
    for row in records:
        if row.get('type')=='audio-stream-frame': details[(row.get('case'),row.get('n'))].append(row)
    summary={}; baseline_layout=None
    expected_keys={(i,f['n']) for i,ff in frames.items() for f in ff}
    if set(details)!=expected_keys: reject('Missing or extra per-frame audio records')
    for i,ff in frames.items():
        mode=1 if i==1 else 2 if i==3 else 0
        if cases.get(i,{}).get('audioMode')!=mode: reject('Requested audio mode mismatch')
        measured=[]
        for f in ff:
            if f.get('audioMode')!=mode or f.get('audioErrors')!=0: reject('Audio mode/failure mismatch')
            if f.get('geometryMode')!=0: reject('Audio experiment changed geometry')
            for key in ('noLocalLightDraws','noSpecularDraws','noNormalMapDraws','adaptiveFullLightDraws'):
                if f.get(key)!=0: reject('Audio experiment changed shader features')
            rows=details[(i,f['n'])]
            if len(rows)!=f.get('audioStreams') or len({r.get('channel') for r in rows})!=len(rows):
                reject('Audio channel inventory mismatch')
            eligible=[]
            for r in rows:
                integers=('channel','nameHash','rate','channels','readUs','readBytes','readCalls',
                          'decodeIncludingReadUs','decodedFrames','refillCalls','processedBuffers','uploads',
                          'stoppedWithQueuedBuffers')
                if any(type(r.get(k))!=int or r[k]<0 for k in integers) or any(type(r.get(k))!=bool for k in ('eligible','pcm','suppressed')):
                    reject('Missing/invalid audio stream evidence'); continue
                if r['channel'] not in range(40,53) or r['channels'] not in (1,2) or r['rate']<=0:
                    reject('Invalid audio format/channel')
                if r['eligible']: eligible.append((r['channel'],r['nameHash'],r['rate'],r['channels']))
                suppressed=mode==1 and r['eligible']
                pcm=mode==2 and r['eligible'] and r['nameHash'] in PCM_HASHES
                if r['suppressed']!=suppressed or r['pcm']!=pcm: reject('Wrong audio stream intervention')
                if suppressed and any(r[k] for k in ('readBytes','readCalls','decodedFrames','uploads','refillCalls')):
                    reject('Suppressed stream still performing background work')
                measured.append(r)
            layout=tuple(sorted(eligible))
            if baseline_layout is None: baseline_layout=layout
            elif layout!=baseline_layout: reject('Looped background workload changed between frames/cases')
            for key,flag in (('eligibleStreams','eligible'),('pcmStreams','pcm'),('suppressedStreams','suppressed')):
                if f.get(key)!=sum(r.get(flag) is True for r in rows): reject('Audio aggregate inventory mismatch: '+key)
        streams={}
        for h in sorted({r['nameHash'] for r in measured}):
            rr=[r for r in measured if r['nameHash']==h]
            decoded=sum(r['decodedFrames'] for r in rr)
            decode=sum(r['decodeIncludingReadUs'] for r in rr)
            reads=sum(r['readUs'] for r in rr)
            streams[str(h)]={
                'sampleRate':rr[0]['rate'],'channels':rr[0]['channels'],'eligible':rr[0]['eligible'],
                'meanReadMs':statistics.mean(r['readUs'] for r in rr)/1000,
                'meanDecodeIncludingReadMs':statistics.mean(r['decodeIncludingReadUs'] for r in rr)/1000,
                'totalReadBytes':sum(r['readBytes'] for r in rr),'totalReadCalls':sum(r['readCalls'] for r in rr),
                'decodedFrames':decoded,'totalUploads':sum(r['uploads'] for r in rr),
                'stoppedWithQueuedObservations':sum(r['stoppedWithQueuedBuffers'] for r in rr),
                'decodeExcludingReadUsPerFrame':max(0,decode-reads)/decoded if decoded else None}
        summary[i]={'streams':streams,
                    'exercised':any(r['suppressed'] if mode==1 else r['pcm'] if mode==2 else r['eligible'] for r in measured)}
    return summary
