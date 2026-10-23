# Unit Tests for ReXGlue core components

add_executable(unit_tests
    audio/sdl_audio_test.cpp
    memory/heap_allocation_test.cpp
    kernel/object_table_test.cpp
    core/cvar_test.cpp
    core/fiber_test.cpp
    core/sha256_test.cpp
    core/stream_test.cpp
    core/byte_order_test.cpp
    core/chrono_test.cpp
    core/filesystem_test.cpp
    graphics/register_file_test.cpp
    graphics/xsh_dump_test.cpp
    graphics/texture_integer_scale_test.cpp
    graphics/xenos_zpd_report_test.cpp
    codegen/config_test.cpp
    codegen/gapfill_test.cpp
    codegen/template_registry_test.cpp
    ppc/import_function_test.cpp
    ppc/stack_test.cpp
    system/guest_path_test.cpp
    system/function_dispatcher_test.cpp
    system/socket_test.cpp
    ui/renderdoc_test.cpp
    codegen/manifest_test.cpp
    rexglue/migration_scan_test.cpp
    rexglue/ui_test.cpp
)

target_sources(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/rexglue/commands/migration_scan.cpp
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/ui.cpp
    ${PROJECT_SOURCE_DIR}/src/rexglue/ui/progress.cpp
)

target_include_directories(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(unit_tests PRIVATE
    rexcodegen
    rexruntime
    renderdoc
    SDL3::Headers
    xxHash::xxhash
    Catch2::Catch2WithMain
)

# Strip Tracy/profiling instrumentation from test targets
target_compile_options(unit_tests PRIVATE
    -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
    -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
)

set_target_properties(unit_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
if(UNIX AND NOT APPLE)
    set_target_properties(unit_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()

# Discover tests
include(Catch)
catch_discover_tests(unit_tests
    TEST_PREFIX "unit."
    PROPERTIES
        LABELS "unit"
        TIMEOUT 60
)
