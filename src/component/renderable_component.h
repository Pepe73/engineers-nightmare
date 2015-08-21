#pragma once

#include "component_manager.h"

// renderable component
// mesh -- render mesh
// hw_mesh

struct renderable_component_manager : component_manager {
    struct renderable_instance_data {
        c_entity *entity;
        hw_mesh *mesh;
    } instance_pool;

    void create_component_instance_data(unsigned count) override;

    void destroy_instance(instance i) override;

    void entity(const c_entity &e) override;

    hw_mesh & mesh(const c_entity &e) {
        auto inst = lookup(e);

        return instance_pool.mesh[inst.index];
    }
};
