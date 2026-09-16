cmake_minimum_required(VERSION 3.17)
project(VolunteerArmyPC LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局编译设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# godot-cpp：GDExtension 的 C++ 绑定
#   GODOTCPP_TARGET 决定绑定面向哪个目标变体编译（editor / template_debug /
#   template_release）。默认 template_debug，便于在编辑器里带调试符号迭代。
# ---------------------------------------------------------------------------
if(NOT DEFINED GODOTCPP_TARGET)
    set(GODOTCPP_TARGET "template_debug" CACHE STRING "godot-cpp build target" FORCE)
endif()

add_subdirectory(ext/godot-cpp)

# ---------------------------------------------------------------------------
# 本工程源码
# ---------------------------------------------------------------------------
set(VA_SOURCES
    src/register_types.cpp
    src/sim/va_config.cpp
    src/sim/va_grammar.cpp
    src/sim/va_parser.cpp
    src/sim/va_world.cpp
    src/sim/va_level.cpp
    src/sim/va_units.cpp
    src/sim/va_combat.cpp
    src/sim/va_commands.cpp
    src/sim/va_ai_ally.cpp
    src/sim/va_ai_enemy.cpp
    src/sim/va_flow.cpp
    src/node/viewmodel.cpp
    src/node/world_sim.cpp
    src/node/scene_builder.cpp
)

# 尚未写到的文件先跳过（骨架期允许文件不存在）
set(VA_EXISTING "")
foreach(f ${VA_SOURCES})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${f}")
        list(APPEND VA_EXISTING ${f})
    else()
        message(STATUS "  [skip] 尚未创建: ${f}")
    endif()
endforeach()

add_library(volunteer_army_pc SHARED ${VA_EXISTING})

target_include_directories(volunteer_army_pc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(volunteer_army_pc PRIVATE godot::cpp)

# 输出到 bin/，文件名固定，由 .gdextension 引用
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set_target_properties(volunteer_army_pc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY${cfg} "${CMAKE_CURRENT_SOURCE_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY${cfg} "${CMAKE_CURRENT_SOURCE_DIR}/bin"
        PREFIX ""
        OUTPUT_NAME "volunteer_army_pc"
    )
endforeach()

if(MSVC)
    target_compile_options(volunteer_army_pc PRIVATE /utf-8 /MP /bigobj)
    target_compile_definitions(volunteer_army_pc PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(volunteer_army_pc PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

message(STATUS "──────────────────────────────────────────────")
message(STATUS " VolunteerArmyPC  ·  Godot 4.5 + C++ GDExtension")
message(STATUS "   godot-cpp target : ${GODOTCPP_TARGET}")
message(STATUS "   源文件           : ${VA_EXISTING}")
message(STATUS "   输出目录         : ${CMAKE_CURRENT_SOURCE_DIR}/bin")
message(STATUS "──────────────────────────────────────────────")
