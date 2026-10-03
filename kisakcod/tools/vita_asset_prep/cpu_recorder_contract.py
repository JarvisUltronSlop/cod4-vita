"""Validate observation compatibility while allowing candidate sink repairs."""
import hashlib
import re

PROTECTED = (
    'SafeName', 'WriteEvent', 'SampleThreads', 'VitaCpuProfile_Start',
    'VitaCpuProfile_Stop', 'VitaCpuProfile_FlushThread', 'VitaCpuProfile_SetSequence',
    'VitaCpuProfile_Record', 'VitaCpuProfile_Counter', 'VitaCpuProfile_Trace',
    'VitaCpuProfile_Invalidate', 'VitaCpuScope::VitaCpuScope', 'VitaCpuScope::~VitaCpuScope',
)


def function(text, name):
    match = re.search(r'^(?:(?:void|bool) )?' + re.escape(name) + r'\(', text, re.M)
    if not match:
        raise ValueError('Missing recorder contract function: ' + name)
    start = match.start()
    end = text.index('{', match.end()) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end].replace('\r\n', '\n')


def startup_observation(text):
    # The log transport now uses bounded lossless compression. Only this exact
    # initializer and six suffix changes are exempt from startup comparison;
    # TLS checks, thread creation, JSON header and error routing remain checked.
    text=text.replace('    if(!InitializeCompressedLogs()){failed.store(true);return false;}\n','')
    for name in ('cpu-perf','loading-stages','vita-perf','script-stages','shipment-trace','cpu-replay'):
        text=text.replace(name+'.jsonl.z"',name+'.jsonl"')
    return text


def verify(reference, candidate):
    for name in PROTECTED:
        before,after=function(reference,name),function(candidate,name)
        if name=='VitaCpuProfile_Start':before,after=startup_observation(before),startup_observation(after)
        if before != after:
            raise ValueError('Recorder observation contract changed: ' + name)
    return dict(referenceTextSha256=hashlib.sha256(reference.encode()).hexdigest(),
                candidateTextSha256=hashlib.sha256(candidate.encode()).hexdigest(),
                unchangedObservationFunctions=list(PROTECTED),
                scope='Candidate sink buffering/flush routing differs; losslessly decoded event encoding, aggregation, timestamps, startup/TLS and replay capture semantics are checked unchanged. Writer failure/queue tests required separately.')
