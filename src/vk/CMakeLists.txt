# Vulkan renderer. Standalone: `cmake -S src/vk -B build-vk && ninja -C build-vk`
# builds vkr_selftest. The game links the vkr library target.
cmake_minimum_required(VERSION 3.20)
if(NOT PROJECT_NAME)
    project(vkr C)
endif()

find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)

set(VKR_GEN "${CMAKE_CURRENT_BINARY_DIR}/vkr_gen")
file(MAKE_DIRECTORY "${VKR_GEN}")

# vkr_shader(<stage> [<base>]): shaders/<base>.<stage> (default base xbox)
# -> vkr_gen/<name>_spv.h with the array <name>_spv, name = xbox_<stage>, or
# <base>_<stage> for another base.
function(vkr_shader stage)
    set(base "xbox")
    set(name "xbox_${stage}")
    if(ARGC GREATER 1)
        set(base "${ARGV1}")
        set(name "${ARGV1}_${stage}")
    endif()
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${base}.${stage}")
    set(spv "${VKR_GEN}/${name}.spv")
    set(hdr "${VKR_GEN}/${name}_spv.h")
    add_custom_command(
        OUTPUT "${hdr}"
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -o "${spv}" "${src}"
        COMMAND ${CMAKE_COMMAND} -DIN=${spv} -DOUT=${hdr} -DNAME=${name}_spv
                -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake"
        DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake"
        VERBATIM)
    set(VKR_HEADERS ${VKR_HEADERS} "${hdr}" PARENT_SCOPE)
endfunction()
# vkr_shader_variant(<stage> <base> <name> <define>): the same source built
# with -D<define> into <name>_spv.
function(vkr_shader_variant stage base name def)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${base}.${stage}")
    set(spv "${VKR_GEN}/${name}.spv")
    set(hdr "${VKR_GEN}/${name}_spv.h")
    add_custom_command(
        OUTPUT "${hdr}"
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -D${def} -o "${spv}" "${src}"
        COMMAND ${CMAKE_COMMAND} -DIN=${spv} -DOUT=${hdr} -DNAME=${name}_spv
                -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake"
        DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake"
        VERBATIM)
    set(VKR_HEADERS ${VKR_HEADERS} "${hdr}" PARENT_SCOPE)
endfunction()
vkr_shader(vert)
vkr_shader(frag)
vkr_shader_variant(frag xbox xbox_frag_wdepth W_DEPTH)
vkr_shader(vert xbox_vsh)
vkr_shader(vert crt)
vkr_shader(frag crt)

add_library(vkr STATIC vkr.c ${VKR_HEADERS})
# vkr_backend.c is added by the game build (needs the toolkit headers).
target_include_directories(vkr PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${VKR_GEN}")
target_link_libraries(vkr PUBLIC Vulkan::Vulkan)

add_executable(vkr_selftest vkr_selftest.c)
target_link_libraries(vkr_selftest PRIVATE vkr)
