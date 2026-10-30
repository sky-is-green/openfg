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

set(SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/openfg/shaders")
set(SHADER_BIN "${CMAKE_CURRENT_BINARY_DIR}/openfg-shaders")
file(GLOB SHADER_FILES CONFIGURE_DEPENDS "${SHADER_SRC}/*.comp")

set(SHADER_SPV)
foreach(shader ${SHADER_FILES})
  get_filename_component(name "${shader}" NAME_WE)
  set(spv "${SHADER_BIN}/${name}.spv")
  add_custom_command(OUTPUT "${spv}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_BIN}"
    COMMAND "${GLSLC_EXECUTABLE}" -O -I"${SHADER_SRC}" -o "${spv}" "${shader}"
    DEPENDS "${shader}" "${SHADER_SRC}/common.glsl"
    COMMENT "glslc ${name}.comp")
  list(APPEND SHADER_SPV "${spv}")
endforeach()

set(EMBED_CPP "${CMAKE_CURRENT_BINARY_DIR}/openfg_shaders_embedded.cpp")
add_custom_command(OUTPUT "${EMBED_CPP}"
  COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_openfg_shaders.py"
          "${EMBED_CPP}" ${SHADER_SPV}
  DEPENDS ${SHADER_SPV} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_openfg_shaders.py"
  COMMENT "Embedding openfg shaders")

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
target_compile_definitions(openfg PRIVATE
  OPENFG_SHADER_DIR="${SHADER_BIN}")

add_executable(fg-lab
  "tools/fg-lab/main.cpp")
target_include_directories(fg-lab PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/openfg"
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(fg-lab PRIVATE openfg vkutil)
target_compile_definitions(fg-lab PRIVATE
  OPENFG_SHADER_DIR="${SHADER_BIN}")

install(TARGETS fg-lab RUNTIME DESTINATION bin)

add_library(openfg-layer SHARED
  "src/layer/layer.cpp"
  "src/layer/fg.cpp")
target_include_directories(openfg-layer PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/openfg")
target_link_libraries(openfg-layer PRIVATE openfg vkutil)
set_target_properties(openfg-layer PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "openfg-layer"
  CXX_VISIBILITY_PRESET hidden)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/layer/VkLayer_OPENFG_frame_generation.json.in"
  "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_OPENFG_frame_generation.json"
  @ONLY)

install(TARGETS openfg-layer
  LIBRARY DESTINATION lib)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_OPENFG_frame_generation.json"
  DESTINATION share/vulkan/implicit_layer.d)
