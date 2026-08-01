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
set_source_files_properties(src/vulkan_renderer.cpp PROPERTIES
  COMPILE_FLAGS "-Wno-missing-field-initializers"
)

if(MSVC)
  target_compile_options(wds_renderer PRIVATE /W4)
else()
  target_compile_options(wds_renderer PRIVATE -Wall -Wextra -Wpedantic)
endif()
