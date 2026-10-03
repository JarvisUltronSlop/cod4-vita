# Licenses and provenance

Based on [SwagSoftware/KisakCOD](https://github.com/SwagSoftware/KisakCOD), imported at `1c03702cbe176e9274e486d295edcd035b3c2b5f`, with local Vita/runtime/exporter changes. Its GPL v3 license is retained at the root and `kisakcod/LICENSE`; preserve existing copyright/attribution comments.

| Component | Location/source | License/notice |
| --- | --- | --- |
| KisakCOD and Vita additions | Engine, tools and build scripts | GPL v3, except identified third-party files |
| ODE | `kisakcod/deps/ode`, `src/physics/ode` | Existing LGPL/BSD dual-license notices; BSD text in `kisakcod/deps/ode/LICENSE-BSD.TXT` |
| zlib | `kisakcod/deps/zlib` | License in source headers |
| Speex | `kisakcod/deps/speex` | BSD notices in headers |
| dr_wav/dr_mp3 | `kisakcod/deps/dr_libs` | Public-domain/MIT-0 alternatives in each header |
| VitaGL | Pinned Rinnegatamante/vitaGL and patches | Upstream GPL/LGPL notices in downloaded source |
| VitaShaRK | Pinned Rinnegatamante/vitaShaRK | Upstream notices in downloaded source |
| OpenAL Soft | Pinned kcat/openal-soft and patches | Upstream LGPL notices in downloaded source |
| Tracy | Windows exporter dependency | Upstream BSD notices in downloaded source |
| Microsoft D3DX | Official local NuGet package | Microsoft package license; not vendored here |

Keep open-source dependency source, patches and license texts when distributing linked binaries; follow their source/relinking obligations. The source-ownership audit checks engine linkage, not copyright clearance.

Upstream credits include the original COD4 developers, [PJayB/jk3src](https://github.com/PJayB/jk3src), [voron00/CoD2rev_Server](https://github.com/voron00/CoD2rev_Server), [shiversoftdev/BO3Enhanced](https://github.com/shiversoftdev/BO3Enhanced), ODE and RAD Game Tools. See upstream for development history.

Excluded: retail game archives/executables/artwork/audio/video/scripts and converted payloads; Bink/Miles/Steam SDK headers/binaries; Sony `libshacccg.suprx`; game icons, compiled shader fixtures, existing VPKs/ELFs, private captures and old Git history. An owned installation does not grant permission to redistribute its files. Call of Duty content/names remain their owners' property; the GPL does not cover them.
