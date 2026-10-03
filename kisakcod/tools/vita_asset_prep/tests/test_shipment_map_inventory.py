from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from shipment_map_inventory import extract_references


def main() -> None:
    payload = (
        b"mc/mtl_cargocontainer\0"
        b"mc/ch_concretewall09\0"
        b"maps/mp/mp_shipment.d3dbsp\0"
        b"weapons/m4\0"
        b"sound/weapons/m4_fire.wav\0"
        b"$victorybackdrop\0"
        b"$BAD!\0"
        b"aA/P\0"
        b"mc/mtl_cargocontainer\0"
    )
    records = extract_references(payload)
    assert records == [
        {"kind": "image", "name": "$victorybackdrop"},
        {"kind": "material", "name": "mc/ch_concretewall09"},
        {"kind": "material", "name": "mc/mtl_cargocontainer"},
        {"kind": "sound", "name": "sound/weapons/m4_fire.wav"},
        {"kind": "weapon", "name": "weapons/m4"},
        {"kind": "world", "name": "maps/mp/mp_shipment.d3dbsp"},
    ]
    print("shipment map inventory tests: ok")


if __name__ == "__main__":
    main()
