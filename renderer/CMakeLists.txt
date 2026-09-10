# Renderer layer: Vulkan present + texture upload. CPU draw types live in wds_draw.
# Chart stage/skin helpers live in wds_chart_render.

# --- Vulkan (headers vendored; loader/import lib looked up per target) ---
set(Vulkan_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
if(EXISTS "/usr/include/vulkan/vulkan.h" AND NOT CMAKE_CROSSCOMPILING)
  set(Vulkan_INCLUDE_DIR "/usr/include")
endif()

set(_wds_vulkan_hints
  ${CMAKE_PREFIX_PATH}
  ${CMAKE_FIND_ROOT_PATH}
  /usr
  /usr/local
)
if(DEFINED ENV{VULKAN_SDK})
  list(APPEND _wds_vulkan_hints "$ENV{VULKAN_SDK}")
endif()

find_library(Vulkan_LIBRARY
  NAMES vulkan vulkan-1 vulkan.1
  HINTS ${_wds_vulkan_hints}
  PATH_SUFFIXES lib lib64
)
if(NOT Vulkan_LIBRARY AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
  set(Vulkan_LIBRARY "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
endif()
if(NOT Vulkan_LIBRARY AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}/lib/vulkan-1.lib")
  set(Vulkan_LIBRARY "$ENV{VULKAN_SDK}/lib/vulkan-1.lib")
endif()
if(NOT Vulkan_LIBRARY AND DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}/lib/libvulkan.1.dylib")
  set(Vulkan_LIBRARY "$ENV{VULKAN_SDK}/lib/libvulkan.1.dylib")
endif()
if(NOT Vulkan_LIBRARY)
  message(FATAL_ERROR
    "libvulkan not found for target '${WDS_TARGET}'.\n"
    "Install a Vulkan loader for this target, or pass -DVulkan_LIBRARY=/path/to/lib,\n"
    "or configure with -DWDS_BUILD_RENDERER=OFF to build core only.\n"
    "Vendored headers are under renderer/third_party/vulkan.")
endif()

add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
set_target_properties(Vulkan::Vulkan PROPERTIES
  IMPORTED_LOCATION "${Vulkan_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}"
)
message(STATUS "Vulkan: ${Vulkan_LIBRARY} (headers: ${Vulkan_INCLUDE_DIR})")

# Seed common dependency prefixes before looking for zlib/png. This keeps a
# plain `cmake ..` usable on the Windows developer machine while still allowing
# CI/cross builds to provide CMAKE_PREFIX_PATH explicitly.
set(_wds_dependency_prefixes ${CMAKE_PREFIX_PATH})
if(DEFINED WDS_VCPKG_ROOT AND NOT WDS_VCPKG_ROOT STREQUAL "")
  set(_wds_vcpkg_root "${WDS_VCPKG_ROOT}")
elseif(DEFINED ENV{WDS_VCPKG_ROOT} AND NOT "$ENV{WDS_VCPKG_ROOT}" STREQUAL "")
  set(_wds_vcpkg_root "$ENV{WDS_VCPKG_ROOT}")
elseif(WIN32 AND EXISTS "C:/SDK/vcpkg/vcpkg")
  set(_wds_vcpkg_root "C:/SDK/vcpkg/vcpkg")
endif()
if(DEFINED _wds_vcpkg_root AND EXISTS "${_wds_vcpkg_root}/installed")
  if(DEFINED WDS_VCPKG_TRIPLET AND NOT WDS_VCPKG_TRIPLET STREQUAL "")
    set(_wds_vcpkg_triplet "${WDS_VCPKG_TRIPLET}")
  elseif(DEFINED ENV{WDS_VCPKG_TRIPLET} AND NOT "$ENV{WDS_VCPKG_TRIPLET}" STREQUAL "")
    set(_wds_vcpkg_triplet "$ENV{WDS_VCPKG_TRIPLET}")
  elseif(WIN32)
    set(_wds_vcpkg_triplet "x64-windows-static-md")
  else()
    set(_wds_vcpkg_triplet "x64-mingw-static")
  endif()
  if(EXISTS "${_wds_vcpkg_root}/installed/${_wds_vcpkg_triplet}")
    list(PREPEND _wds_dependency_prefixes
      "${_wds_vcpkg_root}/installed/${_wds_vcpkg_triplet}")
  endif()
endif()
if(_wds_dependency_prefixes)
  list(REMOVE_DUPLICATES _wds_dependency_prefixes)
  list(PREPEND CMAKE_PREFIX_PATH ${_wds_dependency_prefixes})
  set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" CACHE PATH "Dependency package prefixes" FORCE)
endif()

# vcpkg MinGW static zlib installs as libzs.a (not libz.a). Prefer its CONFIG
# package and seed the FindZLIB variables so FindPNG / PNGConfig succeed.
find_package(ZLIB CONFIG QUIET)
if(TARGET ZLIB::ZLIBSTATIC)
  get_target_property(_wds_zlib_loc ZLIB::ZLIBSTATIC IMPORTED_LOCATION_RELEASE)
  if(NOT _wds_zlib_loc)
    get_target_property(_wds_zlib_loc ZLIB::ZLIBSTATIC IMPORTED_LOCATION)
  endif()
  get_target_property(_wds_zlib_inc ZLIB::ZLIBSTATIC INTERFACE_INCLUDE_DIRECTORIES)
  if(_wds_zlib_loc)
    set(ZLIB_LIBRARY "${_wds_zlib_loc}" CACHE FILEPATH "zlib library" FORCE)
    set(ZLIB_LIBRARIES "${ZLIB_LIBRARY}")
  endif()
  if(_wds_zlib_inc)
    set(ZLIB_INCLUDE_DIR "${_wds_zlib_inc}" CACHE PATH "zlib include" FORCE)
    set(ZLIB_INCLUDE_DIRS "${ZLIB_INCLUDE_DIR}")
  endif()
  set(ZLIB_FOUND TRUE)
  if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS ZLIB::ZLIBSTATIC)
  endif()
endif()
if(NOT ZLIB_FOUND)
  find_package(ZLIB REQUIRED)
endif()

find_package(PNG CONFIG QUIET)
if(NOT TARGET PNG::PNG)
  find_package(PNG REQUIRED)
endif()

# Headers and the linked dylib must be the same libpng major line. A 1.4 header
# with a 1.6 dylib (common when /opt/X11 precedes Homebrew) makes every
# png_create_read_struct fail → skin atlas load fails at startup.
set(_wds_png_ver "")
if(DEFINED PNG_VERSION_STRING AND PNG_VERSION_STRING)
  set(_wds_png_ver "${PNG_VERSION_STRING}")
endif()
if(NOT _wds_png_ver AND TARGET PNG::PNG)
  get_target_property(_wds_png_inc PNG::PNG INTERFACE_INCLUDE_DIRECTORIES)
  if(_wds_png_inc)
    foreach(_wds_png_inc_dir IN LISTS _wds_png_inc)
      if(EXISTS "${_wds_png_inc_dir}/png.h")
        file(STRINGS "${_wds_png_inc_dir}/png.h" _wds_png_ver_line
             REGEX "^#define[ \t]+PNG_LIBPNG_VER_STRING[ \t]+")
        if(_wds_png_ver_line)
          string(REGEX REPLACE "^#define[ \t]+PNG_LIBPNG_VER_STRING[ \t]+\"([^\"]+)\".*"
                 "\\1" _wds_png_ver "${_wds_png_ver_line}")
          break()
        endif()
      endif()
    endforeach()
  endif()
endif()
message(STATUS "PNG: version=${_wds_png_ver} include=${PNG_PNG_INCLUDE_DIR} lib=${PNG_LIBRARY}")
if(_wds_png_ver AND _wds_png_ver VERSION_LESS "1.6")
  message(FATAL_ERROR
    "libpng >= 1.6 required for skin PNG loading (found ${_wds_png_ver} via "
    "include=${PNG_PNG_INCLUDE_DIR}). On macOS: brew install libpng, and ensure "
    "CMAKE_PREFIX_PATH prefers Homebrew over /opt/X11.")
endif()
unset(_wds_png_ver)
unset(_wds_png_inc)
unset(_wds_png_inc_dir)
unset(_wds_png_ver_line)

# Vendored binary is Linux x86_64; only use it when it actually runs on this host.
set(_wds_vendored_glslang "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bin/glslangValidator")
unset(WDS_GLSLANG_VALIDATOR CACHE)
unset(WDS_GLSLANG_VALIDATOR)
if(EXISTS "${_wds_vendored_glslang}")
  execute_process(
    COMMAND "${_wds_vendored_glslang}" --version
    RESULT_VARIABLE _wds_glslang_ok
    OUTPUT_QUIET
    ERROR_QUIET
  )
  if(_wds_glslang_ok EQUAL 0)
    set(WDS_GLSLANG_VALIDATOR "${_wds_vendored_glslang}")
  endif()
endif()
if(NOT WDS_GLSLANG_VALIDATOR)
  find_program(WDS_GLSLANG_VALIDATOR NAMES glslangValidator glslang)
endif()
unset(_wds_vendored_glslang)
unset(_wds_glslang_ok)

set(WDS_SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(WDS_SHADER_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${WDS_SHADER_BIN_DIR}")

set(WDS_SPV_FILES
  "${WDS_SHADER_BIN_DIR}/textured_quad.vert.spv"
  "${WDS_SHADER_BIN_DIR}/textured_quad.frag.spv"
)

if(WDS_GLSLANG_VALIDATOR)
  add_custom_command(
    OUTPUT "${WDS_SHADER_BIN_DIR}/textured_quad.vert.spv"
    COMMAND "${WDS_GLSLANG_VALIDATOR}" -V "${WDS_SHADER_SRC_DIR}/textured_quad.vert"
            -o "${WDS_SHADER_BIN_DIR}/textured_quad.vert.spv"
    DEPENDS "${WDS_SHADER_SRC_DIR}/textured_quad.vert"
    COMMENT "Compiling textured_quad.vert"
  )
  add_custom_command(
    OUTPUT "${WDS_SHADER_BIN_DIR}/textured_quad.frag.spv"
    COMMAND "${WDS_GLSLANG_VALIDATOR}" -V "${WDS_SHADER_SRC_DIR}/textured_quad.frag"
            -o "${WDS_SHADER_BIN_DIR}/textured_quad.frag.spv"
    DEPENDS "${WDS_SHADER_SRC_DIR}/textured_quad.frag"
    COMMENT "Compiling textured_quad.frag"
  )
  add_custom_target(wds_shaders DEPENDS ${WDS_SPV_FILES})
else()
  message(FATAL_ERROR "glslangValidator not found; cannot compile Vulkan shaders")
endif()

# CPU-only draw primitives (no Vulkan). Safe for interaction / headless consumers.
add_library(wds_draw INTERFACE)
add_library(wds::draw ALIAS wds_draw)
target_include_directories(wds_draw INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wds_draw INTERFACE cxx_std_17)

add_library(wds_renderer STATIC
  src/texture.cpp
  src/vulkan_renderer.cpp
)

add_library(wds::renderer ALIAS wds_renderer)
add_dependencies(wds_renderer wds_shaders)

target_include_directories(wds_renderer
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(wds_renderer
  PUBLIC
    wds::draw
    wds::common
    Vulkan::Vulkan
  PRIVATE
    PNG::PNG
)

target_compile_features(wds_renderer PUBLIC cxx_std_17)
target_compile_definitions(wds_renderer PRIVATE
  WDS_SHADER_DIR="${WDS_SHADER_BIN_DIR}"
)
if(NOT MSVC)
  set_source_files_properties(src/vulkan_renderer.cpp PROPERTIES
    COMPILE_FLAGS "-Wno-missing-field-initializers")
endif()

if(MSVC)
  target_compile_options(wds_renderer PRIVATE /W4)
else()
  target_compile_options(wds_renderer PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Native default ON. Cross builds still compile the binary; host ctest cannot
# execute a foreign PE, so add_test is skipped there.
option(WDS_RENDERER_BUILD_TESTS "Build renderer unit tests" ON)
if(WDS_RENDERER_BUILD_TESTS)
  set(_wds_renderer_test_sources tests/renderer_logic_tests.cpp)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/texture_cache_tests.cpp")
    list(APPEND _wds_renderer_test_sources tests/texture_cache_tests.cpp)
  endif()

  add_executable(wds_renderer_tests ${_wds_renderer_test_sources})
  target_link_libraries(wds_renderer_tests PRIVATE wds::renderer)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/texture_cache_tests.cpp")
    target_compile_definitions(wds_renderer_tests PRIVATE WDS_HAS_TEXTURE_CACHE_TESTS=1)
  endif()
  if(MSVC)
    target_compile_options(wds_renderer_tests PRIVATE /W4 /utf-8)
  else()
    target_compile_options(wds_renderer_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME wds_renderer_tests COMMAND wds_renderer_tests)
  else()
    message(STATUS "Skipping ctest registration for wds_renderer_tests (cross build)")
  endif()
endif()

# Renderer benchmarks are intentionally disabled in the editor build; the editor host is Qt.
option(WDS_RENDERER_BUILD_BENCHMARKS "Build renderer path benchmarks" OFF)
