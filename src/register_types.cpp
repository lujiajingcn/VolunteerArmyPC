// VolunteerArmyPC —— GDExtension 模块注册
#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "node/world_sim.h"

void initialize_volunteer_army_pc_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    GDREGISTER_CLASS(volunteer_army::WorldSim);
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
