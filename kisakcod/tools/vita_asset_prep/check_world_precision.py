"""Host rounding envelope, not a Vita compiler/visual or performance proof.

Compare a double-precision scalar oracle with deliberately conservative binary16
rounding after every half multiply/add. Hardware can fuse these operations. All
normal/specular math stays full precision; secondary atlas samples round first.
"""
import argparse
import json
import math
import random
import struct
from pathlib import Path


def half(value):
    return struct.unpack('<e', struct.pack('<e', value))[0]


def check(cases=200000):
    rng = random.Random(0x70C0D4)
    errors = []
    for i in range(cases):
        # Full authored range including encoded secondary directions at edges.
        top = [rng.randrange(256) / 255 for _ in range(4)]
        bottom = [rng.randrange(256) / 255 for _ in range(4)]
        if i < 256:
            top[3] = i / 255
            bottom[3] = (255-i) / 255
        x, y = rng.uniform(-2.08, 2), rng.uniform(-2.06451607, 2)
        ni = 1 / math.sqrt(x*x + y*y + 1)

        def alignment(a, b):
            sx, sy = a * 4.07999992 - 2.07999992, b * 4.06451607 - 2.06451607
            return max(0, min(1, (sx*x + sy*y + 1) * ni / math.sqrt(sx*sx + sy*sy + 1)))

        al = alignment(top[3], bottom[3])
        ht, hb = list(map(half, top)), list(map(half, bottom))
        hal = alignment(ht[3], hb[3])
        primary, sun_dot, shadow = rng.random(), rng.random(), rng.random()
        fog = rng.random()
        for c in range(3):
            sun = rng.uniform(0, 4)
            material, highlight, environment, fog_color = [rng.random() for _ in range(4)]
            env_factor = rng.random() * .2
            spec = highlight + environment * env_factor
            static = min(1.35, top[c]*ni + bottom[c]*al + primary*sun*sun_dot*shadow)
            oracle = (material + spec) * static
            oracle = oracle * (1-fog) + fog_color*fog
            # All half colour stores in the actual dual-lightmap path.
            lit = half(material)
            lit = half(lit + highlight)
            lit = half(lit + half(environment)*env_factor)
            secondary = half(half(ht[c]*half(ni)) + half(hb[c]*half(hal)))
            direct = half(half(half(half(primary)*half(sun))*half(sun_dot))*half(shadow))
            hs = min(half(1.35), half(secondary + direct))
            lit = half(lit * hs)
            candidate = half(lit * (1-fog) + fog_color*fog)
            assert math.isfinite(candidate)
            errors.append(abs(candidate-oracle))
    errors.sort()
    maximum = errors[-1]
    assert maximum < 2/255, maximum
    return dict(cases=cases, channels=len(errors), p95AbsoluteError=errors[int(len(errors)*.95)],
                p99AbsoluteError=errors[int(len(errors)*.99)], maxAbsoluteError=maximum,
                maxIn8BitColourSteps=maximum*255,
                testedInputs='UNORM8 secondary atlas; normal perturbations [-2.08,2]; sun [0,4]; colour/specular/fog [0,1]',
                bound=2/255, compilerOrVisualQualification=False,
                limitations='Synthetic scalar colour rounding envelope; not target execution, texture filtering, all authored inputs, sharp specular precision, or postprocess amplification. Full precision specular and original alpha are unchanged in the generated shader, checked separately.')


if __name__ == '__main__':
    p=argparse.ArgumentParser();p.add_argument('--output', type=Path);p.add_argument('--cases',type=int,default=200000);args=p.parse_args()
    result=check(args.cases);text=json.dumps(result,indent=2)+'\n'
    if args.output:args.output.write_text(text,encoding='utf-8')
    print(text)
