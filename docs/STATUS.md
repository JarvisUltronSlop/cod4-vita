# Handoff at publication — 2026-10-03

This experimental offline multiplayer port was working on Shipment recovery (development run 91). Physical loading, visuals, recovery and FPS for that candidate were unverified at handoff. Earlier builds reached roughly 10 FPS on physical hardware; that result does not qualify the current source.

## How the pieces fit

- `kisakcod/src/vita/vita_bootstrap.cpp`: frontend/loading, typed package gates and presentation.
- `vita_source_session.cpp`, `vita_source_registry.cpp`, `vita_script_services.cpp`: database, local server/client and script services on Vita.
- `kisakcod/src/gfx_vita`: VitaGL rendering. Current candidate: 720×408 Balanced world, 960×544 weapon/HUD, Simple world lighting and stock clocks.
- KVAP readers: fixed-width pointer-free records and chunk CRCs. Common and selected map packs must match. Audio remains external.
- `kisakcod/tools/vita_asset_prep`: conversion, image closure, model/animation/audio packing, dependency builds and verification. `tools/prepare_owned_content.py` joins them for the public workflow.
- `kisakcod/tests/vita_portable` and Python tests: parser, allocation, renderer policy and lifetime coverage.

## Latest work and limitations

Recent physical captures failed typed package decoding before source gameplay, rejecting environment, primary lights, FX, HUD images and models despite successful host reads of retained packages. The hardware read/checksum-versus-parser cause remained unresolved.

Recovery adds exact installed size/header/TOC checks, failed-read/CRC/header records and readable errors. Shared menu/font/button textures survive teardown, and loading joins gameplay automatically when ready. Shipment keeps textures resident; larger-map paging stays experimental. The private build's retail-derived application icon is excluded from public source and the public builder.

Crash/Crossfire have conversion and host readback work but hit CPU staging/registry capacity and load-time problems. Bounded texture indexes, shared immutable model indices, texture leases/backing storage and pressure recovery were introduced. A catalog entry does not establish playability. Campaign, online multiplayer, complete parity, soak testing and all-map qualification remain open.

## Continue here

1. Rebuild/convert from a fresh public checkout and owned English Steam install. Confirm the exporter produces a matching runtime-ready Shipment set without private retained packs.
2. Test ordinary Shipment load/play/exit/reload on Vita. Locate the first failed gate with read/CRC/header diagnostics; host success alone does not resolve hardware failures.
3. Measure completed render intervals, source update rates, load time and memory peaks. The overlay counts completed application frames including waits, not simulation ticks or LCD scans.
4. Once Shipment is stable, profile rendering/waits and validate larger-map paging/recovery. Preserve coherent pack generations and distinguish memory estimates from measurements.

Historical benchmark tools are retained for their implementation logic; their private reference captures and archives are excluded. Use the owned-data builder for public installs. No 30-FPS, full-map or current-hardware qualification is claimed.
