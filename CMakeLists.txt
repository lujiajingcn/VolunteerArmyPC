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
    src/sim/va_campaign.cpp
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
    src/node/hud.cpp
    src/node/audio.cpp
    src/node/voice.cpp
    src/node/unit_anim.cpp
    src/node/unit_leg.cpp
    src/node/fx_layer.cpp
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

# ---------------------------------------------------------------------------
# 让 VS 的 F5「运行工程」真的能跑起来
#
#   volunteer_army_pc 是 SHARED 库（GDExtension），它**不是**可执行程序。
#   VS 找不到调试器命令时，会退而拿"目标的输出文件"当要启动的程序 ——
#   于是 F5 去执行 bin/volunteer_army_pc.dll，弹出
#       「无法启动程序 … 不是有效的 Win32 应用程序」
#   这句话的字面意思就是 CreateProcess 只能启动 EXE，却被喂了个 DLL；
#   跟文件损坏、架构不对都没关系（这个 dll 是合法的 x64 PE）。
#
#   正解不是换启动项目，而是告诉 VS"这个目标该怎么被调试"：
#   用引擎可执行程序带着本工程跑（--path <工程根>）。引擎会加载
#   bin/volunteer_army_pc.dll，断点就落在我们自己的代码里 ——
#   GDExtension 没有独立的宿主 EXE，这也是它唯一的本地调试方式。
#
#   两个必须注意的点：
#   1) 只能指向**主 exe**，不能指向 *_console.exe。后者是 197 KB 的外壳
#      （subsys=CONSOLE，导入 CreateProcessW），只负责 fork 出主 exe 并挂
#      一个控制台；VS 会附到外壳上，而外壳里根本没加载我们的 dll，
#      断点永远绑不上。下面自动挑选时已把 *console.exe 排除。
#   2) 必须带 --log-file。主 exe 是 GUI 子系统，stdout 无人接收 ——
#      不加这个参数，print 出来的东西（含 [capture-ev]/[hud-ev] 与
#      Godot 自身的报错）会全部丢掉，而本项目「回归判据 = 日志里有没有
#      ERROR」全靠它。日志写到工程根的 debug_run.log（已被 *.log 忽略）。
# ---------------------------------------------------------------------------
set(VA_GODOT_EXECUTABLE "" CACHE FILEPATH
    "VS 调试(F5)时用来加载本 GDExtension 的引擎可执行文件；留空则自动在 sdk/godot/ 下寻找")

if(NOT VA_GODOT_EXECUTABLE)
    file(GLOB _va_godot_exes "${CMAKE_CURRENT_SOURCE_DIR}/sdk/godot/*.exe")
    foreach(_e ${_va_godot_exes})
        if(_e MATCHES "[_.]console\\.exe$")
            # 控制台外壳：能看输出，但它不是加载 dll 的那个进程，调试用不了
        else()
            list(APPEND _va_godot_engines "${_e}")
        endif()
    endforeach()
    if(_va_godot_engines)
        list(SORT _va_godot_engines)
        list(GET _va_godot_engines 0 VA_GODOT_EXECUTABLE)
    endif()
endif()

# 可选：F5 时注入环境变量（分号分隔，如 VA_SCRIPT_A=1;VA_FF=6）。
# 取证跑图那套旋钮见 README，想 F5 直接进交火就靠它。
set(VA_DEBUG_ENV "" CACHE STRING
    "F5 时注入的环境变量，分号分隔，如 VA_SCRIPT_A=1;VA_FF=6")

if(MSVC)
    target_compile_options(volunteer_army_pc PRIVATE /utf-8 /MP /bigobj)
    target_compile_definitions(volunteer_army_pc PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    if(VA_GODOT_EXECUTABLE)
        set_target_properties(volunteer_army_pc PROPERTIES
            VS_DEBUGGER_COMMAND "${VA_GODOT_EXECUTABLE}"
            VS_DEBUGGER_COMMAND_ARGUMENTS
                "--path \"${CMAKE_CURRENT_SOURCE_DIR}\" --log-file \"${CMAKE_CURRENT_SOURCE_DIR}/debug_run.log\""
            VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        if(VA_DEBUG_ENV)
            string(REPLACE ";" "\n" _va_env_lines "${VA_DEBUG_ENV}")
            set_target_properties(volunteer_army_pc PROPERTIES
                VS_DEBUGGER_ENVIRONMENT "${_va_env_lines}")
        endif()
    else()
        message(WARNING
            "sdk/godot/ 下没找到引擎可执行文件 —— VS 里 F5 仍会去启动那个 dll。\n"
            "  请把 Godot 引擎（主 exe，不是 *_console.exe）放进 sdk/godot/，"
            "或设 -DVA_GODOT_EXECUTABLE=<绝对路径>。")
    endif()
endif()

# ---------------------------------------------------------------------------
# 离线平衡扫描工具（可选）
#   逻辑层零引擎依赖（src/sim/ 不 include 任何 godot 头），所以它能不链接
#   godot-cpp 单独编成控制台程序 —— 这既是 README 里「逻辑层可脱离引擎单独跑」
#   那句话的**可执行证据**，也是平衡调整的能量化前提：
#   跑一局要 620 秒模拟时间，10 个种子 × 6 套对照只能靠它。
#   从 VA_SOURCES 里按前缀挑出逻辑层，避免维护第二份文件清单（会漂）。
# ---------------------------------------------------------------------------
set(VA_SIM_SOURCES "")
foreach(f ${VA_SOURCES})
    if(f MATCHES "^src/sim/")
        list(APPEND VA_SIM_SOURCES ${f})
    endif()
endforeach()

option(VA_BUILD_SIM_SWEEP "构建离线平衡扫描工具 va_sweep（纯逻辑层，不链接引擎）" ON)
if(VA_BUILD_SIM_SWEEP AND VA_SIM_SOURCES)
    add_executable(va_sweep tools/va_sweep.cpp ${VA_SIM_SOURCES})
    target_include_directories(va_sweep PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # 产物落在 sweep/（.gitignore 已忽略）—— 可再生成的工具不入库
    foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
        set_target_properties(va_sweep PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY${cfg} "${CMAKE_CURRENT_SOURCE_DIR}/sweep/sim")
    endforeach()
    if(MSVC)
        target_compile_options(va_sweep PRIVATE /utf-8 /MP)
        target_compile_definitions(va_sweep PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endif()

message(STATUS "──────────────────────────────────────────────")
message(STATUS " VolunteerArmyPC  ·  Godot 4.5 + C++ GDExtension")
message(STATUS "   godot-cpp target : ${GODOTCPP_TARGET}")
message(STATUS "   源文件           : ${VA_EXISTING}")
message(STATUS "   输出目录         : ${CMAKE_CURRENT_SOURCE_DIR}/bin")
message(STATUS "   离线扫描 va_sweep: ${VA_BUILD_SIM_SWEEP}（产物 -> sweep/sim/）")
if(VA_GODOT_EXECUTABLE)
    message(STATUS "   VS 的 F5         : ${VA_GODOT_EXECUTABLE}")
    message(STATUS "                      --path <工程根> --log-file debug_run.log")
else()
    message(STATUS "   VS 的 F5         : 未配置（sdk/godot/ 下没有引擎 exe）")
endif()
message(STATUS "──────────────────────────────────────────────")
