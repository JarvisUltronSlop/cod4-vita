# COD4 Vita

An experimental native PS Vita port of the multiplayer side of [KisakCOD](https://github.com/SwagSoftware/KisakCOD), published so others can continue the work. The C++ engine runs an offline local server/client session, with VitaGL graphics and OpenAL audio. A Windows builder reads your own Steam installation of **Call of Duty 4: Modern Warfare (2007)** and converts maps, models, textures, scripts and audio into pointer-free KVAP packages. The application is a **VPK**. Game data stays separate under `ux0:data/CODVIT001`; this repository contains source, conversion tools and tests, without retail assets or proprietary SDKs.

The current focus is ordinary offline Free-for-All on Shipment. Earlier physical-Vita builds reached roughly **10 FPS**; that is a historical result, not a promise for the current build. At publication, work was focused on recovering reliable Shipment loading after package-decoding failures, preserving menu/error-screen resources, checking matching installed packs, and keeping Shipment textures resident instead of paging them from disk. Crash/Crossfire conversion and larger-map paging exist but remain experimental. Latest-build loading, graphics, memory, reload behavior and performance need hardware validation; campaign, online multiplayer and a complete all-map release are not supported. See [the handoff](docs/STATUS.md) for where to continue.

Demo video: https://youtu.be/A08zMfo94Nk?si=SwCiC1klDWkjEYEn

## Requirements

- Your own installed **Steam COD4 (2007)** with English game data. The remaster and other Call of Duty games are incompatible.
- A homebrew-enabled **PS Vita or PS TV**, **VitaShell**, and storage for the application and converted data. Allow hundreds of MiB on the console and several GiB of PC build/conversion scratch space.
- **`libshacccg.suprx`**, extracted/decrypted on your own console and installed separately at `ur0:data/libshacccg.suprx`. Follow [VitaGL's prerequisites](https://github.com/Rinnegatamante/vitaGL#prerequisites). The builder never downloads or supplies this Sony module.
- To build: **Windows**, **PowerShell**, **Python 3.11+**, **Git**, **CMake 3.25+**, **Ninja**, and **Visual Studio 2022 Build Tools** with Desktop development with C++ and the Windows SDK.
- A working **Windows VitaSDK** and required libraries under `.tools/vitasdk`. Scripts build the pinned VitaGL, VitaShaRK and OpenAL dependencies; see [BUILDING.md](BUILDING.md).
- Internet access for open-source dependencies and Microsoft's DirectX D3DX package. Game data is read locally.

## Build from your Steam folder

After installing the toolchains:

```powershell
.\prepare-owned-content.ps1 -Cod4Root 'E:\SteamLibrary\steamapps\common\Call of Duty 4'
```

The default is Shipment. The script builds the offline Windows exporter, generates and validates your local game-data set, then builds a development VPK using the current Vita candidate configuration. Install the resulting VPK with VitaShell and copy the matching `ux0/data/CODVIT001` folder as instructed by the generated `INSTALL.txt`. Keep that output private: converted assets are still game data.

Use `-CheckOnly` for a path/tool preflight or `-ContentOnly` to convert without a Vita toolchain. See [BUILDING.md](BUILDING.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

## License and credits

Engine and port contributions follow the upstream [GNU GPL v3 license](LICENSE); retain its notices. Third-party components keep their own licenses; see [THIRD_PARTY.md](THIRD_PARTY.md). Game assets, Sony modules and vendor SDKs are outside this grant. This independent community project is unaffiliated with Activision, Infinity Ward, Valve or Sony.
