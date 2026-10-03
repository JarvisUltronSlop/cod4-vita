# Contributing

Start with [README.md](README.md), [BUILDING.md](BUILDING.md) and [the handoff](docs/STATUS.md). Explain the problem, resulting behavior and relevant validation in each pull request. Contribute under the existing GPL terms and retain upstream notices.

Do not commit game files, converted packs/sidecars, retail artwork, Sony modules, vendor SDKs, private captures, credentials or personal installation paths. Use synthetic test fixtures. Converted assets remain copyrighted game data.

Run relevant Python and portable C++ tests with assertions enabled. For memory/loading/renderer changes, exercise failure cleanup and exit/reload. Hardware reports should identify the source/build, map, data set, clocks and actual observations. Distinguish host/emulator checks from physical measurements and sanitize logs before sharing.

The public tree starts with fresh history because previous private backups contain generated/proprietary material. Run `python tools/audit_public_source.py` before committing. Do not push private backup branches or tags here.
