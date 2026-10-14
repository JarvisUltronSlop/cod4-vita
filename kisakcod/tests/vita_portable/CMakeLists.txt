cmake_minimum_required(VERSION 3.20)
project(VitaPortableTests LANGUAGES C CXX)
enable_testing()
add_executable(vita-pack-layout-tests pack_layout_tests.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-pack-layout-tests PRIVATE cxx_std_20)
target_include_directories(vita-pack-layout-tests PRIVATE ../../src)
add_test(NAME vita-installed-pack-layout COMMAND vita-pack-layout-tests)
add_executable(vita-texture-pressure-tests texture_pressure_tests.cpp)
target_compile_features(vita-texture-pressure-tests PRIVATE cxx_std_20)
target_include_directories(vita-texture-pressure-tests PRIVATE ../../src)
add_test(NAME vita-texture-pressure-recovery COMMAND vita-texture-pressure-tests)
add_executable(vita-texture-reclaim-tests texture_reclaim_tests.cpp)
target_compile_features(vita-texture-reclaim-tests PRIVATE cxx_std_20)
target_include_directories(vita-texture-reclaim-tests PRIVATE ../../src)
add_test(NAME vita-completed-texture-reclaim COMMAND vita-texture-reclaim-tests)
add_executable(vita-texture-scratch-tests texture_scratch_tests.cpp)
target_compile_features(vita-texture-scratch-tests PRIVATE cxx_std_20)
target_include_directories(vita-texture-scratch-tests PRIVATE ../../src)
add_test(NAME vita-texture-scratch-lifetime COMMAND vita-texture-scratch-tests)
add_executable(vita-model-indices-tests model_indices_tests.cpp)
target_compile_features(vita-model-indices-tests PRIVATE cxx_std_20)
target_include_directories(vita-model-indices-tests PRIVATE ../../src)
add_test(NAME vita-source-model-index-sharing COMMAND vita-model-indices-tests)
add_executable(vita-map-content-tests map_content_tests.cpp ../../src/vita/vita_content_manifest.cpp
    ../../src/vita/vita_asset_db.cpp ../../src/vita/kvap_registry.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-map-content-tests PRIVATE cxx_std_20)
target_include_directories(vita-map-content-tests PRIVATE frontend_platform ../../src)
add_test(NAME vita-original-map-content COMMAND vita-map-content-tests)
add_executable(vita-map-payload-probe map_payload_probe.cpp
    ../../src/vita/vita_content_manifest.cpp ../../src/vita/vita_shipment_collision.cpp
    ../../src/vita/vita_shipment_entities.cpp ../../src/vita/vita_shipment_weapons.cpp
    ../../src/vita/vita_model_bundle.cpp ../../src/vita/vita_animation_bundle.cpp ../../src/vita/vita_effect_bundle.cpp
    ../../src/vita/vita_material_bundle.cpp ../../src/vita/vita_rawfile_bundle.cpp
    ../../src/vita/vita_image_bundle.cpp ../../src/vita/vita_asset_db.cpp
    ../../src/vita/kvap_registry.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-map-payload-probe PRIVATE cxx_std_20)
target_sources(vita-map-payload-probe PRIVATE ../../src/vita/vita_environment.cpp
    ../../src/vita/vita_impact_fx_bundle.cpp ../../src/vita/vita_hud_bundle.cpp
    ../../src/vita/vita_font_bundle.cpp ../../src/vita/vita_localize_bundle.cpp
    ../../src/vita/vita_stringtable_bundle.cpp ../../src/vita/vita_menu_bundle.cpp
    ../../src/vita/vita_sound_bundle.cpp)
target_include_directories(vita-map-payload-probe PRIVATE frontend_platform ../../src)
add_executable(vita-material-family-tests material_family_tests.cpp)
target_compile_features(vita-material-family-tests PRIVATE cxx_std_20)
target_include_directories(vita-material-family-tests PRIVATE ../../src)
add_test(NAME vita-material-family-contract COMMAND vita-material-family-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

# Exercise compression with the exact retained zlib sources used by Vita.
set(recorder_zlib_sources adler32.c crc32.c deflate.c trees.c infblock.c infcodes.c inffast.c inflate.c inftrees.c infutil.c zutil.c)
list(TRANSFORM recorder_zlib_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/zlib/")
add_library(vita-recorder-zlib STATIC ${recorder_zlib_sources})
add_executable(vita-cpu-compressed-log-tests cpu_compressed_log_tests.cpp)
target_compile_features(vita-cpu-compressed-log-tests PRIVATE cxx_std_20)
target_include_directories(vita-cpu-compressed-log-tests PRIVATE ../../src)
target_link_libraries(vita-cpu-compressed-log-tests PRIVATE vita-recorder-zlib)
add_test(NAME vita-cpu-compressed-log-contract COMMAND vita-cpu-compressed-log-tests)

if(MSVC)
    target_compile_definitions(vita-recorder-zlib PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(vita-specular-shader-tests specular_shader_tests.cpp)
target_compile_features(vita-specular-shader-tests PRIVATE cxx_std_20)
target_include_directories(vita-specular-shader-tests PRIVATE ../../src)
add_test(NAME vita-specular-shader-contract COMMAND vita-specular-shader-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")
add_executable(vita-material-probe-tests material_probe_tests.cpp)
target_compile_features(vita-material-probe-tests PRIVATE cxx_std_20)
target_include_directories(vita-material-probe-tests PRIVATE ../../src)
add_test(NAME vita-material-probe-contract COMMAND vita-material-probe-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")
add_executable(vita-world-depth-order-tests world_depth_order_tests.cpp)
target_compile_features(vita-world-depth-order-tests PRIVATE cxx_std_20)
target_include_directories(vita-world-depth-order-tests PRIVATE ../../src)
add_test(NAME vita-world-depth-order-contract COMMAND vita-world-depth-order-tests)
add_executable(vita-opaque-shader-tests opaque_shader_tests.cpp)
target_compile_features(vita-opaque-shader-tests PRIVATE cxx_std_20)
target_include_directories(vita-opaque-shader-tests PRIVATE ../../src)
add_test(NAME vita-opaque-shader-contract COMMAND vita-opaque-shader-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")
add_executable(vita-prop-workload-tests prop_workload_tests.cpp)
target_compile_features(vita-prop-workload-tests PRIVATE cxx_std_20)
target_include_directories(vita-prop-workload-tests PRIVATE ../../src)
add_test(NAME vita-prop-workload-contract COMMAND vita-prop-workload-tests)
add_executable(vita-prop-shader-tests prop_shader_tests.cpp)
target_compile_features(vita-prop-shader-tests PRIVATE cxx_std_20)
target_include_directories(vita-prop-shader-tests PRIVATE ../../src)
add_test(NAME vita-prop-shader-contract COMMAND vita-prop-shader-tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")
set(model_reference "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/cpu-preservation/reference_model_bundle.cpp")
if(EXISTS "${model_reference}")
    add_executable(vita-model-stream-tests model_stream_tests.cpp "${model_reference}"
        ../../src/vita/vita_model_bundle.cpp ../../src/vita/vita_asset_db.cpp
        ../../src/vita/kvap_registry.cpp ../../src/vita/kvap_reader.cpp)
    target_compile_features(vita-model-stream-tests PRIVATE cxx_std_20)
    target_compile_definitions(vita-model-stream-tests PRIVATE KISAK_VITA_CPU_RUNTIME=1)
    target_include_directories(vita-model-stream-tests PRIVATE ../../src
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/cpu-preservation")
    add_test(NAME vita-model-stream-parity COMMAND vita-model-stream-tests
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../Data/CODVIT001/paks/mp_shipment.kvap"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../Data/CODVIT001/paks/common.kvap")
    set_tests_properties(vita-model-stream-parity PROPERTIES TIMEOUT 120)
endif()
add_executable(vita-brush-scan-timing-tests brush_scan_timing_tests.cpp)
target_compile_features(vita-brush-scan-timing-tests PRIVATE cxx_std_20)
target_include_directories(vita-brush-scan-timing-tests PRIVATE ../../src)
add_test(NAME vita-brush-scan-timing COMMAND vita-brush-scan-timing-tests)
find_package(Threads REQUIRED)
add_executable(vita-cpu-queue-tests cpu_queue_tests.cpp)
target_compile_features(vita-cpu-queue-tests PRIVATE cxx_std_20)
target_include_directories(vita-cpu-queue-tests PRIVATE ../../src)
target_link_libraries(vita-cpu-queue-tests PRIVATE Threads::Threads)
add_test(NAME vita-cpu-scheduling-snapshots COMMAND vita-cpu-queue-tests)
set_tests_properties(vita-cpu-scheduling-snapshots PROPERTIES TIMEOUT 30)
add_executable(vita-frontend-graphics-tests frontend_graphics_tests.cpp
    ../../src/vita/vita_frontend.cpp ../../src/vita/vita_menu_bundle.cpp
    ../../src/vita/vita_localize_bundle.cpp ../../src/vita/vita_stringtable_bundle.cpp
    ../../src/vita/vita_font_bundle.cpp ../../src/vita/vita_asset_db.cpp
    ../../src/vita/kvap_registry.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-frontend-graphics-tests PRIVATE cxx_std_20)
target_compile_definitions(vita-frontend-graphics-tests PRIVATE KISAK_VITA_BALANCED_CANDIDATE=1 KISAK_VITA_BENCHMARK=1)
target_include_directories(vita-frontend-graphics-tests PRIVATE frontend_platform ../../src)
add_test(NAME vita-graphics-menu-route COMMAND vita-frontend-graphics-tests)
add_executable(vita-balanced-tests balanced_tests.cpp)
target_compile_features(vita-balanced-tests PRIVATE cxx_std_20)
target_include_directories(vita-balanced-tests PRIVATE ../../src)
add_test(NAME vita-balanced-profile COMMAND vita-balanced-tests)
add_executable(vita-benchmark-pcm-convert ../../tools/vita_asset_prep/benchmark_pcm_convert.cpp)
target_compile_features(vita-benchmark-pcm-convert PRIVATE cxx_std_17)
target_include_directories(vita-benchmark-pcm-convert PRIVATE ../../deps)
add_executable(vita-trace-budget-tests trace_budget_tests.cpp)
target_compile_features(vita-trace-budget-tests PRIVATE cxx_std_20)
target_include_directories(vita-trace-budget-tests PRIVATE ../../src)
add_test(NAME vita-trace-failure-reserve COMMAND vita-trace-budget-tests)
add_executable(vita-light-dispatch-tests light_dispatch_tests.cpp)
target_compile_features(vita-light-dispatch-tests PRIVATE cxx_std_20)
target_include_directories(vita-light-dispatch-tests PRIVATE ../../src)
add_test(NAME vita-zero-light-dispatch COMMAND vita-light-dispatch-tests)
add_executable(vita-animation-bundle-tests animation_bundle_tests.cpp
    ../../src/vita/vita_animation_bundle.cpp ../../src/vita/vita_asset_db.cpp
    ../../src/vita/kvap_registry.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-animation-bundle-tests PRIVATE cxx_std_20)
target_include_directories(vita-animation-bundle-tests PRIVATE ../../src)
add_test(NAME vita-animation-streaming COMMAND vita-animation-bundle-tests)
add_executable(vita-weapon-memory-tests weapon_memory_tests.cpp)
target_compile_features(vita-weapon-memory-tests PRIVATE cxx_std_20)
target_include_directories(vita-weapon-memory-tests PRIVATE ../../src)
add_test(NAME vita-source-weapon-memory-lifetime COMMAND vita-weapon-memory-tests)
add_executable(vita-benchmark-tests benchmark_tests.cpp ../../src/vita/vita_benchmark.cpp)
target_compile_features(vita-benchmark-tests PRIVATE cxx_std_20)
target_include_directories(vita-benchmark-tests PRIVATE ../../src)
add_test(NAME vita-benchmark-round COMMAND vita-benchmark-tests)
add_executable(vita-perf-tests perf_tests.cpp ../../src/vita/vita_perf.cpp)
target_compile_features(vita-perf-tests PRIVATE cxx_std_20)
target_include_directories(vita-perf-tests PRIVATE ../../src)
add_test(NAME vita-perf-run COMMAND vita-perf-tests)
add_executable(vita-audio-tests audio_tests.cpp ../../src/vita/vita_audio_mixer.cpp)
target_compile_features(vita-audio-tests PRIVATE cxx_std_20)
target_include_directories(vita-audio-tests PRIVATE ../../src)
add_test(NAME vita-audio-mixer COMMAND vita-audio-tests)
add_executable(vita-clock-tests clock_tests.cpp)
target_compile_features(vita-clock-tests PRIVATE cxx_std_20)
target_include_directories(vita-clock-tests PRIVATE ../../src)
add_test(NAME vita-simulation-clock COMMAND vita-clock-tests)
add_executable(vita-sound-bundle-tests sound_bundle_tests.cpp
    ../../src/vita/vita_sound_bundle.cpp ../../src/vita/vita_asset_db.cpp
    ../../src/vita/kvap_registry.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-sound-bundle-tests PRIVATE cxx_std_20)
target_include_directories(vita-sound-bundle-tests PRIVATE ../../src)
add_test(NAME vita-sound-bundle-lifetime COMMAND vita-sound-bundle-tests)
add_executable(vita-hunk-tests hunk_tests.cpp)
target_compile_features(vita-hunk-tests PRIVATE cxx_std_20)
target_include_directories(vita-hunk-tests PRIVATE ../../src)
add_test(NAME vita-source-hunk-lifetime COMMAND vita-hunk-tests)
add_executable(vita-texture-tests texture_tests.cpp)
target_compile_features(vita-texture-tests PRIVATE cxx_std_20)
target_include_directories(vita-texture-tests PRIVATE ../../src)
add_test(NAME vita-texture-content-lifetime COMMAND vita-texture-tests)

add_executable(vita-texture-store-tests texture_store_tests.cpp cpu_platform/kernel.cpp ../../src/vita/vita_texture_store.cpp)
target_compile_features(vita-texture-store-tests PRIVATE cxx_std_20)
target_compile_definitions(vita-texture-store-tests PRIVATE KISAK_VITA_CPU_RUNTIME=1 KISAK_VITA_CPU_TEST=1)
target_include_directories(vita-texture-store-tests PRIVATE cpu_platform ../../src)
target_link_libraries(vita-texture-store-tests PRIVATE Threads::Threads)
add_test(NAME vita-texture-backing-lifetime COMMAND vita-texture-store-tests)
set_tests_properties(vita-texture-backing-lifetime PROPERTIES TIMEOUT 30)
add_executable(vita-vertex-tests vertex_tests.cpp)
target_compile_features(vita-vertex-tests PRIVATE cxx_std_20)
target_include_directories(vita-vertex-tests PRIVATE ../../src)
add_test(NAME vita-static-model-vertex COMMAND vita-vertex-tests)
add_executable(vita-font-tests font_tests.cpp ../../src/vita/vita_font_bundle.cpp
    ../../src/vita/vita_asset_db.cpp ../../src/vita/kvap_registry.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-font-tests PRIVATE cxx_std_20)
target_include_directories(vita-font-tests PRIVATE ../../src)
add_test(NAME vita-source-font-contract COMMAND vita-font-tests)
add_executable(vita-random-tests random_tests.cpp)
target_compile_features(vita-random-tests PRIVATE cxx_std_20)
target_include_directories(vita-random-tests PRIVATE ../../src)
add_test(NAME vita-source-crt-random COMMAND vita-random-tests)
add_executable(vita-material-tests material_tests.cpp ../../src/vita/vita_material_bundle.cpp
    ../../src/vita/vita_asset_db.cpp ../../src/vita/kvap_registry.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-material-tests PRIVATE cxx_std_20)
target_include_directories(vita-material-tests PRIVATE ../../src)
add_test(NAME vita-source-material-contract COMMAND vita-material-tests)
add_executable(vita-projection-tests projection_tests.cpp)
target_compile_features(vita-projection-tests PRIVATE cxx_std_20)
target_include_directories(vita-projection-tests PRIVATE ../../src)
add_test(NAME vita-source-projection-depth COMMAND vita-projection-tests)

add_executable(vita-benchmark-texture-tests benchmark_texture_tests.cpp)
target_compile_features(vita-benchmark-texture-tests PRIVATE cxx_std_20)
target_include_directories(vita-benchmark-texture-tests PRIVATE ../../src)
add_test(NAME vita-benchmark-mip-selection COMMAND vita-benchmark-texture-tests)

add_executable(vita-image-stream-tests image_stream_tests.cpp ../../src/vita/vita_image_bundle.cpp
    ../../src/vita/vita_asset_db.cpp ../../src/vita/kvap_registry.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-image-stream-tests PRIVATE cxx_std_20)
target_include_directories(vita-image-stream-tests PRIVATE frontend_platform ../../src)
add_test(NAME vita-bounded-image-stream COMMAND vita-image-stream-tests)

add_executable(vita-cpu-file-tests cpu_file_tests.cpp cpu_platform/kernel.cpp ../../src/vita/vita_cpu_file.cpp)
target_compile_features(vita-cpu-file-tests PRIVATE cxx_std_20)
target_compile_definitions(vita-cpu-file-tests PRIVATE KISAK_VITA_CPU_RUNTIME=1 KISAK_VITA_CPU_TEST=1)
target_include_directories(vita-cpu-file-tests PRIVATE cpu_platform ../../src)
target_link_libraries(vita-cpu-file-tests PRIVATE Threads::Threads)
add_test(NAME vita-cpu-file-ownership COMMAND vita-cpu-file-tests)
set_tests_properties(vita-cpu-file-ownership PROPERTIES TIMEOUT 30)

add_executable(vita-cpu-audio-index ../../tools/vita_asset_prep/cpu_audio_index.cpp)
target_compile_features(vita-cpu-audio-index PRIVATE cxx_std_20)
target_include_directories(vita-cpu-audio-index PRIVATE ../../src ../../deps)

foreach(cpu_case render audio)
    add_executable(vita-cpu-${cpu_case}-tests cpu_${cpu_case}_tests.cpp cpu_platform/kernel.cpp ../../src/vita/vita_cpu_${cpu_case}.cpp)
    target_compile_features(vita-cpu-${cpu_case}-tests PRIVATE cxx_std_20)
    target_compile_definitions(vita-cpu-${cpu_case}-tests PRIVATE KISAK_VITA_CPU_RUNTIME=1 KISAK_VITA_CPU_TEST=1)
    target_include_directories(vita-cpu-${cpu_case}-tests PRIVATE cpu_platform ../../src ../../deps)
    target_link_libraries(vita-cpu-${cpu_case}-tests PRIVATE Threads::Threads)
    add_test(NAME vita-cpu-${cpu_case}-ownership COMMAND vita-cpu-${cpu_case}-tests)
    set_tests_properties(vita-cpu-${cpu_case}-ownership PROPERTIES TIMEOUT 30)
endforeach()

add_executable(vita-cpu-common-tests font_tests.cpp ../../src/vita/vita_font_bundle.cpp
    ../../src/vita/vita_asset_db.cpp ../../src/vita/kvap_registry.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-cpu-common-tests PRIVATE cxx_std_20)
target_compile_definitions(vita-cpu-common-tests PRIVATE KISAK_VITA_CPU_RUNTIME=1)
target_include_directories(vita-cpu-common-tests PRIVATE ../../src)
add_test(NAME vita-cpu-common-residency COMMAND vita-cpu-common-tests)

add_executable(vita-cpu-frontend-tests frontend_graphics_tests.cpp
    ../../src/vita/vita_frontend.cpp ../../src/vita/vita_menu_bundle.cpp
    ../../src/vita/vita_localize_bundle.cpp ../../src/vita/vita_stringtable_bundle.cpp
    ../../src/vita/vita_font_bundle.cpp ../../src/vita/vita_asset_db.cpp
    ../../src/vita/kvap_registry.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-cpu-frontend-tests PRIVATE cxx_std_20)
target_compile_definitions(vita-cpu-frontend-tests PRIVATE KISAK_VITA_BALANCED_CANDIDATE=1 KISAK_VITA_BENCHMARK=1 KISAK_VITA_CPU_RUNTIME=1 KISAK_VITA_CPU_PROFILE=1)
target_include_directories(vita-cpu-frontend-tests PRIVATE frontend_platform ../../src)
add_test(NAME vita-cpu-menu-route COMMAND vita-cpu-frontend-tests)

add_executable(vita-shipment-autotest-tests shipment_autotest_tests.cpp)
target_compile_features(vita-shipment-autotest-tests PRIVATE cxx_std_20)
target_include_directories(vita-shipment-autotest-tests PRIVATE ../../src)
add_test(NAME vita-shipment-autotest-lifecycle COMMAND vita-shipment-autotest-tests)

add_executable(vita-cpu-recorder-start-tests cpu_recorder_start_tests.cpp)
target_compile_features(vita-cpu-recorder-start-tests PRIVATE cxx_std_20)
target_include_directories(vita-cpu-recorder-start-tests PRIVATE ../../src)
add_test(NAME vita-cpu-recorder-startup COMMAND vita-cpu-recorder-start-tests)

add_executable(vita-cpu-tls-tests cpu_tls_tests.cpp cpu_platform/kernel.cpp)
target_compile_features(vita-cpu-tls-tests PRIVATE cxx_std_20)
target_include_directories(vita-cpu-tls-tests PRIVATE cpu_platform ../../src)
target_link_libraries(vita-cpu-tls-tests PRIVATE Threads::Threads)
add_test(NAME vita-cpu-tls-isolation COMMAND vita-cpu-tls-tests)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
foreach(part types draw)
    set(world_visibility_${part} "${CMAKE_CURRENT_BINARY_DIR}/world_visibility_${part}.inc")
    add_custom_command(OUTPUT "${world_visibility_${part}}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_cpu_optimization_test.py"
            --kind "world-visibility-${part}" --source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vita/vita_bootstrap.cpp"
            --output "${world_visibility_${part}}"
        DEPENDS ../../tools/vita_asset_prep/extract_cpu_optimization_test.py ../../src/vita/vita_bootstrap.cpp
        VERBATIM)
endforeach()
foreach(source_runtime 0 1)
    set(visibility_target "vita-world-visibility-${source_runtime}-tests")
    add_executable(${visibility_target} world_visibility_tests.cpp
        "${world_visibility_types}" "${world_visibility_draw}")
    target_compile_features(${visibility_target} PRIVATE cxx_std_20)
    target_compile_definitions(${visibility_target} PRIVATE
        KISAK_VITA_SOURCE_RUNTIME=${source_runtime} KISAK_VITA_CPU_RUNTIME=1 KISAK_VITA_BENCHMARK=0)
    target_include_directories(${visibility_target} PRIVATE ../../src "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME "vita-world-visibility-${source_runtime}-parity" COMMAND ${visibility_target})
endforeach()
set(session_stop_header "${CMAKE_CURRENT_BINARY_DIR}/source_session_stop.inc")
add_custom_command(OUTPUT "${session_stop_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_cpu_optimization_test.py"
        --kind session-stop --source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vita/vita_source_session.cpp" --output "${session_stop_header}"
    DEPENDS ../../tools/vita_asset_prep/extract_cpu_optimization_test.py ../../src/vita/vita_source_session.cpp
        ../../src/sound/snd_driver_openal.cpp ../../src/sound/snd.cpp
    VERBATIM)
add_executable(vita-session-stop-tests session_stop_tests.cpp "${session_stop_header}")
target_compile_features(vita-session-stop-tests PRIVATE cxx_std_20)
target_include_directories(vita-session-stop-tests PRIVATE ../../src "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME vita-source-session-stop COMMAND vita-session-stop-tests)
foreach(optimization lighting terrain audio residency)
    if(optimization STREQUAL "lighting")
        set(optimization_source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vita/vita_bootstrap.cpp")
        set(optimization_header "${CMAKE_CURRENT_BINARY_DIR}/source_model_light.inc")
        set(optimization_test normal_response_cache_tests.cpp)
    elseif(optimization STREQUAL "residency")
        set(optimization_source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vita/vita_bootstrap.cpp")
        set(optimization_header "${CMAKE_CURRENT_BINARY_DIR}/source_material_residency.inc")
        set(optimization_test material_residency_tests.cpp)
    elseif(optimization STREQUAL "audio")
        set(optimization_source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/sound/snd_driver_openal.cpp")
        set(optimization_header "${CMAKE_CURRENT_BINARY_DIR}/source_audio_service.inc")
        set(optimization_test audio_service_tests.cpp)
    else()
        set(optimization_source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/qcommon/cm_mesh.cpp")
        set(optimization_header "${CMAKE_CURRENT_BINARY_DIR}/source_terrain.inc")
        set(optimization_test terrain_partition_tests.cpp)
    endif()
    add_custom_command(OUTPUT "${optimization_header}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_cpu_optimization_test.py"
            --kind "${optimization}" --source "${optimization_source}" --output "${optimization_header}"
        DEPENDS ../../tools/vita_asset_prep/extract_cpu_optimization_test.py "${optimization_source}" ../../src/vita/vita_source_registry.cpp
        VERBATIM)
    add_executable(vita-${optimization}-optimization-tests "${optimization_test}" "${optimization_header}")
    target_compile_features(vita-${optimization}-optimization-tests PRIVATE cxx_std_20)
    target_include_directories(vita-${optimization}-optimization-tests PRIVATE ../../src "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME vita-${optimization}-optimization-parity COMMAND vita-${optimization}-optimization-tests)
    set_tests_properties(vita-${optimization}-optimization-parity PROPERTIES TIMEOUT 60)
endforeach()
set(shipment_terrain_fixture "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/cpu-preservation/shipment-terrain-test.bin")
if(EXISTS "${shipment_terrain_fixture}")
    add_test(NAME vita-shipment-terrain-parity COMMAND vita-terrain-optimization-tests "${shipment_terrain_fixture}")
    set_tests_properties(vita-shipment-terrain-parity PROPERTIES TIMEOUT 60)
endif()
set(trace_brush_test_header "${CMAKE_CURRENT_BINARY_DIR}/trace_brush_native.inc")
add_custom_command(OUTPUT "${trace_brush_test_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_trace_brush_test.py"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/qcommon/cm_trace.cpp" --output "${trace_brush_test_header}"
    DEPENDS ../../tools/vita_asset_prep/extract_trace_brush_test.py ../../src/qcommon/cm_trace.cpp
    VERBATIM)
add_executable(vita-trace-brush-tests trace_brush_tests.cpp "${trace_brush_test_header}")
target_compile_features(vita-trace-brush-tests PRIVATE cxx_std_20)
target_include_directories(vita-trace-brush-tests PRIVATE ../../src "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME vita-source-brush-filter-parity COMMAND vita-trace-brush-tests)
set_tests_properties(vita-source-brush-filter-parity PROPERTIES TIMEOUT 30)
set(cpu_cache_test_header "${CMAKE_CURRENT_BINARY_DIR}/cpu_cache_envelope.inc")
add_custom_command(OUTPUT "${cpu_cache_test_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_cpu_cache_test.py"
        --patch "${CMAKE_CURRENT_SOURCE_DIR}/../../dependencies/vita/vitagl-cpu-cache.patch"
        --padding-patch "${CMAKE_CURRENT_SOURCE_DIR}/../../dependencies/vita/vitagl-cpu-cache-padding.patch"
        --output "${cpu_cache_test_header}"
    DEPENDS ../../tools/vita_asset_prep/extract_cpu_cache_test.py ../../dependencies/vita/vitagl-cpu-cache.patch ../../dependencies/vita/vitagl-cpu-cache-padding.patch
    VERBATIM)
add_executable(vita-cpu-cache-tests cpu_cache_tests.cpp "${cpu_cache_test_header}")
target_compile_features(vita-cpu-cache-tests PRIVATE cxx_std_20)
target_include_directories(vita-cpu-cache-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME vita-cpu-cache-io COMMAND vita-cpu-cache-tests)

add_executable(vita-cpu-replay-tape-tests cpu_replay_tape_tests.cpp)
target_compile_features(vita-cpu-replay-tape-tests PRIVATE cxx_std_20)
target_include_directories(vita-cpu-replay-tape-tests PRIVATE ../../src)
get_filename_component(cpu_outer_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(cpu_init_test_header "${CMAKE_CURRENT_BINARY_DIR}/cpu_gxm_init.inc")
add_custom_command(OUTPUT "${cpu_init_test_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_cpu_init_test.py"
        --workspace "${cpu_outer_root}" --output "${cpu_init_test_header}"
    DEPENDS ../../tools/vita_asset_prep/extract_cpu_init_test.py ../../dependencies/vita/vitagl-cpu-init.patch
    VERBATIM)
add_executable(vita-cpu-gxm-init-tests cpu_gxm_init_tests.cpp "${cpu_init_test_header}")
target_compile_features(vita-cpu-gxm-init-tests PRIVATE cxx_std_20)
target_include_directories(vita-cpu-gxm-init-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME vita-cpu-gxm-startup-failures COMMAND vita-cpu-gxm-init-tests)
set(registry_test_header "${CMAKE_CURRENT_BINARY_DIR}/registry_storage_methods.inc")
add_custom_command(OUTPUT "${registry_test_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_registry_storage_test.py"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vita/vita_source_registry.cpp" --output "${registry_test_header}"
    DEPENDS ../../tools/vita_asset_prep/extract_registry_storage_test.py ../../src/vita/vita_source_registry.cpp
    VERBATIM)
add_executable(vita-registry-storage-tests registry_storage_tests.cpp "${registry_test_header}")
target_compile_features(vita-registry-storage-tests PRIVATE cxx_std_20)
target_compile_definitions(vita-registry-storage-tests PRIVATE KISAK_VITA_CPU_RUNTIME=1 KISAK_VITA_CPU_TEST=1)
target_include_directories(vita-registry-storage-tests PRIVATE ../../src "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME vita-registry-fragmented-storage COMMAND vita-registry-storage-tests)
set(cpu_replay_fixture "${cpu_outer_root}/build/cpu-preservation/cpu-source-replay.bin")
add_custom_target(vita-cpu-replay-fixture
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/cpu_replay_tape.py"
        --workspace "${cpu_outer_root}"
    BYPRODUCTS "${cpu_replay_fixture}" VERBATIM)
add_dependencies(vita-cpu-replay-tape-tests vita-cpu-replay-fixture)
add_test(NAME vita-cpu-replay-input-tape COMMAND vita-cpu-replay-tape-tests "${cpu_replay_fixture}")

add_executable(vita-vertex-pack-tests vertex_pack_tests.cpp)
target_compile_features(vita-vertex-pack-tests PRIVATE cxx_std_17)
target_include_directories(vita-vertex-pack-tests PRIVATE ../../src)
add_test(NAME vita-vertex-pack-parity COMMAND vita-vertex-pack-tests)

add_executable(vita-cpu-crc-tests cpu_crc_tests.cpp ../../src/vita/kvap_reader.cpp)
target_compile_features(vita-cpu-crc-tests PRIVATE cxx_std_20)
target_compile_definitions(vita-cpu-crc-tests PRIVATE KISAK_VITA_CPU_RUNTIME=1)
target_include_directories(vita-cpu-crc-tests PRIVATE ../../src)
add_test(NAME vita-cpu-crc-parity COMMAND vita-cpu-crc-tests)
set_tests_properties(vita-cpu-crc-parity PROPERTIES TIMEOUT 60)

set(shipment_brush_fixture "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/cpu-preservation/shipment-collision-v16-test.bin")
if(EXISTS "${shipment_brush_fixture}")
    add_test(NAME vita-shipment-brush-group-parity COMMAND vita-trace-brush-tests "${shipment_brush_fixture}")
    set_tests_properties(vita-shipment-brush-group-parity PROPERTIES TIMEOUT 60)
endif()

set(skin_math_header "${CMAKE_CURRENT_BINARY_DIR}/source_skin_math.inc")
add_custom_command(OUTPUT "${skin_math_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_skin_math_test.py"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/../../src" --output "${skin_math_header}"
    DEPENDS ../../tools/vita_asset_prep/extract_skin_math_test.py
        ../../src/gfx_d3d/r_model_skin_cpu.cpp ../../src/universal/com_math.cpp ../../src/qcommon/com_pack.cpp
        ../../src/vita/vita_source_model_backend.cpp
    VERBATIM)
add_executable(vita-skin-math-tests skin_math_tests.cpp "${skin_math_header}")
target_compile_features(vita-skin-math-tests PRIVATE cxx_std_20)
target_include_directories(vita-skin-math-tests PRIVATE ../../src "${CMAKE_CURRENT_BINARY_DIR}")
if(NOT MSVC)
    target_compile_options(vita-skin-math-tests PRIVATE -fno-strict-aliasing -ffp-contract=off)
endif()
add_test(NAME vita-source-skin-math-parity COMMAND vita-skin-math-tests)
set_tests_properties(vita-source-skin-math-parity PROPERTIES TIMEOUT 60)

set(cpu_recorder_header "${CMAKE_CURRENT_BINARY_DIR}/cpu_recorder_writer.inc")
add_custom_command(OUTPUT "${cpu_recorder_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_cpu_recorder_test.py"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vita/vita_cpu_profile.cpp" --output "${cpu_recorder_header}"
    DEPENDS ../../tools/vita_asset_prep/extract_cpu_recorder_test.py ../../tools/vita_asset_prep/cpu_recorder_contract.py
        ../../src/vita/vita_cpu_profile.cpp VERBATIM)
add_executable(vita-cpu-recorder-tests cpu_recorder_tests.cpp "${cpu_recorder_header}")
target_compile_features(vita-cpu-recorder-tests PRIVATE cxx_std_20)
target_include_directories(vita-cpu-recorder-tests PRIVATE ../../src "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(vita-cpu-recorder-tests PRIVATE Threads::Threads vita-recorder-zlib)
add_test(NAME vita-cpu-recorder-storage-stalls COMMAND vita-cpu-recorder-tests)
set_tests_properties(vita-cpu-recorder-storage-stalls PROPERTIES TIMEOUT 30)

set(dynamic_upload_header "${CMAKE_CURRENT_BINARY_DIR}/source_dynamic_upload.inc")
add_custom_command(OUTPUT "${dynamic_upload_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_dynamic_upload_test.py"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp" --output "${dynamic_upload_header}"
    DEPENDS ../../tools/vita_asset_prep/extract_dynamic_upload_test.py ../../src/gfx_vita/gfx_vita_backend.cpp VERBATIM)
add_executable(vita-dynamic-upload-tests dynamic_upload_tests.cpp "${dynamic_upload_header}")
target_compile_features(vita-dynamic-upload-tests PRIVATE cxx_std_20)
target_include_directories(vita-dynamic-upload-tests PRIVATE ../../src "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME vita-dynamic-upload-parity COMMAND vita-dynamic-upload-tests)

add_executable(vita-display-probe-tests display_probe_tests.cpp)
target_compile_features(vita-display-probe-tests PRIVATE cxx_std_20)
target_include_directories(vita-display-probe-tests PRIVATE ../../src)
target_link_libraries(vita-display-probe-tests PRIVATE Threads::Threads)
add_test(NAME vita-display-queue-contract COMMAND vita-display-probe-tests)

add_executable(vita-texture-prefetch-tests texture_prefetch_tests.cpp)
target_compile_features(vita-texture-prefetch-tests PRIVATE cxx_std_20)
target_include_directories(vita-texture-prefetch-tests PRIVATE ../../src)
add_test(NAME vita-texture-prefetch-contract COMMAND vita-texture-prefetch-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-gpu-draw-probe-tests gpu_draw_probe_tests.cpp)
target_compile_features(vita-gpu-draw-probe-tests PRIVATE cxx_std_20)
target_include_directories(vita-gpu-draw-probe-tests PRIVATE ../../src)
add_test(NAME vita-gpu-draw-probe-contract COMMAND vita-gpu-draw-probe-tests)

add_executable(vita-color-fast-tests color_fast_tests.cpp)
target_compile_features(vita-color-fast-tests PRIVATE cxx_std_20)
target_include_directories(vita-color-fast-tests PRIVATE ../../src)
add_test(NAME vita-color-fast-contract COMMAND vita-color-fast-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-weapon-opaque-tests weapon_opaque_tests.cpp)
target_compile_features(vita-weapon-opaque-tests PRIVATE cxx_std_20)
target_include_directories(vita-weapon-opaque-tests PRIVATE ../../src)
add_test(NAME vita-weapon-opaque-contract COMMAND vita-weapon-opaque-tests)

add_executable(vita-hud-pipeline-tests hud_pipeline_tests.cpp)
target_compile_features(vita-hud-pipeline-tests PRIVATE cxx_std_20)
target_include_directories(vita-hud-pipeline-tests PRIVATE ../../src)
add_test(NAME vita-hud-pipeline-contract COMMAND vita-hud-pipeline-tests)

add_executable(vita-early-alpha-tests early_alpha_tests.cpp)
target_compile_features(vita-early-alpha-tests PRIVATE cxx_std_20)
target_include_directories(vita-early-alpha-tests PRIVATE ../../src)
add_test(NAME vita-early-alpha-contract COMMAND vita-early-alpha-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-weapon-pipeline-tests weapon_pipeline_tests.cpp)
target_compile_features(vita-weapon-pipeline-tests PRIVATE cxx_std_20)
target_include_directories(vita-weapon-pipeline-tests PRIVATE ../../src)
add_test(NAME vita-weapon-pipeline-contract COMMAND vita-weapon-pipeline-tests)

add_executable(vita-weapon-vertex-tests weapon_vertex_tests.cpp)
target_compile_features(vita-weapon-vertex-tests PRIVATE cxx_std_20)
target_include_directories(vita-weapon-vertex-tests PRIVATE ../../src)
add_test(NAME vita-weapon-vertex-contract COMMAND vita-weapon-vertex-tests)

add_executable(vita-weapon-specular-tests weapon_specular_tests.cpp)
target_compile_features(vita-weapon-specular-tests PRIVATE cxx_std_20)
target_include_directories(vita-weapon-specular-tests PRIVATE ../../src)
add_test(NAME vita-weapon-specular-contract COMMAND vita-weapon-specular-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-world-alpha-tests world_alpha_tests.cpp)
target_compile_features(vita-world-alpha-tests PRIVATE cxx_std_20)
target_include_directories(vita-world-alpha-tests PRIVATE ../../src)
add_test(NAME vita-world-alpha-contract COMMAND vita-world-alpha-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-post-film-tests post_film_tests.cpp)
target_compile_features(vita-post-film-tests PRIVATE cxx_std_20)
target_include_directories(vita-post-film-tests PRIVATE ../../src)
add_test(NAME vita-post-film-contract COMMAND vita-post-film-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-world-lifetime-tests world_lifetime_tests.cpp)
target_compile_features(vita-world-lifetime-tests PRIVATE cxx_std_20)
target_include_directories(vita-world-lifetime-tests PRIVATE ../../src)
add_test(NAME vita-world-lifetime-contract COMMAND vita-world-lifetime-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

# The renderer parity matrix injects over30seconds of intentional stalls.
set_tests_properties(vita-cpu-render-ownership PROPERTIES TIMEOUT 60)

add_executable(vita-resource-audit-tests resource_audit_tests.cpp)
target_compile_features(vita-resource-audit-tests PRIVATE cxx_std_20)
target_include_directories(vita-resource-audit-tests PRIVATE ../../src)
add_test(NAME vita-resource-audit-contract COMMAND vita-resource-audit-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../../.tools/vitaGL/source/shaders/precompiled_clear_f.h")

add_executable(vita-lightmap-layout-tests lightmap_layout_tests.cpp)
target_compile_features(vita-lightmap-layout-tests PRIVATE cxx_std_20)
target_include_directories(vita-lightmap-layout-tests PRIVATE ../../src)
add_test(NAME vita-lightmap-layout-contract COMMAND vita-lightmap-layout-tests)


add_executable(vita-world-math-tests world_math_tests.cpp)
target_compile_features(vita-world-math-tests PRIVATE cxx_std_20)
target_include_directories(vita-world-math-tests PRIVATE ../../src)
add_test(NAME vita-world-math-contract COMMAND vita-world-math-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-world-basis-tests world_basis_tests.cpp)
target_compile_features(vita-world-basis-tests PRIVATE cxx_std_20)
target_include_directories(vita-world-basis-tests PRIVATE ../../src)
add_test(NAME vita-world-basis-contract COMMAND vita-world-basis-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-world-gloss-tests world_gloss_tests.cpp)
target_compile_features(vita-world-gloss-tests PRIVATE cxx_std_20)
target_include_directories(vita-world-gloss-tests PRIVATE ../../src)
add_test(NAME vita-world-gloss-contract COMMAND vita-world-gloss-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-world-components-tests world_components_tests.cpp)
target_compile_features(vita-world-components-tests PRIVATE cxx_std_20)
target_include_directories(vita-world-components-tests PRIVATE ../../src)
add_test(NAME vita-world-components-contract COMMAND vita-world-components-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-graphics-profile-tests graphics_profile_tests.cpp)
target_compile_features(vita-graphics-profile-tests PRIVATE cxx_std_20)
target_include_directories(vita-graphics-profile-tests PRIVATE ../../src)
add_test(NAME vita-graphics-profile-contract COMMAND vita-graphics-profile-tests)

add_executable(vita-composition-tests composition_tests.cpp)
target_compile_features(vita-composition-tests PRIVATE cxx_std_20)
target_include_directories(vita-composition-tests PRIVATE ../../src)
add_test(NAME vita-composition-contract COMMAND vita-composition-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-pass-isolation-tests pass_isolation_tests.cpp)
target_compile_features(vita-pass-isolation-tests PRIVATE cxx_std_20)
target_include_directories(vita-pass-isolation-tests PRIVATE ../../src)
add_test(NAME vita-pass-isolation-contract COMMAND vita-pass-isolation-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-temp-hunk-tests temp_hunk_tests.cpp)
target_compile_features(vita-temp-hunk-tests PRIVATE cxx_std_20)
target_include_directories(vita-temp-hunk-tests PRIVATE ../../src)
add_test(NAME vita-temp-hunk-failure-recovery COMMAND vita-temp-hunk-tests)

add_executable(vita-world-precision-tests world_precision_tests.cpp)
target_compile_features(vita-world-precision-tests PRIVATE cxx_std_20)
target_include_directories(vita-world-precision-tests PRIVATE ../../src)
add_test(NAME vita-world-precision-contract COMMAND vita-world-precision-tests "${CMAKE_CURRENT_SOURCE_DIR}/../../src/gfx_vita/gfx_vita_backend.cpp")

add_executable(vita-presented-frame-rate-tests presented_frame_rate_tests.cpp)
target_compile_features(vita-presented-frame-rate-tests PRIVATE cxx_std_20)
target_include_directories(vita-presented-frame-rate-tests PRIVATE ../../src)
add_test(NAME vita-presented-frame-rate COMMAND vita-presented-frame-rate-tests)

set(initial_join_header "${CMAKE_CURRENT_BINARY_DIR}/source_initial_join.inc")
add_custom_command(OUTPUT "${initial_join_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_cpu_optimization_test.py"
        --kind initial-join --source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vita/vita_source_session.cpp" --output "${initial_join_header}"
    DEPENDS ../../tools/vita_asset_prep/extract_cpu_optimization_test.py ../../src/vita/vita_source_session.cpp
    VERBATIM)
add_executable(vita-initial-join-tests initial_join_tests.cpp "${initial_join_header}")
target_compile_features(vita-initial-join-tests PRIVATE cxx_std_20)
target_include_directories(vita-initial-join-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME vita-source-initial-join COMMAND vita-initial-join-tests)

set(primary_light_header "${CMAKE_CURRENT_BINARY_DIR}/source_primary_lights.inc")
add_custom_command(OUTPUT "${primary_light_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_cpu_optimization_test.py"
        --kind primary-lights --source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vita/vita_bootstrap.cpp" --output "${primary_light_header}"
    DEPENDS ../../tools/vita_asset_prep/extract_cpu_optimization_test.py ../../src/vita/vita_bootstrap.cpp
    VERBATIM)
target_sources(vita-map-payload-probe PRIVATE "${primary_light_header}")
target_include_directories(vita-map-payload-probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Decode real retained packs through the same bounded worker as the Vita.
# The direct-file probe alone cannot detect worker/window regressions.
get_target_property(map_probe_sources vita-map-payload-probe SOURCES)
add_executable(vita-map-payload-worker-probe ${map_probe_sources}
    cpu_platform/kernel.cpp ../../src/vita/vita_cpu_file.cpp)
target_compile_features(vita-map-payload-worker-probe PRIVATE cxx_std_20)
target_compile_definitions(vita-map-payload-worker-probe PRIVATE
    KISAK_VITA_CPU_RUNTIME=1 KISAK_VITA_CPU_TEST=1 KISAK_VITA_MAP_WORKER_READ=1)
target_include_directories(vita-map-payload-worker-probe PRIVATE
    cpu_platform frontend_platform ../../src "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(vita-map-payload-worker-probe PRIVATE Threads::Threads)

set(package_probe_header "${CMAKE_CURRENT_BINARY_DIR}/source_package_probe.inc")
add_custom_command(OUTPUT "${package_probe_header}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/vita_asset_prep/extract_cpu_optimization_test.py"
        --kind package-probe --source "${CMAKE_CURRENT_SOURCE_DIR}/../../src/vita/vita_bootstrap.cpp" --output "${package_probe_header}"
    DEPENDS ../../tools/vita_asset_prep/extract_cpu_optimization_test.py ../../src/vita/vita_bootstrap.cpp
    VERBATIM)
list(FILTER map_probe_sources EXCLUDE REGEX "map_payload_probe.cpp$")
add_executable(vita-map-launch-probe ${map_probe_sources} map_launch_probe.cpp "${package_probe_header}"
    cpu_platform/kernel.cpp ../../src/vita/vita_cpu_file.cpp)
target_compile_features(vita-map-launch-probe PRIVATE cxx_std_20)
target_compile_definitions(vita-map-launch-probe PRIVATE KISAK_VITA_CPU_RUNTIME=1 KISAK_VITA_CPU_TEST=1
    KISAK_VITA_SOURCE_RUNTIME=1 KISAK_VITA_EXTERNAL_CONTENT=1)
target_include_directories(vita-map-launch-probe PRIVATE
    cpu_platform frontend_platform ../../src "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(vita-map-launch-probe PRIVATE Threads::Threads)
