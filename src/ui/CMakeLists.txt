# rexui

# Core UI sources
set(REXUI_CORE_SOURCES
    graphics_upload_buffer_pool.cpp
    graphics_util.cpp
    hdr_availability.cpp
    imgui_dialog.cpp
    imgui_drawer.cpp
    immediate_drawer.cpp
    keybinds.cpp
    menu_item.cpp
    presenter.cpp
    renderdoc_api.cpp
    window.cpp
    windowed_app.cpp
    windowed_app_context.cpp
)

# Optional overlay dialogs
set(REXUI_OVERLAY_SOURCES
    overlay/debug_overlay.cpp
    overlay/console_overlay.cpp
    overlay/settings_overlay.cpp
)

# Vulkan UI backend
set(REXUI_VULKAN_SOURCES
    vulkan/linked_type_descriptor_set_allocator.cpp
    vulkan/single_layout_descriptor_set_pool.cpp
    vulkan/spirv_tools_context.cpp
    vulkan/ui_samplers.cpp
    vulkan/vulkan_device.cpp
    vulkan/vulkan_immediate_drawer.cpp
    vulkan/vulkan_instance.cpp
    vulkan/vulkan_mem_alloc.cpp
    vulkan/vulkan_presenter.cpp
    vulkan/vulkan_provider.cpp
    vulkan/vulkan_submission_tracker.cpp
    vulkan/vulkan_upload_buffer_pool.cpp
    vulkan/vulkan_util.cpp
    vulkan/vendor_upscalers.cpp
)

# D3D12 UI backend
if(REXGLUE_USE_D3D12)
    set(REXUI_D3D12_SOURCES
        d3d12/d3d12_cpu_descriptor_pool.cpp
        d3d12/d3d12_descriptor_heap_pool.cpp
        d3d12/d3d12_immediate_drawer.cpp
        d3d12/d3d12_presenter.cpp
        d3d12/d3d12_provider.cpp
        d3d12/d3d12_submission_tracker.cpp
        d3d12/d3d12_upload_buffer_pool.cpp
        d3d12/d3d12_util.cpp
    )
endif()

# Platform-specific sources
if(WIN32)
    set(REXUI_PLATFORM_SOURCES
        surface_win.cpp
        window_win.cpp
        windowed_app_context_win.cpp
    )
elseif(APPLE)
    set(REXUI_PLATFORM_SOURCES
        window_sdl.cpp
        windowed_app_context_sdl.cpp
    )
else()
    set(REXUI_PLATFORM_SOURCES
        surface_gnulinux.cpp
        window_gtk.cpp
        windowed_app_context_gtk.cpp
    )
endif()

# Combine sources
set(REXUI_SOURCES
    ${REXUI_CORE_SOURCES}
    ${REXUI_OVERLAY_SOURCES}
    ${REXUI_PLATFORM_SOURCES}
)

if(REXGLUE_USE_VULKAN)
    list(APPEND REXUI_SOURCES ${REXUI_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXUI_SOURCES ${REXUI_D3D12_SOURCES})
endif()

add_library(rexui OBJECT ${REXUI_SOURCES})
add_library(rex::ui ALIAS rexui)

target_include_directories(rexui PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexui PRIVATE
    ${REXGLUE_ROOT}/src  # For ui/shaders/bytecode includes
)

target_link_libraries(rexui PUBLIC
    rexcore
    renderdoc
    imgui::imgui
)

if(REXGLUE_FIDELITYFX_SOURCE_DIR AND EXISTS "${REXGLUE_FIDELITYFX_SOURCE_DIR}/sdk/include")
    target_include_directories(rexui PRIVATE
        "${REXGLUE_FIDELITYFX_SOURCE_DIR}/sdk/include"
        "${REXGLUE_FIDELITYFX_SOURCE_DIR}/ffx-api/include"
    )
    target_compile_definitions(rexui PRIVATE REX_HAS_FIDELITYFX_SDK=1)
endif()

set(REXUI_HAS_FIDELITYFX_RUNTIME OFF)
if(TARGET amd_fidelityfx_vk)
    target_link_libraries(rexui PRIVATE amd_fidelityfx_vk)
    set(REXUI_HAS_FIDELITYFX_RUNTIME ON)
endif()
if(TARGET amd_fidelityfx_dx12)
    target_link_libraries(rexui PRIVATE amd_fidelityfx_dx12)
    set(REXUI_HAS_FIDELITYFX_RUNTIME ON)
endif()
if(REXUI_HAS_FIDELITYFX_RUNTIME)
    target_compile_definitions(rexui PRIVATE REX_HAS_FIDELITYFX_RUNTIME=1)
endif()

# Optional vendor upscalers from SDKs the application fetches itself (they are
# not redistributed with this source). Runtime libraries are listed in
# REXGLUE_VENDOR_UPSCALER_RUNTIMES for the application to place next to its
# executable.
set(REXGLUE_DLSS_SDK_DIR "" CACHE PATH "NVIDIA DLSS SDK root (NGX); enables DLSS Super Resolution")
set(REXGLUE_XESS_SDK_DIR "" CACHE PATH "Intel XeSS SDK root; enables XeSS Super Resolution (Windows)")
set(REXGLUE_VENDOR_UPSCALER_RUNTIMES "" CACHE INTERNAL "")
set(_rex_vendor_runtimes)
if(REXGLUE_USE_VULKAN AND REXGLUE_DLSS_SDK_DIR AND EXISTS "${REXGLUE_DLSS_SDK_DIR}/include/nvsdk_ngx_vk.h")
    target_include_directories(rexui PRIVATE "${REXGLUE_DLSS_SDK_DIR}/include")
    target_compile_definitions(rexui PRIVATE REX_HAS_DLSS=1)
    if(WIN32)
        target_link_libraries(rexui PRIVATE "${REXGLUE_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
        file(GLOB _rex_dlss_runtime "${REXGLUE_DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
    else()
        target_link_libraries(rexui PRIVATE "${REXGLUE_DLSS_SDK_DIR}/lib/Linux_x86_64/libnvsdk_ngx.a" ${CMAKE_DL_LIBS})
        file(GLOB _rex_dlss_runtime "${REXGLUE_DLSS_SDK_DIR}/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.*")
    endif()
    list(APPEND _rex_vendor_runtimes ${_rex_dlss_runtime})
    message(STATUS "  DLSS: ${REXGLUE_DLSS_SDK_DIR}")
endif()
if(WIN32 AND REXGLUE_USE_VULKAN AND REXGLUE_XESS_SDK_DIR AND EXISTS "${REXGLUE_XESS_SDK_DIR}/inc/xess/xess_vk.h")
    # libxess.dll is loaded at runtime; no import library.
    target_include_directories(rexui PRIVATE "${REXGLUE_XESS_SDK_DIR}/inc")
    target_compile_definitions(rexui PRIVATE REX_HAS_XESS=1)
    list(APPEND _rex_vendor_runtimes "${REXGLUE_XESS_SDK_DIR}/bin/libxess.dll")
    message(STATUS "  XeSS: ${REXGLUE_XESS_SDK_DIR}")
endif()
set(REXGLUE_VENDOR_UPSCALER_RUNTIMES "${_rex_vendor_runtimes}" CACHE INTERNAL "")

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexui PUBLIC
        volk::volk
        GPUOpen::VulkanMemoryAllocator
        SPIRV-Tools-static
    )
endif()

# Platform-specific dependencies
if(WIN32)
    add_library(rex_adaptive_sync STATIC adaptive_sync_stub.cpp)
    target_include_directories(rex_adaptive_sync PUBLIC ${REXGLUE_ROOT}/include)
    target_link_libraries(rexui PUBLIC rex_adaptive_sync)
    target_link_libraries(rexui PUBLIC
        dwmapi
        dxgi
        shcore
    )
elseif(APPLE)
    add_library(rex_adaptive_sync STATIC adaptive_sync_stub.cpp)
    target_include_directories(rex_adaptive_sync PUBLIC ${REXGLUE_ROOT}/include)
    target_link_libraries(rexui PUBLIC rex_adaptive_sync SDL3::SDL3
        "-framework QuartzCore" "-framework Metal" "-framework Cocoa")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0>=3.24.24)
    pkg_check_modules(X11_XCB REQUIRED x11-xcb)
    pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
    # Protocol code is generated by the wayland-scanner matching the target's
    # libwayland-client (scanners from 1.20 on call wl_proxy_marshal_flags,
    # which older clients lack); toolchains may preset REX_WAYLAND_SCANNER.
    find_program(REX_WAYLAND_SCANNER wayland-scanner REQUIRED)
    function(rex_wayland_protocol xml name out_header out_code)
        set(protocol_xml ${CMAKE_CURRENT_SOURCE_DIR}/wayland/${xml}.xml)
        set(protocol_header ${CMAKE_CURRENT_BINARY_DIR}/wayland/${name}-client.h)
        set(protocol_code ${CMAKE_CURRENT_BINARY_DIR}/wayland/${name}-protocol.c)
        add_custom_command(OUTPUT ${protocol_header} ${protocol_code}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/wayland
            COMMAND ${REX_WAYLAND_SCANNER} client-header ${protocol_xml} ${protocol_header}
            COMMAND ${REX_WAYLAND_SCANNER} private-code ${protocol_xml} ${protocol_code}
            DEPENDS ${protocol_xml}
            VERBATIM)
        set(${out_header} ${protocol_header} PARENT_SCOPE)
        set(${out_code} ${protocol_code} PARENT_SCOPE)
    endfunction()
    set(REX_ADAPTIVE_SYNC_PROTOCOL_SOURCES)
    foreach(protocol wlr-output-management-unstable-v1 xdg-output-unstable-v1)
        rex_wayland_protocol(${protocol} ${protocol} protocol_header protocol_code)
        list(APPEND REX_ADAPTIVE_SYNC_PROTOCOL_SOURCES ${protocol_header} ${protocol_code})
    endforeach()
    set(REX_SURFACE_PROTOCOL_HEADERS)
    set(REX_SURFACE_PROTOCOL_CODE)
    foreach(protocol "color-management-v1;color-management" "viewporter;viewporter"
                     "fractional-scale;fractional-scale"
                     "pointer-constraints-unstable-v1;pointer-constraints"
                     "relative-pointer-unstable-v1;relative-pointer")
        list(GET protocol 0 protocol_xml_name)
        list(GET protocol 1 protocol_name)
        rex_wayland_protocol(${protocol_xml_name} ${protocol_name} protocol_header protocol_code)
        list(APPEND REX_SURFACE_PROTOCOL_HEADERS ${protocol_header})
        list(APPEND REX_SURFACE_PROTOCOL_CODE ${protocol_code})
    endforeach()
    target_sources(rexui PRIVATE ${REX_SURFACE_PROTOCOL_HEADERS})
    target_include_directories(rexui PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    add_library(rex_adaptive_sync STATIC adaptive_sync_wayland.cpp
        ${REX_ADAPTIVE_SYNC_PROTOCOL_SOURCES})
    set_target_properties(rex_adaptive_sync PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(rex_adaptive_sync PUBLIC ${REXGLUE_ROOT}/include)
    target_include_directories(rex_adaptive_sync PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(rex_adaptive_sync PUBLIC PkgConfig::WAYLAND)
    target_link_libraries(rexui PUBLIC rex_adaptive_sync)
    add_library(rex_hdr_detection STATIC hdr_display.cpp ${REX_SURFACE_PROTOCOL_HEADERS}
        ${REX_SURFACE_PROTOCOL_CODE})
    set_target_properties(rex_hdr_detection PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(rex_hdr_detection PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_include_directories(rex_hdr_detection PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
    target_link_libraries(rex_hdr_detection PUBLIC PkgConfig::WAYLAND)
    target_link_libraries(rexui PUBLIC rex_hdr_detection)

    target_include_directories(rexui PRIVATE
        ${GTK3_INCLUDE_DIRS}
        ${X11_XCB_INCLUDE_DIRS}
    )
    target_link_libraries(rexui PUBLIC
        ${GTK3_LIBRARIES}
        ${X11_XCB_LIBRARIES}
    )
endif()

# D3D12 backend dependencies
if(REXGLUE_USE_D3D12)
    target_link_libraries(rexui PUBLIC
        dxguid
        dxc-headers
    )
endif()
