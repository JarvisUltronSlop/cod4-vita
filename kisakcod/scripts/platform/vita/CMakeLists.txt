cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
    else()
        message(FATAL_ERROR "KISAK_PLATFORM=vita requires VITASDK or CMAKE_TOOLCHAIN_FILE")
    endif()
endif()

if(NOT DEFINED VITASDK)
    if(DEFINED ENV{VITASDK})
        set(VITASDK "$ENV{VITASDK}")
    else()
        message(FATAL_ERROR "VITASDK is not configured")
    endif()
endif()

include("${VITASDK}/share/vita.cmake" REQUIRED)
option(KISAK_VITA_CPU_PROFILE "CPU/loading reference instrumentation" OFF)
option(KISAK_VITA_CPU_RUNTIME "Independent CPU runtime candidate" OFF)
if(KISAK_VITA_CPU_RUNTIME AND NOT KISAK_VITA_CPU_PROFILE)
    message(FATAL_ERROR "CPU runtime requires its qualification instrumentation")
endif()
add_compile_definitions(KISAK_VITA_CPU_PROFILE=$<BOOL:${KISAK_VITA_CPU_PROFILE}>
    KISAK_VITA_CPU_RUNTIME=$<BOOL:${KISAK_VITA_CPU_RUNTIME}>)
option(KISAK_VITA_BENCHMARK "Enable explicitly selected hardware benchmark rounds" OFF)
set(KISAK_VITA_BENCHMARK_ROUND "1" CACHE STRING "Diagnostic round: 1 resolution, 2 shading, 3 material features, 4 normal/dispatch, 5 geometry, 6 audio, 7 textures, 8 debris/fog, 9 combined balanced")
if(NOT KISAK_VITA_BENCHMARK_ROUND MATCHES "^[123456789]$")
    message(FATAL_ERROR "KISAK_VITA_BENCHMARK_ROUND must be 1, 2, 3, 4, 5, 6, 7, 8 or 9")
endif()
option(KISAK_VITA_BALANCED_CANDIDATE "Playable balanced renderer with optional comparison" OFF)
add_compile_definitions(KISAK_VITA_BALANCED_CANDIDATE=$<BOOL:${KISAK_VITA_BALANCED_CANDIDATE}>)
add_compile_definitions(KISAK_VITA_BENCHMARK=$<BOOL:${KISAK_VITA_BENCHMARK}>)

# Release and profile use the pinned, no-splash dependency recipe. The
# override also permits focused diagnostic builds without editing the SDK.
set(KISAK_VITA_VITAGL_LIBRARY "" CACHE FILEPATH
    "Pinned VitaGL archive from tools/vita_asset_prep/build_vitagl.py")
if(KISAK_VITA_VITAGL_LIBRARY)
    if(NOT EXISTS "${KISAK_VITA_VITAGL_LIBRARY}")
        message(FATAL_ERROR
            "KISAK_VITA_VITAGL_LIBRARY does not exist: ${KISAK_VITA_VITAGL_LIBRARY}")
    endif()
    add_library(vitaGL STATIC IMPORTED GLOBAL)
    set_target_properties(vitaGL PROPERTIES
        IMPORTED_LOCATION "${KISAK_VITA_VITAGL_LIBRARY}")
    get_filename_component(KISAK_VITA_VITAGL_DIRECTORY "${KISAK_VITA_VITAGL_LIBRARY}" DIRECTORY)
    if(EXISTS "${KISAK_VITA_VITAGL_DIRECTORY}/include/vitaGL.h")
        set_target_properties(vitaGL PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
            "${KISAK_VITA_VITAGL_DIRECTORY}/include")
    endif()
endif()
set(KISAK_VITA_VITASHARK_LIBRARY "" CACHE FILEPATH "Pinned VitaShaRK archive paired with VitaGL")
if(KISAK_VITA_VITASHARK_LIBRARY)
    if(NOT EXISTS "${KISAK_VITA_VITASHARK_LIBRARY}")
        message(FATAL_ERROR "Pinned VitaShaRK archive is missing; run build_vitagl.py")
    endif()
    add_library(vitashark STATIC IMPORTED GLOBAL)
    get_filename_component(KISAK_VITA_VITASHARK_DIRECTORY "${KISAK_VITA_VITASHARK_LIBRARY}" DIRECTORY)
    set_target_properties(vitashark PROPERTIES IMPORTED_LOCATION "${KISAK_VITA_VITASHARK_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${KISAK_VITA_VITASHARK_DIRECTORY}/include")
endif()

set(KISAK_VITA_OPENAL_LIBRARY "${CMAKE_CURRENT_LIST_DIR}/../../../../build/vita/openal-soft/libopenal.a"
    CACHE FILEPATH "Pinned OpenAL Soft Vita backend")
get_filename_component(KISAK_VITA_OPENAL_DIRECTORY "${KISAK_VITA_OPENAL_LIBRARY}" DIRECTORY)
add_library(vitaOpenAL STATIC IMPORTED GLOBAL)
set_target_properties(vitaOpenAL PROPERTIES IMPORTED_LOCATION "${KISAK_VITA_OPENAL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${KISAK_VITA_OPENAL_DIRECTORY}/include"
    INTERFACE_LINK_LIBRARIES "SceAudio_stub;SceAudioIn_stub;SceLibKernel_stub;pthread")

set(VITA_APP_NAME "KisakCOD Vita")
set(VITA_TITLEID "CODVIT001")
set(VITA_VERSION "01.00")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra")

# The recovered Windows source intentionally aliases packed words/floats and
# uses two's-complement integer wrap (for example in source random seeds).
# Preserve those semantics under GCC optimization, without fast-math in any
# gameplay, snapshot, script or Pmove translation unit.
add_compile_options(-fno-strict-aliasing -fwrapv)

# Prototype-only native callback experiment. The normal source runtime owns
# the GSC/entity lifecycle and does not use this bridge.
option(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME
       "Enable experimental native GSC entity callback dispatch" OFF)

# Test-only route helper.  It drives the same frontend actions as Cross on
# Main -> Multiplayer -> Free-for-All -> Shipment, then lets the normal
# loading transition reach InGame.  Keep this disabled for shipping packages;
# it exists so the renderer can be captured deterministically when emulator
# keyboard input is unavailable.
option(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT
       "Autostart the Shipment route for deterministic emulator smoke captures" OFF)
option(KISAK_VITA_CLEAN_VIEWMODEL
       "Isolated source viewmodel renderer bring-up (smoke target only)" OFF)
if(KISAK_VITA_CLEAN_VIEWMODEL AND NOT KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)
    message(FATAL_ERROR "Clean viewmodel bring-up requires the explicit smoke target")
endif()

# Test-only gameplay qualification mode. It runs the same frontend, local
# snapshot bridge, source Pmove/PM_Weapon path, event delivery, and trace
# writer as the rendered slice, but skips the expensive display pass. This
# gives the emulator a fast, deterministic movement/action trace while the
# scalar renderer remains the visual oracle.
option(KISAK_VITA_SMOKE_GAMEPLAY_ONLY
       "Run the deterministic Shipment gameplay route without rasterizing" OFF)

# Test-only authored FX validation.  Inject one source grenade-flash event at
# the first deterministic gameplay frame so the scalar capture can validate
# the transient VFXA runner/light handoff without depending on host input.
option(KISAK_VITA_SMOKE_AUTHORED_FX_EVENT
       "Inject one source-authored grenade FX event for smoke capture" OFF)

# Test-only frontend capture mode.  It leaves the authored Main menu on
# screen with a neutral input sample so Vita3K menu screenshots cannot be
# advanced accidentally by a stale host-controller edge.  Shipping targets
# keep this disabled and continue to poll the physical Vita controls.
option(KISAK_VITA_MENU_ONLY
       "Keep the frontend on the authored Main menu for deterministic captures" OFF)

# Optional test-only route selector for menu screenshots. Values follow
# VitaFrontendScreen (0=Main, 1=Multiplayer, 2=CustomClasses, 3=ClassEditor,
# 4=ClassPicker, 5=GameModeSelect, 6=MapSelect, 7=Options, 8=Controls,
# 9=ControlsLook, 10=Loading, 11=Pause, 12=InGame). Empty keeps normal input.
set(KISAK_VITA_MENU_SCREEN "" CACHE STRING
    "Select an authored frontend route for a diagnostic capture")

# Optional test-only map selector for deterministic full-route captures. The
# normal build starts on Shipment and the authored Map Select route remains
# responsible for changing maps at runtime; this selector only chooses the
# map used by the smoke/autostart qualification build.
set(KISAK_VITA_SMOKE_MAP_INDEX "0" CACHE STRING
    "Map index for a deterministic Vita smoke capture (0=Shipment, 2=Crossfire)")

# The production Vita/Vita3K qualification configuration must exercise the
# same vitaGL/GXM renderer that will run on hardware.  The scalar display
# renderer remains useful for visual diagnostics, but it is deliberately
# opt-in so an emulator capture can never be mistaken for an accelerated
# vertical-slice result.
option(KISAK_VITA_ALLOW_SOFTWARE_FALLBACK
       "Allow the diagnostic scalar renderer when libshacccg.suprx is absent" OFF)

# Shipping installs keep the executable VPK small and mount locally generated
# COD4 data from ux0:data/CODVIT001/paks.  Embedding the legacy multi-map KVAP
# remains opt-in for old capture harnesses only.
option(KISAK_VITA_EXTERNAL_CONTENT
       "Load locally generated data packs from ux0 instead of embedding retail assets" ON)

# Enables bounded on-device performance collection without changing renderer
# or gameplay ownership.  The profile preset leaves optimization enabled.
option(KISAK_VITA_PROFILE
       "Enable Vita vertical-slice frame and memory telemetry" OFF)
if(KISAK_VITA_PROFILE)
    # Retain the source invariants with optimized code. Without this, the
    # diagnostic build can write through an invalid source object before a
    # later explicit assertion reports an unrelated symptom.
    add_compile_definitions(RELEASE_ASSERTS=1)
endif()

# Vita3K 0.2.x can fault in its host audio shim when a native-GXM guest opens
# the main output port.  Keep this an explicit qualification-build switch:
# physical release/profile builds must exercise the real Vita audio service.
option(KISAK_VITA_VITA3K_NO_AUDIO
       "Skip sceAudioOut initialization in native Vita3K qualification builds" OFF)

# Native release/profile packages run the recovered KisakCOD MP server,
# client, snapshot, script, and gameplay roots. The bounded Shipment runtime
# remains available only to the scalar oracle and focused regression targets.
option(KISAK_VITA_SOURCE_RUNTIME
       "Run the source-owned KisakCOD offline MP session" ON)

# Test-only scalar capture fallback. The normal runner uses capture.req, but a
# few Vita3K builds do not expose an installed-title request file to the guest
# even though the display buffer is valid. Enabling this option captures one
# completed scalar frame after a short warm-up; it never affects the
# accelerated or shipping path.
option(KISAK_VITA_CAPTURE_SOFTWARE_FRAME
       "Capture one warmed-up scalar frame without a guest request file" OFF)

# Diagnostic-only scalar comparison. This exposes the packed scene before the
# final film/glow/gamma stage so an emulator capture can distinguish authored
# post-processing from a world/material submission defect. Keep it disabled
# for production and accelerated builds.
option(KISAK_VITA_DEBUG_NO_FRAME_EFFECTS
       "Disable scalar film/glow/gamma for renderer diagnostics only" OFF)

# Test-only overlay capture.  It uses the authored MP5/viewmodel/HUD passes
# while bypassing the expensive scalar world projection.  This is useful for
# an emulator screenshot when a Vita3K build faults inside the guest
# projection loop; it never changes the normal accelerated or scalar route.
option(KISAK_VITA_CAPTURE_OVERLAY_ONLY
       "Capture the authored gameplay overlay without scalar world projection" OFF)

# Test-only memory profile for large authored maps under Vita3K's scalar
# compatibility renderer.  It keeps the selected world/collision/entities and
# HUD path intact while omitting optional static-model texture uploads that can
# exhaust the emulator guest heap before the first frame.
option(KISAK_VITA_SMOKE_LITE_ASSETS
       "Use the bounded large-map asset profile for scalar smoke captures" OFF)

# Diagnostic-only native frame probe.  It keeps the complete authored BSP,
# lightmap, shader, sky, and post-processing path intact while deferring the
# optional XModel color/semantic residency pass.  This isolates first-frame
# GXM scene lifecycle failures from Vita3K's known heap corruption during the
# second native model upload batch; the shipping native build leaves it off.
option(KISAK_VITA_NATIVE_FRAME_PROBE
       "Defer optional native model residency while qualifying the first frame" OFF)

# Diagnostic-only display target probe.  The shipping native path retains the
# authored HDR scene target and post pass; this switch sends one qualification
# build directly to vitaGL's display framebuffer so a GXM scene-start failure
# can be separated from FBO/post-processing state.
option(KISAK_VITA_NATIVE_DIRECT_DISPLAY_PROBE
       "Use the vitaGL display framebuffer while qualifying native scene startup" OFF)

# Diagnostic-only clear probe.  vitaGL's clear helper is itself a shader-backed
# GXM draw; bypassing it for one qualification build tells us whether the
# failure is in that helper or in ordinary native geometry submission.
option(KISAK_VITA_NATIVE_SKIP_CLEAR_PROBE
       "Skip the native clear helper while qualifying GXM geometry submission" OFF)

# Diagnostic-only overlay probe. The native viewmodel draw remains in the
# shipping path, but can be bypassed while qualifying HUD/end-frame capture
# independently from a Vita3K shader or vertex-stream fault in that model.
option(KISAK_VITA_NATIVE_SKIP_VIEWMODEL_PROBE
       "Skip the native viewmodel while qualifying HUD frame capture" OFF)

# Diagnostic-only model residency probe.  The native frame probe already
# defers the complete semantic model bundle, but the small common color
# companion is staged before that gate.  Skipping it lets a world-only capture
# qualify BSP/lightmap/sky submission without entering Vita3K's large DXT
# model-upload path.  The shipping native path leaves this off.
option(KISAK_VITA_NATIVE_SKIP_MODEL_COLOR_PROBE
       "Skip the native model color companion while qualifying world rendering" OFF)

# Diagnostic-only texture upload probe.  It decodes small source DXT images to
# RGBA before the VitaGL upload, allowing Vita3K validation faults in the
# compressed transfer path to be separated from native scene submission.  The
# shipping native path keeps the authored compressed upload unchanged.
option(KISAK_VITA_NATIVE_CPU_TEXTURE_UPLOAD
       "Decode small native DXT images to RGBA for Vita3K upload diagnostics" OFF)

# Diagnostic-only native projection bound.  A nonzero value samples a bounded
# source triangle set and streams it through the same native material pass so
# GXM scene/post validation can finish on Vita3K before the full source
# lightgrid/shadow walk is qualified.  Empty/zero keeps the complete source
# projection used by the shipping renderer.
set(KISAK_VITA_NATIVE_TRIANGLE_BUDGET "" CACHE STRING
    "Bound native projection triangles for a diagnostic capture")

if(KISAK_VITA_BENCHMARK AND (NOT KISAK_VITA_SOURCE_RUNTIME
   OR KISAK_VITA_ALLOW_SOFTWARE_FALLBACK OR KISAK_VITA_PROFILE
   OR KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT OR KISAK_VITA_NATIVE_DIRECT_DISPLAY_PROBE
   OR KISAK_VITA_NATIVE_SKIP_VIEWMODEL_PROBE OR KISAK_VITA_NATIVE_SKIP_CLEAR_PROBE
   OR KISAK_VITA_NATIVE_SKIP_MODEL_COLOR_PROBE OR KISAK_VITA_DEBUG_NO_FRAME_EFFECTS
   OR KISAK_VITA_MENU_ONLY OR KISAK_VITA_CAPTURE_OVERLAY_ONLY
   OR KISAK_VITA_SMOKE_LITE_ASSETS OR KISAK_VITA_VITA3K_NO_AUDIO))
    message(FATAL_ERROR "Benchmark requires full source/native Release rendering and audio without smoke/probe overrides")
endif()

# The scalar oracle owns a small stand-alone snapshot/parser state block.  A
# source-runtime executable instead links cl_main_mp.cpp, which owns the real
# `cls` and `clientUIActives` globals; compiling both would be an ABI-invalid
# duplicate rather than a useful fallback.
set(KISAK_VITA_BOOTSTRAP_SNAPSHOT_SERVICE "")
if(NOT KISAK_VITA_SOURCE_RUNTIME)
    set(KISAK_VITA_BOOTSTRAP_SNAPSHOT_SERVICE
        ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_snapshot_parser_services.cpp)
endif()

# Catch animation/model/image staging and decoded-array failures inside the loaders.
# Gameplay translation units retain -fno-exceptions.
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_animation_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_model_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_image_bundle.cpp
    PROPERTIES COMPILE_OPTIONS "-fexceptions")

add_executable(KisakCOD-vita-bootstrap
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_bootstrap.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_process_config.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/kvap_reader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/kvap_registry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_asset_db.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_app_state.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_platform_events.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_content_manifest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_perf.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_benchmark.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_effect_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_impact_fx_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_shipment_collision.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_shipment_entities.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_shipment_dm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_mp_client_bridge.cpp
    ${KISAK_VITA_BOOTSTRAP_SNAPSHOT_SERVICE}
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_shipment_weapons.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_model_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_animation_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_pmove_services.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_input.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_frontend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_rawfile_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_script_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_script_vm_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_script_entity_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_localize_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_stringtable_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_menu_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_sound_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_hud_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_image_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_material_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_font_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_net_compat.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_audio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_audio_mixer.cpp
    # Retain the original source snapshot decoder at the narrowest useful
    # seam. The outer gamestate/download dispatcher remains outside the Vita
    # bootstrap until its platform services are closed.
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client_mp/cl_parse_mp.cpp
    # Shared multiplayer movement is linked into the bootstrap now that the
    # Vita trace/dvar compatibility layer provides its service boundary.
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_math.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/surfaceflags.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_jump.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_slidemove.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_pmove.cpp
)
target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
    KISAK_VITA_BENCHMARK_ROUND=${KISAK_VITA_BENCHMARK_ROUND}
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
if(KISAK_VITA_SOURCE_RUNTIME)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_SOURCE_RUNTIME=1
        KISAK_VITA_PROTOTYPE_BRIDGE=0)
else()
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_SOURCE_RUNTIME=0
        KISAK_VITA_PROTOTYPE_BRIDGE=1)
endif()
if(KISAK_VITA_EXTERNAL_CONTENT)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_EXTERNAL_CONTENT=1)
else()
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_EXTERNAL_CONTENT=0)
endif()
if(KISAK_VITA_PROFILE)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_PROFILE=1)
else()
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_PROFILE=0)
endif()
if(KISAK_VITA_VITA3K_NO_AUDIO)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_VITA3K_NO_AUDIO=1)
endif()
if(KISAK_VITA_ALLOW_SOFTWARE_FALLBACK)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_ALLOW_SOFTWARE_FALLBACK=1)
else()
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_ALLOW_SOFTWARE_FALLBACK=0)
endif()
if(KISAK_VITA_CAPTURE_SOFTWARE_FRAME)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_CAPTURE_SOFTWARE_FRAME=1)
endif()
if(KISAK_VITA_DEBUG_NO_FRAME_EFFECTS)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_DEBUG_NO_FRAME_EFFECTS=1)
endif()
if(KISAK_VITA_CAPTURE_OVERLAY_ONLY)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_CAPTURE_OVERLAY_ONLY=1)
endif()
if(KISAK_VITA_SMOKE_LITE_ASSETS)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_SMOKE_LITE_ASSETS=1)
endif()
if(KISAK_VITA_NATIVE_FRAME_PROBE)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_NATIVE_FRAME_PROBE=1)
endif()
if(KISAK_VITA_NATIVE_DIRECT_DISPLAY_PROBE)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_NATIVE_DIRECT_DISPLAY_PROBE=1)
endif()
if(KISAK_VITA_NATIVE_SKIP_CLEAR_PROBE)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_NATIVE_SKIP_CLEAR_PROBE=1)
endif()
if(KISAK_VITA_NATIVE_SKIP_VIEWMODEL_PROBE)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_NATIVE_SKIP_VIEWMODEL_PROBE=1)
endif()
if(KISAK_VITA_NATIVE_SKIP_MODEL_COLOR_PROBE)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_NATIVE_SKIP_MODEL_COLOR_PROBE=1)
endif()
if(KISAK_VITA_NATIVE_CPU_TEXTURE_UPLOAD)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_NATIVE_CPU_TEXTURE_UPLOAD=1)
endif()
if(NOT KISAK_VITA_NATIVE_TRIANGLE_BUDGET STREQUAL "")
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_NATIVE_TRIANGLE_BUDGET=${KISAK_VITA_NATIVE_TRIANGLE_BUDGET})
endif()
if(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME=1)
endif()
if(KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT=1)
endif()
if(KISAK_VITA_SMOKE_GAMEPLAY_ONLY)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_SMOKE_GAMEPLAY_ONLY=1)
endif()
if(KISAK_VITA_SMOKE_AUTHORED_FX_EVENT)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_SMOKE_AUTHORED_FX_EVENT=1)
endif()
if(KISAK_VITA_MENU_ONLY)
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_MENU_ONLY=1)
endif()
if(NOT KISAK_VITA_SMOKE_MAP_INDEX STREQUAL "")
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_SMOKE_MAP_INDEX=${KISAK_VITA_SMOKE_MAP_INDEX})
endif()
if(NOT KISAK_VITA_MENU_SCREEN STREQUAL "")
    target_compile_definitions(KisakCOD-vita-bootstrap PRIVATE
        KISAK_VITA_MENU_SCREEN=${KISAK_VITA_MENU_SCREEN})
endif()
target_compile_options(KisakCOD-vita-bootstrap PRIVATE
    -fpermissive
    -ffunction-sections -fdata-sections
)

add_library(KisakCOD-vita-server-layout-probe OBJECT
    ${CMAKE_CURRENT_LIST_DIR}/../../../tools/vita_asset_prep/vita_server_layout_probe.cpp
)
target_compile_definitions(KisakCOD-vita-server-layout-probe PRIVATE
    KISAK_MP=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-server-layout-probe PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra
)
target_include_directories(KisakCOD-vita-server-layout-probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
)

# Compile a first slice of the real shared gameplay code for ARM.  This is an
# object-only target on purpose: it validates headers, calling conventions,
# math, and data layouts without pretending that the Vita backend is linkable
# before its platform services and asset registry exist.  The source list is
# intentionally expanded in dependency order as each boundary is made
# portable.
add_library(KisakCOD-vita-core-probe OBJECT
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_math.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/surfaceflags.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_jump.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_slidemove.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_pmove.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_client_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_client_script_cmd_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_cmds_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_combat_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_utils_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_misc_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_player_corpse_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_vehicles_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_scr_helicopter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/player_use_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_spawn_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_trigger_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_weapon.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_missile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_items.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_hudelem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_scr_mover.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_main_mp.cpp
    # The actual MP script builtin/method registry is kept in the ARM audit
    # even while its broad game/entity call graph remains outside the visual
    # bootstrap link.
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_scr_main_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server_mp/sv_main_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server_mp/sv_client_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server_mp/sv_snapshot_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server_mp/sv_archive_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server_mp/sv_init_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server_mp/sv_net_chan_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client_mp/cl_input.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/msg_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/net_chan_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/sv_msg_write_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_const.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_memorytree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_stringlist.cpp
    # Object-level ABI gate for the real script VM/compiler. The focused
    # lifecycle probe links the storage/error boundary separately, while the
    # full MP builtin/entity call graph remains visible in the linker output.
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_vm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_main.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_parsetree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_animtree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_variable.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_compiler2.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_evaluate.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_parser.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_yacc2.cpp
)
target_compile_definitions(KisakCOD-vita-core-probe PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
if(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
    # The real script VM/variable objects are compiled into this archive and
    # then linked by both the bootstrap and renderer-free probe. Propagate the
    # diagnostic gate here so allocator telemetry and any ABI fixes are
    # isolated to the explicit experimental build rather than silently being
    # compiled out of the shared archive.
    target_compile_definitions(KisakCOD-vita-core-probe PRIVATE
        KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME=1)
endif()
target_compile_options(KisakCOD-vita-core-probe PRIVATE
    # The recovered x86 sources use unsigned bit-pattern case labels that are
    # intentionally consumed as signed field offsets.  Keep those semantics
    # while the probe is expanded; the eventual Vita source pass will replace
    # them with named fixed-width constants.
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections
)
target_include_directories(KisakCOD-vita-core-probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)

# Broader object-only ARM audit for the real MP entity/weapon/script support
# files.  This stays separate from the linkable engine archive because the
# current bootstrap owns a few of these services (movement, dvars, and
# renderer-facing helpers); compiling them here proves portability without
# introducing duplicate runtime definitions into the existing probes.
add_library(KisakCOD-vita-game-audit OBJECT
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_active_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/bullet.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_mover.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_scr_vehicle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_helicopter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/enthandle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_load_utils.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_client_fields.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_svcmds.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/DynEntity/DynEntity_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/dobj_management.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xanim_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xmodel_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/com_pack.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_cmds_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_misc_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_player_corpse_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_vehicles_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_scr_helicopter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/player_use_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_hudelem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_scr_mover.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_weapons.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_misc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_perks_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_animation_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_vehicles_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_mantle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_stringtable.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_constantconfigstrings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_math_anglevectors.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/q_parse.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/q_shared.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/huffman.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/dobj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/dobj_utils.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/dobj_skel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xanim.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xanim_calc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xmodel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xmodel_utils.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server/sv_world.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server/sv_game.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cmd.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server_mp/sv_snapshot_profile_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_debugger.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_trace.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_world.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_tracebox.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_mesh.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_staticmodel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_load.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/com_bsp_load_obj.cpp
)
target_compile_definitions(KisakCOD-vita-game-audit PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-game-audit PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections
)
target_include_directories(KisakCOD-vita-game-audit PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)

# Archive the real ARM gameplay/server/client objects as a linkable engine
# slice.  It deliberately does not enter the visual bootstrap link: unresolved
# desktop-era service symbols remain visible until the full Vita runtime loop
# supplies them, rather than being hidden by broad dummy implementations.
add_library(KisakCOD-vita-engine STATIC
    $<TARGET_OBJECTS:KisakCOD-vita-core-probe>
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_net_compat.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_audio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_audio_mixer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_input.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_frontend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_rawfile_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_script_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_script_services.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_lowlevel_services.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_platform.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_registry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_session.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_cpu_replay.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_effect_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_impact_fx_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_localize_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_stringtable_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_menu_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_sound_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_hud_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_image_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_material_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_font_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/kvap_reader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/kvap_registry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_asset_db.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_content_manifest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_shipment_collision.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_shipment_entities.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_shipment_dm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_shipment_weapons.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_model_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_animation_bundle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_pmove_services.cpp
    # Source FX visual-state evaluation uses the same authored normalized
    # random table as EffectsCore. Keep the table in the shared engine slice
    # instead of copying it into a Vita-only renderer translation unit.
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_random.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/gfx_vita/gfx_vita_backend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_cpu_profile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_cpu_render.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_display_probe.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_graphics_profile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_cpu_audio.cpp
    ${PROJECT_SOURCE_DIR}/src/vita/vita_cpu_file.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_boot_status.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/gfx_vita/gfx_vita_viewmodel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/q_parse.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_stringtable.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_constantconfigstrings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/huffman.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/dobj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/dobj_utils.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/dobj_skel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xanim.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xanim_calc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xmodel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xmodel_utils.cpp
)
target_compile_definitions(KisakCOD-vita-engine PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
    KISAK_VITA_PROTOTYPE_BRIDGE=0
    KISAK_VITA_PROFILE=$<BOOL:${KISAK_VITA_PROFILE}>
    KISAK_VITA_CLEAN_VIEWMODEL=$<BOOL:${KISAK_VITA_CLEAN_VIEWMODEL}>
)
if(KISAK_VITA_ALLOW_SOFTWARE_FALLBACK)
    target_compile_definitions(KisakCOD-vita-engine PRIVATE
        KISAK_VITA_ALLOW_SOFTWARE_FALLBACK=1)
else()
    target_compile_definitions(KisakCOD-vita-engine PRIVATE
        KISAK_VITA_ALLOW_SOFTWARE_FALLBACK=0)
endif()
if(KISAK_VITA_CAPTURE_SOFTWARE_FRAME)
    target_compile_definitions(KisakCOD-vita-engine PRIVATE
        KISAK_VITA_CAPTURE_SOFTWARE_FRAME=1)
endif()
if(KISAK_VITA_DEBUG_NO_FRAME_EFFECTS)
    target_compile_definitions(KisakCOD-vita-engine PRIVATE
        KISAK_VITA_DEBUG_NO_FRAME_EFFECTS=1)
endif()
if(KISAK_VITA_NATIVE_DIRECT_DISPLAY_PROBE)
    target_compile_definitions(KisakCOD-vita-engine PRIVATE
        KISAK_VITA_NATIVE_DIRECT_DISPLAY_PROBE=1)
endif()
if(KISAK_VITA_NATIVE_CPU_TEXTURE_UPLOAD)
    target_compile_definitions(KisakCOD-vita-engine PRIVATE
        KISAK_VITA_NATIVE_CPU_TEXTURE_UPLOAD=1)
endif()
target_compile_options(KisakCOD-vita-engine PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
)
target_include_directories(KisakCOD-vita-engine PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)

# The bootstrap only needs the compiler/VM lifecycle, not the complete
# desktop MP object graph carried by KisakCOD-vita-engine. Keep this archive
# narrow so calling Scr_LoadScript does not pull server/client diagnostics or
# renderer-only objects into the homebrew executable.
add_library(KisakCOD-vita-script-runtime STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_const.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_memorytree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_stringlist.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_vm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_main.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_parsetree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_animtree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_variable.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_compiler2.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_evaluate.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_parser.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_yacc2.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_script_services.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_lowlevel_services.cpp
)
target_compile_definitions(KisakCOD-vita-script-runtime PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-script-runtime PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections
)
target_include_directories(KisakCOD-vita-script-runtime PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-engine
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)

# Keep the source-backed BG weapon/animation boundary separate from the
# minimal runtime archive used by the normal Pmove and visual smoke probes.
# The full lifecycle gate links both archives; the everyday Vita build can
# therefore remain green while this broader graph exposes its real services.
add_library(KisakCOD-vita-mp-bgame STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_weapons.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_weapons_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_misctables.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_misc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_perks_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_animation_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_vehicles_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/bgame/bg_mantle.cpp
)
target_compile_definitions(KisakCOD-vita-mp-bgame PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-mp-bgame PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
)
target_include_directories(KisakCOD-vita-mp-bgame PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-mp-bgame
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)

# Source-backed server world/game/command boundary. Keep it separate from the
# minimal runtime archive and the BG archive so each lifecycle dependency is
# visible in the link report.
add_library(KisakCOD-vita-mp-server STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server/sv_world.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server/sv_game.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cmd.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/server_mp/sv_snapshot_profile_mp.cpp
)
target_compile_definitions(KisakCOD-vita-mp-server PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
    "$<$<BOOL:${KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT}>:KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT=1>"
)
target_compile_options(KisakCOD-vita-mp-server PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
)
target_include_directories(KisakCOD-vita-mp-server PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-mp-server
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)

# Source-backed collision boundary used by the real server world.  Keep it
# separate from the normal engine archive until its map-loading services are
# supplied; the lifecycle probe links it explicitly so unresolved collision
# dependencies remain visible instead of being hidden by compatibility code.
add_library(KisakCOD-vita-mp-collision STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_trace.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_world.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_tracebox.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_mesh.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_staticmodel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_load.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/cm_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/com_bsp_load_obj.cpp
)
target_compile_definitions(KisakCOD-vita-mp-collision PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-mp-collision PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
)
target_include_directories(KisakCOD-vita-mp-collision PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-mp-collision
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)

# Source-backed one-client frame boundary.  `g_active_mp.cpp` owns the real
# client think/end-frame, trigger, contents, and controller flow used by the
# MP server; keeping it in its own archive makes the lifecycle delta explicit.
add_library(KisakCOD-vita-mp-active STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_active_mp.cpp
)
target_compile_definitions(KisakCOD-vita-mp-active PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-mp-active PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
)
target_include_directories(KisakCOD-vita-mp-active PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-mp-active
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)

# Source-backed MP client lifecycle audit. Keep the real client main,
# snapshot/UI, and net-channel sources in their own archive while Vita's
# renderer/filesystem services are still being closed. Compiling these files
# for ARM prevents the visual bootstrap from silently carrying a Vita-only
# client replacement and makes the remaining dependencies explicit.
add_library(KisakCOD-vita-mp-client STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client_mp/cl_cgame_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client_mp/cl_main_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client_mp/cl_parse_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client_mp/cl_pose_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client_mp/cl_ui_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client_mp/cl_net_chan_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/base64.cpp
)
target_compile_definitions(KisakCOD-vita-mp-client PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-mp-client PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections
)
target_include_directories(KisakCOD-vita-mp-client PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-mp-client
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)

# Source-owned MP client game.  Keep this archive complete so the session
# lifecycle gate cannot pass with a hand-written view/prediction substitute;
# section GC retains only the closure reached by CG_Init/CG_DrawActiveFrame.
add_library(KisakCOD-vita-mp-cgame STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_animtree_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_client_side_effects_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_compassfriendlies_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_consolecmds_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_draw_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_draw_net_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_ents_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_main_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_newDraw_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_players_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_pose_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_predict_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_scoreboard_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_servercmds_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_snapshot_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_vehicles_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame_mp/cg_view_mp.cpp
)
target_compile_definitions(KisakCOD-vita-mp-cgame PRIVATE
    KISAK_MP=1 KISAK_OPENAL=1 KISAK_VITA_SLICE=1)
target_compile_options(KisakCOD-vita-mp-cgame PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections)
target_include_directories(KisakCOD-vita-mp-cgame PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps)
target_link_libraries(KisakCOD-vita-mp-cgame
    vitaGL vitashark
    SceCtrl_stub SceTouch_stub SceAudio_stub SceDisplay_stub SceGxm_stub
    SceLibKernel_stub SceShaccCg_stub SceShaccCgExt
    SceKernelDmacMgr_stub taihen_stub mathneon)

# Shared client-game implementation used by the MP archive above.  These are
# the original prediction, weapon, event, HUD and view support paths; Vita
# supplies only their platform graphics/audio/input services at final link.
add_library(KisakCOD-vita-cgame STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/aim_assist/aim_assist.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/aim_assist/aim_target_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_ammocounter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_camerashake.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_colltree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_compass.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_drawtools.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_draw_debug.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_draw_indicators.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_draw_reticles.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_effects_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_event.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_hudelem.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_info.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_laser.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_localents.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_playerstate.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_pose_utils.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_shellshock.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_visionsets.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_weapons.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/cg_world.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/cgame/offhandweapons.cpp
)
target_compile_definitions(KisakCOD-vita-cgame PRIVATE
    KISAK_MP=1 KISAK_OPENAL=1 KISAK_VITA_SLICE=1)
target_compile_options(KisakCOD-vita-cgame PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections)
target_include_directories(KisakCOD-vita-cgame PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps)
target_link_libraries(KisakCOD-vita-cgame
    vitaGL vitashark
    SceCtrl_stub SceTouch_stub SceAudio_stub SceDisplay_stub SceGxm_stub
    SceLibKernel_stub SceShaccCg_stub SceShaccCgExt
    SceKernelDmacMgr_stub taihen_stub mathneon)

# Original source effect scheduler/state.  The Vita renderer consumes its
# draw records; only GPU submission is platform-owned.
add_library(KisakCOD-vita-effects STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fxcurve.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_archive.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_beam.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_convert.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_draw.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_dvars.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_graph.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_marks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_postlight.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_profile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_random.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_sort.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_sprite.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_system.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_update.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/EffectsCore/fx_update_util.cpp
)
target_compile_definitions(KisakCOD-vita-effects PRIVATE
    KISAK_MP=1 KISAK_OPENAL=1 KISAK_VITA_SLICE=1)
target_compile_options(KisakCOD-vita-effects PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections)
target_include_directories(KisakCOD-vita-effects PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps)
target_link_libraries(KisakCOD-vita-effects
    vitaGL vitashark
    SceCtrl_stub SceTouch_stub SceAudio_stub SceDisplay_stub SceGxm_stub
    SceLibKernel_stub SceShaccCg_stub SceShaccCgExt
    SceKernelDmacMgr_stub taihen_stub mathneon)

# Portable source utilities reached by cgame/session initialization.  These
# are kept separate from Vita OS services so ownership reports distinguish
# recovered engine behavior from platform adaptation.
add_library(KisakCOD-vita-zlib STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps/zlib/adler32.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps/zlib/crc32.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps/zlib/deflate.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps/zlib/trees.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps/zlib/infblock.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps/zlib/infcodes.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps/zlib/inffast.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps/zlib/inflate.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps/zlib/inftrees.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps/zlib/infutil.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps/zlib/zutil.c)
target_compile_options(KisakCOD-vita-zlib PRIVATE -ffunction-sections -fdata-sections)

add_library(KisakCOD-vita-source-support STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound/snd.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound/snd_utils.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound/snd_openal.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound/snd_driver_openal.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/sound/snd_driver_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_sndalias.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client/con_channels.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_shared.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/graph.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/statmonitor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client/screen_placement.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/stringed/stringed_hooks.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/stringed/stringed_ingame.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client_mp/cl_rank.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/gfx_d3d/r_font.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/gfx_d3d/rb_text.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/gfx_d3d/r_textcmds.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/gfx_d3d/r_material_handle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ui/ui_expressions_logicfunctions.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ui_mp/ui_main_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ui/ui_shared.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ui/ui_utils.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ui/ui_atoms.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ui/ui_localvars.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ui/ui_expressions.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_render.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_model_backend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/gfx_d3d/r_model_skin_cpu.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_ui_backend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_capabilities.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client/cl_console.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client/cl_keys.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client_mp/cl_scrn_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/ui_mp/ui_gameinfo_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/dvar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/dvar_cmds.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_stringstorage.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_bsp_filename.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_localized_numbers.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_timescale.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/com_dvars.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/com_field.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/memfile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/threads_rwlock.cpp
)
target_compile_definitions(KisakCOD-vita-source-support PRIVATE
    KISAK_MP=1 KISAK_OPENAL=1 KISAK_VITA_SLICE=1 KISAK_VITA_OPENAL=1)
# The session belongs to the engine archive, not the bootstrap target. Keep
# its opt-in input/phase diagnostics tied to the same explicit smoke setting.
set_property(SOURCE ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_session.cpp
    APPEND PROPERTY COMPILE_DEFINITIONS
    "$<$<BOOL:${KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT}>:KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT=1>")
set_property(SOURCE ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_model_backend.cpp
    APPEND PROPERTY COMPILE_DEFINITIONS
    "$<$<BOOL:${KISAK_VITA_PROFILE}>:KISAK_VITA_PROFILE=1>")
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../../../src/sound/snd_driver_load_obj.cpp
    PROPERTIES COMPILE_DEFINITIONS "DR_WAV_IMPLEMENTATION;DR_MP3_IMPLEMENTATION")
target_compile_options(KisakCOD-vita-source-support PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections)
target_include_directories(KisakCOD-vita-source-support PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps)
target_link_libraries(KisakCOD-vita-source-support
    KisakCOD-vita-zlib
    vitaOpenAL
    vitaGL vitashark
    SceCtrl_stub SceTouch_stub SceAudio_stub SceDisplay_stub SceGxm_stub
    SceLibKernel_stub SceShaccCg_stub SceShaccCgExt
    SceKernelDmacMgr_stub taihen_stub mathneon)

# Source-backed packet writer gate. This exercises the real MP playerstate
# delta writer without requiring a live server/client runtime or external
# sockets; the eventual local-session bridge will consume the same message
# shape after its gamestate/entity sections are wired.
add_executable(KisakCOD-vita-snapshot-writer-probe EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_snapshot_writer_probe.cpp
)
target_compile_definitions(KisakCOD-vita-snapshot-writer-probe PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-snapshot-writer-probe PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections
)
target_include_directories(KisakCOD-vita-snapshot-writer-probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-snapshot-writer-probe
    "-Wl,--start-group"
    KisakCOD-vita-engine
    KisakCOD-vita-source-physics
    KisakCOD-vita-mp-server
    KisakCOD-vita-mp-bgame
    KisakCOD-vita-mp-collision
    KisakCOD-vita-mp-active
    KisakCOD-vita-mp-client
    KisakCOD-vita-mp-cgame
    KisakCOD-vita-cgame
    KisakCOD-vita-effects
    KisakCOD-vita-source-support
    KisakCOD-vita-mp-game-support
    KisakCOD-vita-script-runtime
    "-Wl,--end-group"
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)
target_link_options(KisakCOD-vita-snapshot-writer-probe PRIVATE -Wl,--gc-sections)

# Source parser gate. This keeps the real MP snapshot decoder and a
 # writer/reader harness on the ARM graph. It stops at CL_ParseSnapshot rather
 # than the outer gamestate/download dispatcher until the live bootstrap owns
 # the full CA_CONNECTED/CA_ACTIVE lifecycle.
add_executable(KisakCOD-vita-snapshot-parser-probe EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_snapshot_parser_probe.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_mp_client_bridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_snapshot_parser_services.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/client_mp/cl_parse_mp.cpp
)
target_compile_definitions(KisakCOD-vita-snapshot-parser-probe PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-snapshot-parser-probe PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections
)
target_include_directories(KisakCOD-vita-snapshot-parser-probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-snapshot-parser-probe
    "-Wl,--start-group"
    KisakCOD-vita-engine
    KisakCOD-vita-source-physics
    KisakCOD-vita-mp-server
    KisakCOD-vita-mp-bgame
    KisakCOD-vita-mp-collision
    KisakCOD-vita-mp-active
    KisakCOD-vita-mp-game-support
    KisakCOD-vita-script-runtime
    "-Wl,--end-group"
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)
target_link_options(KisakCOD-vita-snapshot-parser-probe PRIVATE -Wl,--gc-sections)

# Shared MP weapon/mover support referenced by the active client frame and
# vehicle/entity paths.  This archive is deliberately diagnostic-only until
# its remaining sound/physics services are ported.
add_library(KisakCOD-vita-mp-game-support STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/bullet.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_mover.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_scr_vehicle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_helicopter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game_mp/g_vehicles_mp.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/com_math_anglevectors.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/q_shared.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/com_pack.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/enthandle.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_load_utils.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_client_fields.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/game/g_svcmds.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/qcommon/dobj_management.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xanim_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/xanim/xmodel_load_obj.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/universal/aabbtree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/physics/physpreset_load_obj.cpp
)
target_compile_definitions(KisakCOD-vita-mp-game-support PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-mp-game-support PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
)
target_include_directories(KisakCOD-vita-mp-game-support PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-mp-game-support
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)

# Source-backed GSC debugger services.  Keep the real debugger separate from
# the normal runtime until its UI/console dependencies are supplied; the
# lifecycle probe links it explicitly to expose the authentic VM boundary.
add_library(KisakCOD-vita-mp-script-debugger STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/script/scr_debugger.cpp
)
target_compile_definitions(KisakCOD-vita-mp-script-debugger PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-mp-script-debugger PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
)
target_include_directories(KisakCOD-vita-mp-script-debugger PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-mp-script-debugger
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)

add_executable(KisakCOD-vita-runtime-smoke
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_runtime_smoke.cpp
)
target_compile_definitions(KisakCOD-vita-runtime-smoke PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_include_directories(KisakCOD-vita-runtime-smoke PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-runtime-smoke
    KisakCOD-vita-engine
    KisakCOD-vita-source-physics
    vitaGL
    vitashark
    SceCommonDialog_stub
    SceAppMgr_stub
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)
# The smoke executes one bounded shared Pmove frame after the pure helper
# checks. Function/data sections keep that gate focused without requiring every
# server and script service at once.
target_link_options(KisakCOD-vita-runtime-smoke PRIVATE -Wl,--gc-sections)

# Link-only feasibility gate for the real shared Pmove entry point.  This is
# intentionally separate from the bootstrap until the Vita trace/dvar/script
# services satisfy the full call graph.
add_executable(KisakCOD-vita-pmove-link-probe
    ${CMAKE_CURRENT_LIST_DIR}/../../../tools/vita_asset_prep/vita_pmove_link_probe.cpp
)
target_compile_definitions(KisakCOD-vita-pmove-link-probe PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_include_directories(KisakCOD-vita-pmove-link-probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-pmove-link-probe
    KisakCOD-vita-engine
    KisakCOD-vita-source-physics
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)
target_link_options(KisakCOD-vita-pmove-link-probe PRIVATE -Wl,--gc-sections)

# Link-only feasibility gate for dependency-free readers in the real script
# VM object. The full lifecycle remains blocked on Vita storage/allocator
# services, but this prevents the ARM archive from dropping the VM wholesale.
add_executable(KisakCOD-vita-script-vm-probe
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_script_vm_probe.cpp
)
target_compile_definitions(KisakCOD-vita-script-vm-probe PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-script-vm-probe PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra
    -ffunction-sections -fdata-sections
)
target_include_directories(KisakCOD-vita-script-vm-probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-script-vm-probe
    KisakCOD-vita-engine
    KisakCOD-vita-source-physics
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)
target_link_options(KisakCOD-vita-script-vm-probe PRIVATE -Wl,--gc-sections)

# Focused linker gate for the actual GSC compiler/loader/VM entry points.  It
# remains separate from the visual bootstrap while the Vita filesystem,
# allocator, builtin, and entity-field services are being ported.
add_executable(KisakCOD-vita-script-lifecycle-probe EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_script_lifecycle_probe.cpp
)
target_compile_definitions(KisakCOD-vita-script-lifecycle-probe PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
)
target_compile_options(KisakCOD-vita-script-lifecycle-probe PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections
)
target_include_directories(KisakCOD-vita-script-lifecycle-probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-script-lifecycle-probe
    "-Wl,--start-group"
    KisakCOD-vita-engine
    KisakCOD-vita-source-physics
    KisakCOD-vita-mp-bgame
    KisakCOD-vita-mp-server
    KisakCOD-vita-mp-collision
    KisakCOD-vita-mp-active
    KisakCOD-vita-mp-client
    KisakCOD-vita-mp-game-support
    "-Wl,--end-group"
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)
target_link_options(KisakCOD-vita-script-lifecycle-probe PRIVATE -Wl,--gc-sections)

# Link-only closure gate for the true in-process MP frame roots. It is kept
# outside ALL until its unresolved worklist is closed; unlike the visual
# development composition, this target cannot satisfy the link by routing
# gameplay through ShipmentDmServer.
add_executable(KisakCOD-vita-source-loop-link-probe EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_loop_link_probe.cpp
)
target_compile_definitions(KisakCOD-vita-source-loop-link-probe PRIVATE
    KISAK_MP=1 KISAK_OPENAL=1 KISAK_VITA_SLICE=1)
target_compile_options(KisakCOD-vita-source-loop-link-probe PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections)
target_include_directories(KisakCOD-vita-source-loop-link-probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps)
target_link_libraries(KisakCOD-vita-source-loop-link-probe
    "-Wl,--start-group"
    KisakCOD-vita-engine
    KisakCOD-vita-source-physics
    KisakCOD-vita-mp-bgame
    KisakCOD-vita-mp-server
    KisakCOD-vita-mp-collision
    KisakCOD-vita-mp-active
    KisakCOD-vita-mp-client
    KisakCOD-vita-mp-cgame
    KisakCOD-vita-cgame
    KisakCOD-vita-effects
    KisakCOD-vita-source-support
    KisakCOD-vita-mp-game-support
    KisakCOD-vita-script-runtime
    "-Wl,--end-group"
    vitaGL vitashark
    SceCtrl_stub SceTouch_stub SceAudio_stub SceDisplay_stub SceGxm_stub
    SceLibKernel_stub SceShaccCg_stub SceShaccCgExt
    SceKernelDmacMgr_stub taihen_stub mathneon)
target_link_options(KisakCOD-vita-source-loop-link-probe PRIVATE
    -Wl,--gc-sections)

# Link-only closure gate for source server/client initialization.  This is a
# separate worklist from the now-closed frame roots: it must pass before the
# visual bootstrap can replace its development ShipmentDmServer composition
# with an initialized source loopback session.
add_executable(KisakCOD-vita-source-session-link-probe EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_source_session_link_probe.cpp
)
target_compile_definitions(KisakCOD-vita-source-session-link-probe PRIVATE
    KISAK_MP=1 KISAK_OPENAL=1 KISAK_VITA_SLICE=1)
target_compile_options(KisakCOD-vita-source-session-link-probe PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections)
target_include_directories(KisakCOD-vita-source-session-link-probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps)
target_link_libraries(KisakCOD-vita-source-session-link-probe
    "-Wl,--start-group"
    KisakCOD-vita-engine
    KisakCOD-vita-source-physics
    KisakCOD-vita-mp-bgame
    KisakCOD-vita-mp-server
    KisakCOD-vita-mp-collision
    KisakCOD-vita-mp-active
    KisakCOD-vita-mp-client
    KisakCOD-vita-mp-cgame
    KisakCOD-vita-cgame
    KisakCOD-vita-effects
    KisakCOD-vita-source-support
    KisakCOD-vita-mp-game-support
    KisakCOD-vita-script-runtime
    "-Wl,--end-group"
    vitaGL vitashark
    SceCtrl_stub SceTouch_stub SceAudio_stub SceDisplay_stub SceGxm_stub
    SceLibKernel_stub SceShaccCg_stub SceShaccCgExt
    SceKernelDmacMgr_stub taihen_stub mathneon)
target_link_options(KisakCOD-vita-source-session-link-probe PRIVATE
    -Wl,--gc-sections)

# Renderer-free package/GSC probe. This is deliberately separate from the
# visual bootstrap so Vita3K or hardware can qualify the real KVAP rawfile
# mount and seven-script native VM load without touching vitaGL, audio, or the
# frame loop.
add_executable(KisakCOD-vita-script-asset-probe EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_script_asset_probe.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_script_vm_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../../../src/vita/vita_script_entity_runtime.cpp
)
target_compile_definitions(KisakCOD-vita-script-asset-probe PRIVATE
    KISAK_MP=1
    KISAK_OPENAL=1
    KISAK_VITA_SLICE=1
    KISAK_VITA_SCRIPT_ASSET_PROBE=1
)
if(KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME)
    # Keep the isolated probe honest when qualifying the native callback ABI:
    # the option must reach this target as well as the visual bootstrap.
    # Shipping/default builds leave the gate off and retain the safe VM-only
    # probe boundary.
    target_compile_definitions(KisakCOD-vita-script-asset-probe PRIVATE
        KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME=1)
endif()
target_compile_options(KisakCOD-vita-script-asset-probe PRIVATE
    -std=c++20 -fno-rtti -fno-exceptions -Wall -Wextra -fpermissive
    -ffunction-sections -fdata-sections
)
target_include_directories(KisakCOD-vita-script-asset-probe PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)
target_link_libraries(KisakCOD-vita-script-asset-probe
    "-Wl,--start-group"
    KisakCOD-vita-engine
    KisakCOD-vita-source-physics
    KisakCOD-vita-mp-bgame
    KisakCOD-vita-mp-server
    KisakCOD-vita-mp-collision
    KisakCOD-vita-mp-active
    KisakCOD-vita-mp-client
    KisakCOD-vita-mp-game-support
    "-Wl,--end-group"
    vitaGL
    vitashark
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)
target_link_options(KisakCOD-vita-script-asset-probe PRIVATE -Wl,--gc-sections)

set(KISAK_VITA_ASSET_PACKAGE "" CACHE FILEPATH
    "Optional local KVAP package to embed under app0:data/")
set(KISAK_VITA_MODEL_SEMANTIC_PACKAGE "" CACHE FILEPATH
    "Optional segmented XModel/viewmodel normal/specular KVAP to embed under app0:data/")

target_include_directories(KisakCOD-vita-bootstrap PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../src
    ${CMAKE_CURRENT_LIST_DIR}/../../../deps
)

set(KISAK_VITA_BOOTSTRAP_SOURCE_RUNTIME_LIBRARIES "")
if(KISAK_VITA_SOURCE_RUNTIME)
    # The shipping bootstrap calls the original client lifecycle rather than
    # stopping at the server-only probe. Keep its full client/cgame closure in
    # the rescan group so CL_Init/CL_Frame and the server's local-client
    # callbacks resolve from KisakCOD source objects. The scalar oracle keeps
    # its deliberately isolated parser globals and must not link this closure.
    list(APPEND KISAK_VITA_BOOTSTRAP_SOURCE_RUNTIME_LIBRARIES
        KisakCOD-vita-mp-client
        KisakCOD-vita-mp-cgame
        KisakCOD-vita-cgame
        KisakCOD-vita-effects
        KisakCOD-vita-source-support)
endif()

target_link_libraries(KisakCOD-vita-bootstrap
    "-Wl,--start-group"
    KisakCOD-vita-engine
    KisakCOD-vita-source-physics
    KisakCOD-vita-mp-bgame
    KisakCOD-vita-mp-server
    KisakCOD-vita-mp-collision
    KisakCOD-vita-mp-active
    ${KISAK_VITA_BOOTSTRAP_SOURCE_RUNTIME_LIBRARIES}
    KisakCOD-vita-mp-game-support
    KisakCOD-vita-script-runtime
    "-Wl,--end-group"
    vitaGL
    vitashark
    SceCommonDialog_stub
    SceAppMgr_stub
    SceCtrl_stub
    SceTouch_stub
    SceAudio_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceKernelModulemgr_stub
    ScePower_stub
    SceShaccCg_stub
    SceShaccCgExt
    SceKernelDmacMgr_stub
    taihen_stub
    mathneon
)
target_link_options(KisakCOD-vita-bootstrap PRIVATE -Wl,--gc-sections
    # GCC's emutls detects pthread support through this weak symbol. Without
    # forcing its archive member into the ELF, thread_local becomes shared.
    -Wl,--undefined=pthread_cancel
    -Wl,--undefined=sceUserMainThreadStackSize
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/vita-source-link.map,--demangle")
if(KISAK_VITA_CPU_RUNTIME)
    target_link_libraries(KisakCOD-vita-bootstrap SceRazorHud_stub_weak SceSysmodule_stub)
    target_link_options(KisakCOD-vita-bootstrap PRIVATE
        -Wl,--wrap=sceKernelCreateThread -Wl,--wrap=sceKernelDeleteThread
        -Wl,--wrap=sceGxmVshInitialize -Wl,--wrap=sceGxmEndScene -Wl,--wrap=sceGxmBeginScene
        -Wl,--wrap=sceGxmDisplayQueueAddEntry -Wl,--wrap=sceDisplaySetFrameBuf
        -Wl,--wrap=sceDisplayWaitVblankStartMulti)
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Always publish an evidence report for the exact garbage-collected ELF. The
# native source-runtime configuration also enforces the gate before packaging;
# the scalar oracle may retain the focused prototype for comparisons.
set(KISAK_VITA_OWNERSHIP_AUDITOR
    "${CMAKE_CURRENT_LIST_DIR}/../../../tools/vita_asset_prep/audit_vita_source_ownership.py")
set(KISAK_VITA_OWNERSHIP_REPORT
    "${CMAKE_CURRENT_BINARY_DIR}/vita-source-ownership.json")
add_custom_target(KisakCOD-vita-source-ownership-report ALL
    DEPENDS KisakCOD-vita-bootstrap
    BYPRODUCTS "${KISAK_VITA_OWNERSHIP_REPORT}"
    COMMAND "${Python3_EXECUTABLE}" "${KISAK_VITA_OWNERSHIP_AUDITOR}"
        --binary "$<TARGET_FILE:KisakCOD-vita-bootstrap>"
        --nm "${CMAKE_NM}"
        --output "${KISAK_VITA_OWNERSHIP_REPORT}"
        --link-map "${CMAKE_CURRENT_BINARY_DIR}/vita-source-link.map"
        --mode report
    COMMENT "Auditing source ownership in the final Vita ELF"
    VERBATIM
)
add_custom_target(KisakCOD-vita-source-ownership-audit
    DEPENDS KisakCOD-vita-bootstrap
    COMMAND "${Python3_EXECUTABLE}" "${KISAK_VITA_OWNERSHIP_AUDITOR}"
        --binary "$<TARGET_FILE:KisakCOD-vita-bootstrap>"
        --nm "${CMAKE_NM}"
        --output "${KISAK_VITA_OWNERSHIP_REPORT}"
        --link-map "${CMAKE_CURRENT_BINARY_DIR}/vita-source-link.map"
        --scope simulation
        --mode enforce
    COMMENT "Enforcing source-owned Vita offline MP linkage"
    VERBATIM
)

vita_create_self(KisakCOD-vita-bootstrap.self KisakCOD-vita-bootstrap STRIPPED)

# vita-pack-vpk copies source mtimes into ZIP headers.  Keep the SDK-generated
# archive as an intermediate target, then normalize those DOS timestamps before
# publishing the stable VPK target consumed by packaging and CI.
set(KISAK_VITA_VPK_NORMALIZER
    "${CMAKE_CURRENT_LIST_DIR}/../../../tools/vita_asset_prep/normalize_vpk.py")
set(KISAK_VITA_VPK_RAW_TARGET "KisakCOD-vita-bootstrap.vpk.raw")
set(KISAK_VITA_VPK_RAW_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/${KISAK_VITA_VPK_RAW_TARGET}")
set(KISAK_VITA_VPK_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/KisakCOD-vita-bootstrap.vpk")
if(${CMAKE_VERSION} VERSION_LESS "3.20.0")
    set(KISAK_VITA_VPK_RAW_DEPENDENCY ${KISAK_VITA_VPK_RAW_TARGET})
else()
    set(KISAK_VITA_VPK_RAW_DEPENDENCY ${KISAK_VITA_VPK_RAW_TARGET}-vpk)
endif()

set(KISAK_VITA_VPK_FILE_ARGS)
set(KISAK_VITA_APP_ICON "" CACHE FILEPATH "Owned COD4 icon converted to 128x128 indexed PNG")
if(KISAK_VITA_APP_ICON)
    if(NOT EXISTS "${KISAK_VITA_APP_ICON}")
        message(FATAL_ERROR "KISAK_VITA_APP_ICON does not exist: ${KISAK_VITA_APP_ICON}")
    endif()
    list(APPEND KISAK_VITA_VPK_FILE_ARGS
        FILE "${KISAK_VITA_APP_ICON}" "sce_sys/icon0.png")
endif()
if(KISAK_VITA_ASSET_PACKAGE AND NOT KISAK_VITA_EXTERNAL_CONTENT)
    if(NOT EXISTS "${KISAK_VITA_ASSET_PACKAGE}")
        message(FATAL_ERROR "KISAK_VITA_ASSET_PACKAGE does not exist: ${KISAK_VITA_ASSET_PACKAGE}")
    endif()
    # The bootstrap intentionally probes one stable app0 path. Keep the
    # source package's descriptive filename out of the guest contract so a
    # release build cannot silently fall back to an older external package.
    list(APPEND KISAK_VITA_VPK_FILE_ARGS
        FILE "${KISAK_VITA_ASSET_PACKAGE}" "data/shipment-db-reference.kvap")
endif()
if(KISAK_VITA_MODEL_SEMANTIC_PACKAGE AND NOT KISAK_VITA_EXTERNAL_CONTENT)
    if(NOT EXISTS "${KISAK_VITA_MODEL_SEMANTIC_PACKAGE}")
        message(FATAL_ERROR "KISAK_VITA_MODEL_SEMANTIC_PACKAGE does not exist: ${KISAK_VITA_MODEL_SEMANTIC_PACKAGE}")
    endif()
    # Model semantic images are optional at runtime and are probed beside the
    # base package. Keeping them in their own VPK file avoids pushing the main
    # multi-map KVAP past the Vita/Vita3K signed file-offset boundary.
    list(APPEND KISAK_VITA_VPK_FILE_ARGS
        FILE "${KISAK_VITA_MODEL_SEMANTIC_PACKAGE}" "data/shipment-db-model-semantics.kvap")
endif()
vita_create_vpk(${KISAK_VITA_VPK_RAW_TARGET} CODVIT001 KisakCOD-vita-bootstrap.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    ${KISAK_VITA_VPK_FILE_ARGS}
)

add_custom_target(KisakCOD-vita-bootstrap.vpk-vpk
    ALL
    DEPENDS ${KISAK_VITA_VPK_RAW_DEPENDENCY}
    COMMAND "${Python3_EXECUTABLE}" "${KISAK_VITA_VPK_NORMALIZER}" "${KISAK_VITA_VPK_RAW_FILE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${KISAK_VITA_VPK_RAW_FILE}" "${KISAK_VITA_VPK_FILE}"
    COMMENT "Normalizing deterministic VPK metadata"
    VERBATIM
)
if(KISAK_VITA_SOURCE_RUNTIME)
    add_dependencies(KisakCOD-vita-bootstrap.vpk-vpk
                     KisakCOD-vita-source-ownership-audit)
endif()

set(KISAK_VITA_SCRIPT_PROBE_SELF
    "${CMAKE_CURRENT_BINARY_DIR}/KisakCOD-vita-script-asset-probe.self")
set(KISAK_VITA_SCRIPT_PROBE_VPK_RAW_TARGET
    "KisakCOD-vita-script-asset-probe.vpk.raw")
set(KISAK_VITA_SCRIPT_PROBE_VPK_RAW_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/${KISAK_VITA_SCRIPT_PROBE_VPK_RAW_TARGET}")
set(KISAK_VITA_SCRIPT_PROBE_VPK_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/KisakCOD-vita-script-asset-probe.vpk")
vita_create_self(KisakCOD-vita-script-asset-probe.self
    KisakCOD-vita-script-asset-probe STRIPPED)
if(KISAK_VITA_ASSET_PACKAGE)
    vita_create_vpk(${KISAK_VITA_SCRIPT_PROBE_VPK_RAW_TARGET} CODVIT002
        KisakCOD-vita-script-asset-probe.self
        VERSION ${VITA_VERSION}
        NAME "KisakCOD Vita Script Asset Probe"
        FILE "${KISAK_VITA_ASSET_PACKAGE}" "data/shipment-db-reference.kvap")
else()
    vita_create_vpk(${KISAK_VITA_SCRIPT_PROBE_VPK_RAW_TARGET} CODVIT002
        KisakCOD-vita-script-asset-probe.self
        VERSION ${VITA_VERSION}
        NAME "KisakCOD Vita Script Asset Probe")
endif()
if(${CMAKE_VERSION} VERSION_LESS "3.20.0")
    set(KISAK_VITA_SCRIPT_PROBE_VPK_RAW_DEPENDENCY
        ${KISAK_VITA_SCRIPT_PROBE_VPK_RAW_TARGET})
else()
    set(KISAK_VITA_SCRIPT_PROBE_VPK_RAW_DEPENDENCY
        ${KISAK_VITA_SCRIPT_PROBE_VPK_RAW_TARGET}-vpk)
endif()
add_custom_target(KisakCOD-vita-script-asset-probe.vpk-vpk
    DEPENDS ${KISAK_VITA_SCRIPT_PROBE_VPK_RAW_DEPENDENCY}
    COMMAND "${Python3_EXECUTABLE}" "${KISAK_VITA_VPK_NORMALIZER}"
        "${KISAK_VITA_SCRIPT_PROBE_VPK_RAW_FILE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${KISAK_VITA_SCRIPT_PROBE_VPK_RAW_FILE}"
        "${KISAK_VITA_SCRIPT_PROBE_VPK_FILE}"
    COMMENT "Normalizing deterministic script probe VPK metadata"
    VERBATIM
)

include(${CMAKE_CURRENT_LIST_DIR}/source-physics.cmake)

if(KISAK_VITA_CPU_RUNTIME)
    set(KISAK_CPU_SHADER_KEY "${CMAKE_CURRENT_BINARY_DIR}/vita_cpu_shader_key.h")
    set(KISAK_CPU_SHADER_KEY_TOOL "${PROJECT_SOURCE_DIR}/tools/vita_asset_prep/cpu_shader_key.py")
    set(KISAK_CPU_GL_PROVENANCE "${KISAK_VITA_VITAGL_DIRECTORY}/vitaGL-build.json")
    add_custom_command(OUTPUT "${KISAK_CPU_SHADER_KEY}"
        COMMAND "${Python3_EXECUTABLE}" "${KISAK_CPU_SHADER_KEY_TOOL}"
            --source "${PROJECT_SOURCE_DIR}" --provenance "${KISAK_CPU_GL_PROVENANCE}"
            --output "${KISAK_CPU_SHADER_KEY}"
        DEPENDS "${KISAK_CPU_SHADER_KEY_TOOL}" "${KISAK_CPU_GL_PROVENANCE}"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_backend.cpp"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_viewmodel.cpp"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_prop_shader.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_color_fast.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_hud_shader.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_early_alpha.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_world_lifetime.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_world_math.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_world_precision.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_world_basis.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_world_gloss.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_world_components.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_opaque_shader.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_material_probe.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_specular_shader.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_material_family.h"
            "${PROJECT_SOURCE_DIR}/src/gfx_vita/gfx_vita_texture_prefetch.h"
        VERBATIM)
    target_sources(KisakCOD-vita-engine PRIVATE "${KISAK_CPU_SHADER_KEY}"
        "${PROJECT_SOURCE_DIR}/src/vita/vita_cpu_shader_cache.cpp"
        "${PROJECT_SOURCE_DIR}/src/vita/vita_texture_store.cpp")
    target_include_directories(KisakCOD-vita-engine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
