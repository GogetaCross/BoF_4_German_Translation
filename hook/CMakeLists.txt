cmake_minimum_required(VERSION 3.15)
project(bof4_d3d9_hook_titlemenu LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# BOF4.exe is 32-bit — this DLL must be 32-bit too.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "BOF4.exe is 32-bit — configure with -A Win32:\n"
        "  cmake -S . -B build -A Win32")
endif()

# ── Vendored MinHook (inline-hooking library) ─────────────────────────────
# https://github.com/TsudaKageyu/minhook  — BSD-2-Clause-ish
# We only vendor the 32-bit HDE files; MinHook auto-selects hde32 for x86
# builds. All files live under src/minhook/.
set(MINHOOK_SOURCES
    src/minhook/buffer.c
    src/minhook/hook.c
    src/minhook/trampoline.c
    src/minhook/hde/hde32.c
)

# ── Vendored Dear ImGui v1.92.9 (MIT) + DX9/Win32 backends ────────────────
# https://github.com/ocornut/imgui  — powers the in-game config overlay
# (src/config_overlay.cpp). Pinned commit in src/imgui/IMGUI_COMMIT.txt.
set(IMGUI_SOURCES
    src/imgui/imgui.cpp
    src/imgui/imgui_draw.cpp
    src/imgui/imgui_tables.cpp
    src/imgui/imgui_widgets.cpp
    src/imgui/backends/imgui_impl_dx9.cpp
    src/imgui/backends/imgui_impl_win32.cpp
)

add_library(d3d9 SHARED
    src/dllmain.cpp
    src/config_overlay.cpp
    ${IMGUI_SOURCES}
    src/hooks.cpp
    src/image_io.cpp
    src/bof4_hooks.cpp
    src/bgm_loop.cpp
    src/text_subst.cpp
    src/text_resolver.cpp
    src/item_name_uncap.cpp
    src/save_anywhere.cpp
    src/hook_health.cpp
    src/subtitles.cpp
    src/ff_overlay.cpp
    src/pause_freeze.cpp
    src/free_camera.cpp
    src/menu_rect_fix.cpp
    src/title_jp.cpp
    src/rect_hook.cpp
    src/rect_tuner.cpp
    src/hang_watchdog.cpp
    src/tex_prov.cpp
    src/glyph_log.cpp
    src/label_center.cpp
    src/box_autofit.cpp
    src/cmdbox_width.cpp
    src/cmdbox_asm.cpp
    src/cmdbox_labels.cpp
    src/recon_scan.cpp
    src/name_slot_bp.cpp
    src/item_name_msg_uncap.cpp
    src/equip_menu_shift.cpp
    src/censor_fix_aream031.cpp
    src/narration_uncap.cpp
    src/dengeki_unlock.cpp
    ${MINHOOK_SOURCES}
)

target_include_directories(d3d9 PRIVATE
    src/minhook src/minhook/hde
    src/imgui src/imgui/backends)

set_target_properties(d3d9 PROPERTIES
    OUTPUT_NAME "d3d9"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_link_libraries(d3d9 PRIVATE kernel32 user32 gdi32 winmm dwmapi)

if(MSVC)
    # Force exports via .def file (avoids name mangling / decorated names)
    target_link_options(d3d9 PRIVATE
        "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/d3d9.def"
    )
    target_compile_options(d3d9 PRIVATE /W3 /EHsc)
    target_compile_definitions(d3d9 PRIVATE
        $<$<CONFIG:Release>:NDEBUG>
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    # MinHook's C sources trigger warnings on /W4+; keep them quiet without
    # affecting our C++ code.
    set_source_files_properties(${MINHOOK_SOURCES}
        PROPERTIES COMPILE_FLAGS "/W0")
    # Vendored ImGui: don't police third-party warnings.
    set_source_files_properties(${IMGUI_SOURCES}
        PROPERTIES COMPILE_FLAGS "/W0")
endif()
