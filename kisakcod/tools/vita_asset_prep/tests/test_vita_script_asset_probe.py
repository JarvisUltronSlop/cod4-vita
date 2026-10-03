#!/usr/bin/env python3
"""Verify the renderer-free native KVAP/GSC qualification probe wiring."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src" / "vita" / "vita_script_asset_probe.cpp"
CMAKE = ROOT / "scripts" / "platform" / "vita" / "CMakeLists.txt"
LEXER = ROOT / "src" / "script" / "scr_yacc2.cpp"
LEXER_HEADER = ROOT / "src" / "script" / "scr_yacc.h"


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    lexer = LEXER.read_text(encoding="utf-8")
    lexer_header = LEXER_HEADER.read_text(encoding="utf-8")

    assert "VitaAssetDatabase" in source
    assert "VitaRawFileBundle" in source
    assert "VitaScriptRuntime" in source
    assert "DeathmatchRulesLoaded" in source
    assert '"[script-asset-probe] dm_rules=' in source
    assert "VitaScriptVm_LoadShipment" in source
    assert "VitaScriptVm_InitializeEntityRuntime" in source
    assert "script-vm.jsonl" in (ROOT / "src" / "vita" / "vita_script_vm_runtime.cpp").read_text(encoding="utf-8")
    assert "entityReady" in source
    assert "app0:data/shipment-db-reference.kvap" in source
    assert "ux0:data/CODVIT002/assets/shipment-db-reference.kvap" in source
    assert "ux0:data/CODVIT001/assets/shipment-db-reference.kvap" in source
    assert '"[script-asset-probe] missing=%s\\n"' in source
    assert "[script-asset-probe]" in source
    # This target is intentionally independent of the visual/audio frame loop.
    for forbidden in ("vitaGL", "sceAudioOut", "Scr_IncTime", "VitaFrontend"):
        assert forbidden not in source

    assert "KisakCOD-vita-script-asset-probe EXCLUDE_FROM_ALL" in cmake
    assert "KisakCOD-vita-script-asset-probe.self" in cmake
    assert "CODVIT002" in cmake
    assert "KISAK_VITA_SCRIPT_ASSET_PROBE=1" in cmake
    assert '"data/shipment-db-reference.kvap"' in cmake
    assert "KisakCOD-vita-script-asset-probe.vpk-vpk" in cmake
    assert "KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME=1" in cmake
    # Keep the ARM scanner's text/buffer pointer model explicit.  The source
    # service still consumes unsigned bytes, but that conversion must be a
    # narrow boundary rather than an implicit -fpermissive conversion through
    # the lexer state machine.
    assert "int StringValue(char *str, int len)" in lexer
    assert "int __cdecl StringValue(char *str, int len);" in lexer_header
    assert "decltype(yytext) source;" in lexer
    assert "decltype(yy_current_buffer->yy_ch_buf) dest;" in lexer
    assert "reinterpret_cast<unsigned char*>(&((yy_current_buffer)->yy_ch_buf[number_to_move]))" in lexer
    assert "(unsigned char *)yy_flex_realloc" not in lexer
    # Large retail scripts can grow the recovered parser stack.  Vita must
    # relocate that stack off the small thread stack and release each prior
    # heap generation after copying it.
    assert "std::malloc(sizeof(short) * static_cast<std::size_t>(yystacksize))" in lexer
    assert "std::malloc(sizeof(stype_t) * static_cast<std::size_t>(yystacksize))" in lexer
    assert "std::free(free1addr)" in lexer
    assert "std::free(free2addr)" in lexer
    assert "g_vitaLexCount" in lexer
    assert "g_vitaParseSteps" in lexer
    assert 'VitaScriptServicesTrace("lexer_progress"' in lexer
    assert 'VitaScriptServicesTrace("parser_progress"' in lexer
    print("vita script asset probe tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
