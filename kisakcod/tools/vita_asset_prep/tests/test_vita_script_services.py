#!/usr/bin/env python3
"""Regression checks for the bounded package-owned GSC rawfile mount."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src" / "vita" / "vita_script_services.cpp"
HEADER = ROOT / "src" / "vita" / "vita_script_services.h"


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    assert "VitaScriptServicesBeginRawFiles" in header
    assert "kMaxMountedRawFiles = 1024" in source
    assert "kMaxOpenRawFiles = 32" in source
    assert "RawPathEqual" in source
    assert "a == '\\\\'" in source
    assert "FindMountedRawFile" in source
    assert "DB_FindXAssetHeader" in source
    assert "result{}" in source
    assert "FS_FOpenFileRead" in source
    assert "open.offset = 0" in source
    assert "open->offset += count" in source
    assert "FS_FCloseFile" in source
    assert "FS_ReadFile" in source
    assert "copy[record->length] = '\\0'" in source
    assert "char* ownedBytes = nullptr" in source
    assert "std::malloc(length + 1u)" in source
    assert "ownedBytes[length] = '\\0'" in source
    assert "std::free(file.ownedBytes)" in source
    assert "CL_GetLocalClientActiveCount() { return 1; }" in source
    assert "VitaScriptServicesClearRawFile" in source
    assert "VitaScriptServicesSetTraceAppId" in header
    assert "script-stages.jsonl" in source
    assert "WriteTraceJsonString" in source
    budget = (ROOT / "src/vita/vita_trace_budget.h").read_text(encoding="utf-8")
    assert "kRegular=4096, kNoisy=128, kLifecycle=512, kFailure=64" in budget
    assert "g_traceBudget.Admit(stage, subject, IsNoisyTraceStage(stage))" in source
    assert "g_traceBudget = {}" in source
    assert "IsNoisyTraceStage" in source
    assert "g_traceFile" in source
    assert 'std::fopen(tracePath, "wb")' in source
    assert "std::remove(tracePath)" in source
    assert "Each process owns its trace" in source
    assert "Recompute it from the new segment" in source
    assert "aligned = AlignUp(static_cast<uintptr_t>(current->pos)" in source

    lowlevel = (ROOT / "src/vita/vita_lowlevel_services.cpp").read_text(encoding="utf-8")
    assert "EnsureThreadContext" in lowlevel
    assert "Com_InitThreadData(0)" in lowlevel
    assert "g_threadContextInitialized" in lowlevel

    vm = (ROOT / "src/script/scr_vm.cpp").read_text(encoding="utf-8")
    assert "objectType = static_cast<Vartype_t>(GetObjectType(objectId))" in vm
    assert "const char* objectTypeName = var_typename[objectType]" in vm

    stringlist = (ROOT / "src/script/scr_stringlist.cpp").read_text(encoding="utf-8")
    assert "vita_script_services.h" in stringlist
    assert 'VITA_SL_TRACE("sl_mt_alloc_begin"' in stringlist
    assert 'VITA_SL_TRACE("sl_copy_begin"' in stringlist

    memorytree = (ROOT / "src/script/scr_memorytree.cpp").read_text(encoding="utf-8")
    assert "VitaMTFreeAdd" in memorytree
    assert "vitaMtFreeOrder" in memorytree
    assert "deterministic buddy free-list" in memorytree
    print("vita script services tests: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
