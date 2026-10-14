# Building COD4 Vita

The conversion host is Windows with English Steam COD4 (2007). Install Python 3.11+, Git, CMake 3.25+, Ninja and Visual Studio 2022 Build Tools with Desktop development with C++ and a Windows SDK. Put `python`, `git`, `cmake` and `ninja` on PATH.

## Vita toolchain and console

Install the Windows [VitaSDK](https://vitasdk.org/) under `.tools/vitasdk` in this checkout. Expect `bin/arm-vita-eabi-gcc.exe`, `share/vita.toolchain.cmake`, standard Vita libraries, zlib, math and pthread, plus libSceShaccCg and taiHEN development libraries for VitaGL/VitaShaRK. Toolchain setup is a developer prerequisite; the builder does not install a system SDK. Keep the dependency pins and patches in `kisakcod/dependencies/vita` intact.

Install VitaShell on the console and your separately extracted/decrypted `libshacccg.suprx` at `ur0:data/libshacccg.suprx`, following [VitaGL's prerequisites](https://github.com/Rinnegatamante/vitaGL#prerequisites). This Sony module is never a repository file, dependency download or VPK payload.

## Owned-data build

```powershell
.\prepare-owned-content.ps1 -Cod4Root 'E:\SteamLibrary\steamapps\common\Call of Duty 4' -CheckOnly
.\prepare-owned-content.ps1 -Cod4Root 'E:\SteamLibrary\steamapps\common\Call of Duty 4'
```

The script builds an offline x86 database exporter, loads the selected map against the owned install with an isolated writable home, converts typed sidecars, resolves exact missing IWI images from local IWD archives, prepares audio and validates a coherent common/map KVAP set. The exporter uses OpenAL and disabled cinematics, with no Bink/Miles/Steam SDK link or runtime dependency. Microsoft's pinned [D3DX package](https://www.nuget.org/packages/Microsoft.DXSDK.D3DX/9.29.952.8) supplies local headers/import libraries and the two needed runtime DLLs beside the generated exporter. They remain in ignored local output, outside this source release.

Each invocation creates a new `build/owned-content/<run>` and, after successful conversion, a local `deploy/owned-content/<run>` install set. The Steam install and earlier outputs are not overwritten. The Vita build uses `vita-cpu-candidate`, its pinned graphics patches and an empty application-icon override. Generated stream/layout metadata accompanies the application; retail payloads remain external. This is a development build requiring physical validation.

Add `-ContentOnly` for conversion without a VitaSDK. For experimental Crash/Crossfire, use `-Maps mp_shipment,mp_crash,mp_crossfire`; include Shipment for common frontend content. Larger maps use a schema-2 paging set and are unqualified. `-ExportTimeout 1200` allows a slower host. `-ExporterPath` can reuse an offline exporter built from this tree, not the Steam executable or a legacy exporter.

Failed runs retain logs and reports. Do not install `.pending-*` or `.failed-*` directories. Exporter console logs are under the current run's map directory; build logs are in its root. `-CheckOnly` checks paths/tools, not successful compilation or gameplay.

## Install and controls

Install `COD4-Vita.vpk` with VitaShell. Copy the generated `ux0/data/CODVIT001` folder to `ux0:data/CODVIT001`, keeping `paks`, `stream` and `vertical-slice-manifest.json` together. Never mix pack generations. Select Main → Multiplayer → Free-for-All → Shipment.

L aims, R fires, Cross jumps, Circle crouches/prones, Square reloads/uses, Triangle switches weapons, Start pauses, Select shows the scoreboard, and Start+Select toggles the overlay. Rear touch provides grenades, sprint and melee. Leave a match before copying logs to flush buffered records.

## Source-only builds and tests

`build-vita.ps1` retains the older normal Release/Profile application-only builds. The owned-data builder uses the newer Shipment recovery candidate. Historical benchmark/staging tools depend on private baselines and are not the public installation entry point.

```powershell
python -m pip install pytest
python -m pytest kisakcod/tools/vita_asset_prep/tests
cmake -S kisakcod/tests/vita_portable -B build/portable
cmake --build build/portable --config Debug
ctest --test-dir build/portable -C Debug --output-on-failure
python tools/audit_public_source.py
```

Some integration probes need locally converted data; some historical source-text assertions may be stale. Host parser success does not establish source-VM/gameplay parity or physical performance. See [STATUS.md](docs/STATUS.md).

### World visibility regression test

This harness compiles the current `DrawNativeWorld` function against a recording graphics backend and compares it with the version from `e9ef7f1`. It checks draw commands and visibility for both camera paths, including material changes, clipping boundaries and cache allocation failure. No game assets or VitaSDK are needed.

For a single-configuration generator such as Ninja:

```sh
cmake -S kisakcod/tests/vita_portable -B build/visibility -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/visibility --target vita-world-visibility-0-tests vita-world-visibility-1-tests
ctest --test-dir build/visibility -R vita-world-visibility --output-on-failure
./build/visibility/vita-world-visibility-1-tests --benchmark
```

The benchmark reports microseconds per synthetic frame, the one-time order-build cost and cache bytes. It tests sorted and shuffled material order at three surface counts and two fields of view. Each timing is the median of nine 240-frame camera sweeps, with alternating baseline/candidate runs after warmup. Graphics calls are stubs, so these are host CPU timings, not Vita frame rates. On-device checks still need matching content, graphics settings and clocks.
