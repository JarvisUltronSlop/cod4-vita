"""Separate original GXM AddEntry blocking from callback body and pre-queue Finish.

The pre-callback interval includes GPU synchronization, pending queue entries,
and scheduling. It is deliberately not labelled GPU execution time.
"""
from collections import defaultdict
from analyze_render_passes import summary


def analyze_display_queue(cpu, generation, begin_us, phases):
    groups=defaultdict(lambda: defaultdict(list))
    relevant=[r for r in cpu if r.get('generation')==generation and r.get('startUs',0)>=begin_us]
    for row in relevant:
        if row.get('name')=='display-probe-drained':continue
        if row.get('name','').startswith('display-') or row.get('name') in ('render-probe-frame','render-probe-swap-us'):
            groups[row.get('sequence')][row['name']].append(row)
    required=('display-submit-id','display-submit-start','display-submit-end','display-submit-finish-us',
              'display-submit-status','display-submit-context','display-probe-config','display-probe-health',
              'display-cb-enter','display-cb-exit','display-cb-setbuf-us','display-cb-vblank-us',
              'display-cb-calls','display-cb-status','display-cb-thread','display-cb-priority',
              'render-probe-frame','render-probe-swap-us')
    frames=defaultdict(list);invalid=[];ids=set()
    for sequence,names in sorted(groups.items(),key=lambda item: -1 if item[0] is None else item[0]):
        if not any(n.startswith('display-submit-') or n.startswith('display-cb-') or n=='render-probe-frame' for n in names):continue
        if any(len(names.get(n,[]))!=1 for n in required):
            invalid.append({'sequence':sequence,'reason':'missing or duplicate display evidence'});continue
        v={n:names[n][0] for n in required}
        a=lambda n:v[n]['value0']
        b=lambda n:v[n]['value1']
        ident=a('display-submit-id');phase=b('display-submit-id')
        id_rows=('display-submit-start','display-submit-end','display-submit-finish-us','display-submit-status',
                 'display-submit-context','display-cb-enter','display-cb-exit','display-cb-setbuf-us',
                 'display-cb-vblank-us','display-cb-status','display-cb-priority')
        start,end,enter,exit=[b(n) for n in ('display-submit-start','display-submit-end','display-cb-enter','display-cb-exit')]
        finish=b('display-submit-finish-us');setbuf=b('display-cb-setbuf-us');vblank=b('display-cb-vblank-us')
        if (ident in ids or ident<=0 or phase not in (1,2,3) or any(a(n)!=ident for n in id_rows)
                or a('display-probe-health')!=1 or b('display-probe-health')!=0
                or b('display-submit-status')!=0 or b('display-cb-status')!=0 or b('display-submit-context')!=1
                or a('render-probe-frame')!=phase or finish+end-start>b('render-probe-swap-us')
                or b('display-probe-config') not in (1,2) or start>end or start>enter or enter>exit
                or finish<0 or (phase!=2 and finish!=0) or (phase==2 and finish==0)
                or setbuf<0 or vblank<0 or setbuf+vblank>exit-enter or a('display-cb-calls')!=1
                or b('display-cb-calls')<0 or (b('display-cb-calls')==0 and vblank!=0)):
            invalid.append({'sequence':sequence,'reason':'invalid display identity, timing, callback or synchronization evidence'});continue
        ids.add(ident)
        frames[phase].append(dict(sequence=sequence,id=ident,addEntryUs=end-start,finishUs=finish,
            submitToCallbackUs=enter-start,callbackUs=exit-enter,setFrameBufUs=setbuf,vblankUs=vblank,
            vblankCalls=b('display-cb-calls'),callbackThread=a('display-cb-thread'),
            affinity=b('display-cb-thread'),priority=b('display-cb-priority'),maxPending=b('display-probe-config')))
    reports=[]
    for phase in (1,2,3):
        kept=frames[phase][2:]
        reports.append(dict(phase=phase,frames=len(frames[phase]),retainedFrames=len(kept),
            **{name:summary([v[name] for v in kept]) for name in ('addEntryUs','finishUs','submitToCallbackUs','callbackUs','setFrameBufUs','vblankUs')},
            vblankCalls=sum(v['vblankCalls'] for v in kept),
            callbackThreads=sorted({v['callbackThread'] for v in kept}),affinityMasks=sorted({v['affinity'] for v in kept}),
            priorities=sorted({v['priority'] for v in kept}),maxPending=sorted({v['maxPending'] for v in kept})))
    drains=[v for v in relevant if v.get('name')=='display-probe-drained']
    complete=not invalid and bool(drains) and all(v.get('value0')==0 and v.get('value1')==0 for v in drains)
    complete=complete and all(report['frames']==reference['frames'] and report['retainedFrames']>=3 for report,reference in zip(reports,phases))
    return dict(captureComplete=bool(complete),phases=reports,invalidFrames=invalid,
        interpretation='AddEntry is timed inside the old queue timer. Phase 2 first calls sceGxmFinish on the existing context after EndScene/PadHeartbeat; the old queue timer includes both waits. Callback body, framebuffer update and VBlank are measured separately. Submit-to-callback includes pending queue/GPU synchronization and scheduling, not isolated GPU time. Diagnostic waiting is not an optimization.')
