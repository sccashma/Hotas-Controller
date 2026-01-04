cmake_minimum_required(VERSION 3.25)
project(hotas_controller LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_STATIC "Build static executable" OFF)

if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/permissive- /W4 /EHsc /Zc:preprocessor)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -O2)
endif()

if(BUILD_STATIC AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)

# Fetch ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    # Pin to docking branch at v1.91.0 tag (matching ImPlot 0.16 expectations) by using the tag name which exists on docking too.
    GIT_TAG v1.91.0-docking
)
# Fetch ImPlot
FetchContent_Declare(
  implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG v0.16
)

FetchContent_MakeAvailable(imgui implot)

# Fetch nlohmann/json for JSON profile persistence
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
)
## Avoid running the external library's CMake which may require an older
## CMake policy. Populate only and use its include dir.
FetchContent_Populate(nlohmann_json)
set(nlohmann_json_SOURCE_DIR "${nlohmann_json_SOURCE_DIR}")

# ImGui sources (core + backends for Win32 + DX11)
set(IMGUI_SRC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)

set(IMPLOT_SRC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)

add_library(imgui_lib STATIC ${IMGUI_SRC} ${IMPLOT_SRC})
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${implot_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

set(APP_SOURCES
    src/main.cpp
    src/core/ring_buffer.hpp
    src/xinput/xinput_poll.cpp
    src/xinput/xinput_poll.hpp
    src/xinput/hotas_reader.cpp
    src/xinput/hotas_reader.hpp
    src/xinput/hotas_mapper.cpp
    src/xinput/hotas_mapper.hpp
    src/xinput/filtered_forwarder.hpp
    src/ui/plots_panel.cpp
    src/ui/plots_panel.hpp
)

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/res/version.rc)
    list(APPEND APP_SOURCES res/version.rc)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest)
    # Add manifest as a source so CMake tracks it; embed explicitly via linker flag
    list(APPEND APP_SOURCES res/app.manifest)
endif()

add_executable(${PROJECT_NAME} ${APP_SOURCES})

if (MSVC)
    # Ensure we link with /SUBSYSTEM:WINDOWS so WinMain is entry point
    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS)
    target_compile_options(${PROJECT_NAME} PRIVATE /sdl /guard:cf)
    # Linker security flags (Control Flow Guard, dynamic base, NX compat are default but be explicit)
    target_link_options(${PROJECT_NAME} PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest)
        # Use the provided manifest instead of auto generated one
    target_link_options(${PROJECT_NAME} PRIVATE 
            "/MANIFEST:EMBED" 
            "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest" 
            "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
        )
    endif()
endif()

add_subdirectory(external/ViGEmClient)

# Require Lunasvg submodule for SVG rendering
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/lunasvg/CMakeLists.txt)
    add_subdirectory(external/lunasvg)
else()
    message(FATAL_ERROR "Lunasvg submodule not found at external/lunasvg. Initialize submodules: git submodule update --init --recursive")
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE imgui_lib d3d11 dxgi ViGEmClient setupapi Windowscodecs lunasvg)

target_include_directories(${PROJECT_NAME} PRIVATE external/ViGEmClient/include)

target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_ENABLE_DOCKING)

target_include_directories(${PROJECT_NAME} PRIVATE src)
target_include_directories(${PROJECT_NAME} PRIVATE ${nlohmann_json_SOURCE_DIR}/include)

# Silence warnings from external libraries to keep build output actionable
if (MSVC)
    foreach(tgt IN ITEMS ViGEmClient lunasvg plutovg)
        if (TARGET ${tgt})
            target_compile_options(${tgt} PRIVATE /W0)
        endif()
    endforeach()
endif()

# Enable higher optimization for release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Copy configuration files next to the built binary in a 'config' folder
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/res/config"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config"
)

# Copy graphics assets next to the executable for portable runs (single ./graphics folder)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/graphics"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/res/graphics"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/graphics"
)
