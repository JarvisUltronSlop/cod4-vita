"""Resize an owned PC icon for Vita; generated retail pixels remain local."""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image, __version__ as pillow_version


def prepare(source, output):
    source, output = Path(source).resolve(), Path(output).resolve()
    with Image.open(source) as ico:
        original = ico.ico.getimage(max(ico.ico.sizes(), key=lambda size: size[0]*size[1]))
        image = original.convert('RGBA').resize((128, 128), Image.Resampling.LANCZOS)
        image = image.quantize(colors=256, method=Image.Quantize.FASTOCTREE,
                               dither=Image.Dither.NONE)
        output.parent.mkdir(parents=True, exist_ok=True)
        image.save(output, format='PNG', bits=8, optimize=True)
    with Image.open(output) as icon:
        assert icon.size == (128, 128) and icon.mode == 'P'
        icon.verify()
    provenance = dict(source=str(source), sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                      outputSha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                      dimensions=[128, 128], format='indexed PNG, 8 bits',
                      largestSourceFrame=list(original.size), pillowVersion=pillow_version,
                      conversion='Lanczos resize; FASTOCTREE 256-colour quantization, no dithering')
    output.with_suffix('.json').write_text(json.dumps(provenance, indent=2)+'\n')
    return provenance


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.source, args.output), indent=2))
