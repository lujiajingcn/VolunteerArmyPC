// VolunteerArmyPC —— GDExtension 模块注册
#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "node/hud.h"
#include "node/world_sim.h"

void initialize_volunteer_army_pc_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    GDREGISTER_CLASS(volunteer_army::WorldSim);
    // Hud 也必须注册：它是 Control 子类，构造时要走 Wrapped::_postinitialize()
    // 去 ClassDB 里找自己的类名（用来绑定 _draw 等虚方法）。
    // 漏注册的表现是运行时报 "Cannot get class 'Hud'"，然后整个 HUD 一片空白 ——
    // 编译期完全看不出来，只有跑起来才知道。
    GDREGISTER_CLASS(volunteer_army::Hud);
}

void uninitialize_volunteer_army_pc_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
}

extern "C" {
GDExtensionBool GDE_EXPORT
volunteer_army_pc_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
                               const GDExtensionClassLibraryPtr p_library,
                               GDExtensionInitialization *r_initialization) {
    godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

    init_obj.register_initializer(initialize_volunteer_army_pc_module);
    init_obj.register_terminator(uninitialize_volunteer_army_pc_module);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

    return init_obj.init();
}
}
