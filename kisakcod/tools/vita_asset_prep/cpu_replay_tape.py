"""Create the shared, timestamped 45-second source-validation input tape."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


def prepare(root):
    root = Path(root).resolve()
    # Authored BUTTON_* constants are extracted from this checkout, not invented.
    import re
    header = (root / 'kisakcod/src/qcommon/msg_mp.h').read_text()
    def button(name):
        match = re.search(r'\b' + name + r'\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)', header)
        if not match:
            # Some source revisions use #define instead of the enum.
            match = re.search(r'#define\s+' + name + r'\s+(0x[0-9a-fA-F]+|[0-9]+)', header)
        if not match:
            shift = re.search(r'#define\s+' + name + r'\s+\(1\s*<<\s*(\d+)\)', header)
            if shift:
                return 1 << int(shift[1])
            raise ValueError('Source button constant unavailable: ' + name)
        return int(match[1], 0)
    buttons = {name: button('BUTTON_' + name) for name in ('ATTACK', 'RELOAD', 'CROUCH', 'JUMP', 'FRAG', 'ADS')}
    records = []
    for index in range(1, 1351):
        at = index * 1000 // 30
        action = bits = pitch = yaw = forward = right = 0
        t = at - 15000  # Fixed neutral connection/spawn warmup; never state-triggered.
        if 0 <= t < 4000:
            forward = 64
            if 2000 <= t < 2100:
                bits |= buttons['JUMP']
        elif 4000 <= t < 8000:
            right, bits = 64, buttons['ATTACK']
        elif 8000 <= t < 12000:
            bits = buttons['ATTACK'] | buttons['ADS']
        elif 12000 <= t < 14000:
            bits = buttons['RELOAD']
        elif 14000 <= t < 18000:
            yaw = 1000
        elif 18000 <= t < 22000:
            forward, bits = -48, buttons['CROUCH']
        elif 22000 <= t < 24000:
            bits, action = buttons['FRAG'], 8 | (2 if t == 22000 else 0)
        elif 24000 <= t:
            bits = buttons['ATTACK']
            if t == 26000:
                action = 1  # one weapon-cycle edge, original source transition
        records.append(struct.pack('<IIhhbbH', at, bits, pitch, yaw, forward, right, action))
    body = b''.join(records)
    data = struct.pack('<8I', 0x314C5052, 1, len(records), 16, 1000, 0x434F4434, 1,
                       zlib.crc32(body)) + body
    output = root / 'build/cpu-preservation/cpu-source-replay.bin'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    provenance = {'format': 'RPL1', 'sha256': hashlib.sha256(data).hexdigest(),
                  'records': len(records), 'durationMs': 45000, 'neutralWarmupMs': 15000,
                  'sourceHz': 30, 'serverHz': 20, 'clockStartMs': 1000,
                  'seed': 0x434F4434, 'map': 'mp_shipment', 'buttons': buttons,
                  'scope': 'Identical source calls and input timestamps; diagnostic virtual time, not a performance measurement.'}
    output.with_suffix('.json').write_text(json.dumps(provenance, indent=2) + '\n')
    return output, provenance


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, default=Path(__file__).resolve().parents[3])
    print(prepare(parser.parse_args().workspace)[0])
