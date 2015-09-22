#include <mesh.h>
#include "chunk.h"
#include "ship_space.h"
#include "component/component_system_manager.h"
#include "physics.h"

extern sw_mesh *door_sw;
extern hw_mesh *door_hw;
extern ship_space *ship;

void remove_ents_from_surface(glm::ivec3 b, int face);
glm::mat4 mat_block_face(glm::ivec3 p, int face);

struct entity_type
{
    /* static */
    char const *name;
    char const *mesh;
    int material;
    bool placed_on_surface;
    int height;

    /* loader loop does these */
    sw_mesh *sw;
    hw_mesh *hw;
    btTriangleMesh *phys_mesh;
    btCollisionShape *phys_shape;
};

static entity_type entity_types[] = {
    { "Door", "mesh/single_door_frame.obj", 2, false, 2 },
    { "Frobnicator", "mesh/frobnicator.obj", 3, false, 1 },
    { "Light", "mesh/panel_4x4.obj", 8, true, 1 },
    { "Warning Light", "mesh/warning_light.obj", 8, true, 1 },
    { "Display Panel", "mesh/panel_4x4.obj", 7, true, 1 },
    { "Switch", "mesh/panel_1x1.obj", 9, true, 1 },
    { "Plaidnicator", "mesh/frobnicator.obj", 13, false, 1 },
    { "Pressure Sensor 1", "mesh/panel_1x1.obj", 12, true, 1 },
    { "Pressure Sensor 2", "mesh/panel_1x1.obj", 14, true, 1 },
    { "Sensor Comparator", "mesh/panel_1x1.obj", 13, true, 1 },
};

struct entity
{
    /* TODO: replace this completely, it's silly. */
    c_entity ce;

    entity(glm::ivec3 p, unsigned type, int face) {
        ce = c_entity::spawn();

        auto mat = mat_block_face(p, face);

        auto et = &entity_types[type];

        type_man.assign_entity(ce);
        type_man.type(ce) = type;

        physics_man.assign_entity(ce);
        physics_man.rigid(ce) = nullptr;
        build_static_physics_rb_mat(&mat, et->phys_shape, &physics_man.rigid(ce));
        /* so that we can get back to the entity from a phys raycast */
        physics_man.rigid(ce)->setUserPointer(this);

        surface_man.assign_entity(ce);
        surface_man.block(ce) = p;
        surface_man.face(ce) = face;

        pos_man.assign_entity(ce);
        pos_man.position(ce) = p;
        pos_man.mat(ce) = mat;

        render_man.assign_entity(ce);
        render_man.mesh(ce) = *et->hw;

        if (type == 0) {
            power_man.assign_entity(ce);
            power_man.powered(ce) = false;
            power_man.required_power(ce) = 8;

            switchable_man.assign_entity(ce);
            switchable_man.enabled(ce) = true;

            door_man.assign_entity(ce);
            door_man.mesh(ce) = door_hw;
            door_man.pos(ce) = 1.0f;
        }
        // frobnicator
        else if (type == 1) {
            power_man.assign_entity(ce);
            power_man.powered(ce) = false;
            power_man.required_power(ce) = 12;

            switchable_man.assign_entity(ce);
            switchable_man.enabled(ce) = true;

            gas_man.assign_entity(ce);
            gas_man.flow_rate(ce) = 0.1f;
            gas_man.max_pressure(ce) = 1.0f;
        }
        // light
        else if (type == 2) {
            power_man.assign_entity(ce);
            power_man.powered(ce) = false;
            power_man.required_power(ce) = 6;

            switchable_man.assign_entity(ce);
            switchable_man.enabled(ce) = true;

            light_man.assign_entity(ce);
            light_man.intensity(ce) = 1.f;
            light_man.type(ce) = 1;
        }
        // warning light
        else if (type == 3) {
            power_man.assign_entity(ce);
            power_man.powered(ce) = false;
            power_man.required_power(ce) = 6;

            switchable_man.assign_entity(ce);
            switchable_man.enabled(ce) = false;

            light_man.assign_entity(ce);
            light_man.intensity(ce) = 1.f;
            light_man.type(ce) = 2;
        }
        // display panel
        else if (type == 4) {
            power_man.assign_entity(ce);
            power_man.powered(ce) = false;
            power_man.required_power(ce) = 4;

            light_man.assign_entity(ce);
            light_man.intensity(ce) = 0.15f;

            switchable_man.assign_entity(ce);
            switchable_man.enabled(ce) = true;
        }
        // switch
        else if (type == 5) {
            switch_man.assign_entity(ce);
            switch_man.enabled(ce) = true;
        }
        // plaidnicator
        else if (type == 6) {
            power_provider_man.assign_entity(ce);
            power_provider_man.max_provided(ce) = 12;
            power_provider_man.provided(ce) = 12;
        }
        // pressure sensor 1
        else if (type == 7) {
            pressure_man.assign_entity(ce);
            pressure_man.pressure(ce) = 0.f;
            pressure_man.type(ce) = 1;
        }
        // pressure sensor 2
        else if (type == 8) {
            pressure_man.assign_entity(ce);
            pressure_man.pressure(ce) = 0.f;
            pressure_man.type(ce) = 2;
        }
        // sensor comparator
        else if (type == 9) {
            comparator_man.assign_entity(ce);
            comparator_man.compare_epsilon(ce) = 0.0001f;
        }
    }
};

void destroy_entity(entity *e);
