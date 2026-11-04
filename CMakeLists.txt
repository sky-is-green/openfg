cmake_minimum_required(VERSION 3.20)

project(openfg
  DESCRIPTION "Open frame generation for Linux"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-O3 -DNDEBUG -Wall -Wextra)

include_directories(SYSTEM "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")

find_program(GLSLC_EXECUTABLE NAMES glslc REQUIRED)
find_program(GLSLANG_EXECUTABLE NAMES glslangValidator glslang REQUIRED)

set(SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/openfg/shaders")
set(SHADER_BIN "${CMAKE_CURRENT_BINARY_DIR}/openfg-shaders")
file(GLOB SHADER_FILES CONFIGURE_DEPENDS "${SHADER_SRC}/*.comp")
file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS "${SHADER_SRC}/*.glsl")

set(SHADER_SPV)
foreach(shader ${SHADER_FILES})
  get_filename_component(name "${shader}" NAME_WE)
  set(spv "${SHADER_BIN}/${name}.spv")
  add_custom_command(OUTPUT "${spv}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_BIN}"
    COMMAND "${GLSLC_EXECUTABLE}" -O -I"${SHADER_SRC}" -o "${spv}" "${shader}"
    DEPENDS "${shader}" ${SHADER_INCLUDES}
    COMMENT "glslc ${name}.comp")
  list(APPEND SHADER_SPV "${spv}")
endforeach()

# FidelityFX Optical Flow passes. The SDK ships these as GLSL with include
# directives and subgroup intrinsics, so they are compiled with glslang.
set(FFX_SRC "${SHADER_SRC}/ffx")
file(GLOB_RECURSE FFX_HEADERS CONFIGURE_DEPENDS "${FFX_SRC}/*.h")
set(FFX_PASSES
  "ffx_prepare_luma:${FFX_SRC}/passes/ffx_opticalflow_prepare_luma_pass.glsl"
  "ffx_luminance_pyramid:${FFX_SRC}/passes/ffx_opticalflow_compute_luminance_pyramid_pass.glsl"
  "ffx_search:${FFX_SRC}/passes/ffx_opticalflow_compute_optical_flow_advanced_pass_v5.glsl"
  "ffx_filter:${FFX_SRC}/passes/ffx_opticalflow_filter_optical_flow_pass_v5.glsl"
  "ffx_scale:${FFX_SRC}/passes/ffx_opticalflow_scale_optical_flow_advanced_pass_v5.glsl"
  "ffx_scd_histogram:${FFX_SRC}/passes/ffx_opticalflow_generate_scd_histogram_pass.glsl"
  "ffx_scd_divergence:${FFX_SRC}/passes/ffx_opticalflow_compute_scd_divergence_pass.glsl")
foreach(pass ${FFX_PASSES})
  string(REPLACE ":" ";" fields "${pass}")
  list(GET fields 0 name)
  list(GET fields 1 source)
  set(spv "${SHADER_BIN}/${name}.spv")
  add_custom_command(OUTPUT "${spv}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_BIN}"
    COMMAND "${GLSLANG_EXECUTABLE}" -V --target-env vulkan1.1 -S comp
            -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0
            "-I${FFX_SRC}" -o "${spv}" "${source}"
    DEPENDS "${source}" ${FFX_HEADERS}
    COMMENT "glslang ${name}")
  list(APPEND SHADER_SPV "${spv}")
endforeach()

set(EMBED_CPP "${CMAKE_CURRENT_BINARY_DIR}/openfg_shaders_embedded.cpp")
add_custom_command(OUTPUT "${EMBED_CPP}"
  COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_openfg_shaders.py"
          "${EMBED_CPP}" ${SHADER_SPV}
  DEPENDS ${SHADER_SPV} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_openfg_shaders.py"
  COMMENT "Embedding openfg shaders")

add_library(openfg-config STATIC
  "src/config/config.cpp")
target_include_directories(openfg-config PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(openfg-config PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(vkutil STATIC
  "src/vkutil/vkutil.cpp")
target_include_directories(vkutil PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(openfg STATIC
  "src/openfg/openfg.cpp"
  "src/openfg/openfg_backend.cpp"
  "${EMBED_CPP}")
target_include_directories(openfg PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/openfg"
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(openfg PRIVATE vkutil)
set_target_properties(vkutil openfg PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(fg-lab
  "tools/fg-lab/main.cpp")
target_include_directories(fg-lab PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/openfg"
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(fg-lab PRIVATE openfg vkutil)

add_executable(openfg-cli
  "tools/openfg-cli/main.cpp")
target_link_libraries(openfg-cli PRIVATE openfg-config)

enable_testing()
add_executable(openfg-config-test "tests/config_test.cpp")
target_link_libraries(openfg-config-test PRIVATE openfg-config)
add_test(NAME config COMMAND openfg-config-test)

add_executable(openfg-gpu-probe
  "tools/gpu-probe/main.cpp")
target_include_directories(openfg-gpu-probe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/openfg")
target_link_libraries(openfg-gpu-probe PRIVATE openfg vkutil)

install(TARGETS fg-lab openfg-cli openfg-gpu-probe RUNTIME DESTINATION bin)

add_library(openfg-layer SHARED
  "src/layer/layer.cpp"
  "src/layer/fg.cpp"
  "src/layer/display.cpp")
target_include_directories(openfg-layer PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/openfg")
target_link_libraries(openfg-layer PRIVATE openfg vkutil openfg-config)
set_target_properties(openfg-layer PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "openfg-layer"
  CXX_VISIBILITY_PRESET hidden)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/layer/VkLayer_OPENFG_frame_generation.json.in"
  "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_OPENFG_frame_generation.json"
  @ONLY)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/install")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/layer/VkLayer_OPENFG_frame_generation.install.json.in"
  "${CMAKE_CURRENT_BINARY_DIR}/install/VkLayer_OPENFG_frame_generation.json"
  @ONLY)

install(TARGETS openfg-layer
  LIBRARY DESTINATION lib)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/install/VkLayer_OPENFG_frame_generation.json"
  DESTINATION share/vulkan/implicit_layer.d)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/config/conf.toml"
  DESTINATION share/doc/openfg)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs"
  DESTINATION share/doc/openfg)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
  "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md"
  DESTINATION share/licenses/openfg)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/openfg/shaders/ffx/LICENSE.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/openfg/shaders/ffx/NOTICE.md"
  DESTINATION share/licenses/openfg/ffx)
