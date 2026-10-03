import io
import struct
import unittest
try:
    from PIL import Image
except ImportError:
    Image = None
import half_texture_packs as half
import vertical_slice_packs as packs
import kvap


def record(name, w, h, levels, fmt, payload, flags=0):
    name = name.encode()
    return struct.pack('<8I', len(name), len(payload), w, h, 1, levels, fmt, flags) + name + payload


@unittest.skipIf(Image is None, 'Pillow required; run with the recorded texture-conversion Python')
class HalfTextures(unittest.TestCase):
    def test_authored_mips_and_cube_faces(self):
        for fmt in half.DXT:
            sizes = [packs.mip_bytes(fmt, x, x) for x in (16, 8, 4)]
            for faces in (1, 6):
                payload = b''.join(bytes([f*3+n])*size for f in range(faces) for n,size in enumerate(sizes))
                wire = record('brick', 16, 16, 3, fmt, payload, 4 if faces==6 else 0)
                result, report = half.half_record('brick', wire, set())
                self.assertEqual(report['after'][:3], [8, 8, 2])
                expected = b''.join(bytes([f*3+n])*sizes[n] for f in range(faces) for n in (1, 2))
                self.assertEqual(result[37:], expected)

    def test_ui_and_constants_exact(self):
        for name in ('custom_atlas', 'checkbox_checked', 'gamefonts_pc', 'objective_line', 'menu_background'):
            wire = record(name, 16, 16, 1, 50, bytes(256))
            result, report = half.half_record(name, wire, {'custom_atlas'})
            self.assertEqual(result, wire)
            self.assertEqual(report['method'], 'preserved-ui')
        wire = record('$white', 4, 4, 1, 0x31545844, bytes(8))
        self.assertEqual(half.half_record('$white', wire, set())[0], wire)

    def test_single_lightmap_channels(self):
        # Channel values must not be premultiplied by encoded coefficient A.
        pixels = b''.join(bytes([x*20, y*20, 200, 0]) for y in range(4) for x in range(4))
        result = half.half_single(pixels, 4, 4, 21)
        self.assertEqual(result, bytes([10,10,200,0,50,10,200,0,10,50,200,0,50,50,200,0]))
        self.assertEqual(half.half_single(bytes(range(16)),4,4,50), bytes([3,5,11,13]))

    def test_single_compressed_cube(self):
        payload = b''
        for face in range(6):
            image = Image.new('RGBA', (16,16), (face*40, 0, 0, 255))
            stream = io.BytesIO(); image.save(stream,format='DDS',pixel_format='DXT1')
            payload += stream.getvalue()[128:]
        wire = record('sky',16,16,1,0x31545844,payload,4)
        result, report = half.half_record('sky',wire,set())
        self.assertEqual(report['after'], [8,8,1,192])
        for face in range(6):
            chunk=result[35+face*32:35+(face+1)*32]
            with Image.open(io.BytesIO(half.dds_header(8,8,0x31545844,32)+chunk)) as image:
                r,g,b=image.convert('RGB').getpixel((4,4))
                self.assertLess(abs(r-face*40),10); self.assertEqual((g,b),(0,0))

    def test_invalid_payload_rejected(self):
        wire=record('brick',16,16,2,0x31545844,bytes(4))
        with self.assertRaises(kvap.KvapError):half.half_record('brick',wire,set())
        wire=record('brick',16,16,1,999,bytes(256))
        with self.assertRaises(kvap.KvapError):half.half_record('brick',wire,set())

    def test_bundle_names_and_metadata(self):
        records={}
        for name in ('brick','ui_atlas'):
            wire=record(name,16,16,2,0x31545844,bytes(160))
            records[name]=(wire,160)
        source=packs.build_viti_records(records)
        output, report=half.half_bundle(source,{'ui_atlas'})
        parsed=packs.parse_viti_records(output)
        self.assertEqual(set(parsed),set(records))
        self.assertEqual(parsed['ui_atlas'],records['ui_atlas'])
        self.assertEqual(parsed['brick'][1],32)
        self.assertEqual(len(report),2)


if __name__ == '__main__':
    unittest.main()
