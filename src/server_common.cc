#include "server_common.h"

void
remove_ents_from_surface(glm::ivec3 b, int face)
{
    chunk *ch = ship->get_chunk_containing(b);
    for (auto it = ch->entities.begin(); it != ch->entities.end(); /* */) {
        entity *e = *it;

        /* entities may have been inserted in this chunk which don't have
         * placement on a surface. don't corrupt everything if we hit one.
         */
        if (!surface_man.exists(e->ce)) {
            ++it;
            continue;
        }

        const auto & p = surface_man.block(e->ce);
        const auto & f = surface_man.face(e->ce);

        if (p == b && f == face) {
            destroy_entity(e);
            it = ch->entities.erase(it);
        }
        else {
            ++it;
        }
    }

    block *bl = ship->get_block(b);
    assert(bl);

    bl->surf_space[face] = 0;   /* we've popped *everything* off, it must be empty now */

    if (face == surface_zm) {

        if (bl->type == block_entity)
            bl->type = block_empty;

        ch->render_chunk.valid = false;
    }
}

glm::mat4
mat_block_face(glm::ivec3 p, int face)
{
    static glm::vec3 offsets[] = {
        glm::vec3(1, 0, 0),
        glm::vec3(0, 0, 1),
        glm::vec3(0, 1, 0),
        glm::vec3(0, 0, 1),
        glm::vec3(0, 1, 1),
        glm::vec3(0, 0, 0)
    };

    auto tr = glm::translate(glm::mat4(1), (glm::vec3)p + offsets[face]);

    switch (face) {
    case surface_zp:
        return glm::rotate(tr, (float)M_PI, glm::vec3(1.0f, 0.0f, 0.0f));
    case surface_zm:
        return tr;
    case surface_xp:
        return glm::rotate(tr, -(float)M_PI/2, glm::vec3(0.0f, 1.0f, 0.0f));
    case surface_xm:
        return glm::rotate(tr, (float)M_PI/2, glm::vec3(0.0f, 1.0f, 0.0f));
    case surface_yp:
        return glm::rotate(tr, (float)M_PI/2, glm::vec3(1.0f, 0.0f, 0.0f));
    case surface_ym:
        return glm::rotate(tr, -(float)M_PI/2, glm::vec3(1.0f, 0.0f, 0.0f));

    default:
        return glm::mat4(1);    /* unreachable */
    }
}

void
destroy_entity(entity *e)
{
    /* removing block influence from this ent */
    /* this should really be componentified */
    if (surface_man.exists(e->ce)) {
        auto b = surface_man.block(e->ce);
        auto type = &entity_types[type_man.type(e->ce)];

        for (auto i = 0; i < type->height; i++) {
            auto p = b + glm::ivec3(0, 0, i);
            block *bl = ship->get_block(p);
            assert(bl);
            if (bl->type == block_entity) {
                printf("emptying %d,%d,%d on remove of ent\n", p.x, p.y, p.z);
                bl->type = block_empty;

                for (auto face = 0; face < 6; face++) {
                    /* unreserve all the space */
                    bl->surf_space[face] = 0;
                }
            }
        }
    }

    comparator_man.destroy_entity_instance(e->ce);
    gas_man.destroy_entity_instance(e->ce);
    light_man.destroy_entity_instance(e->ce);
    teardown_static_physics_setup(nullptr, nullptr, &physics_man.rigid(e->ce));
    physics_man.destroy_entity_instance(e->ce);
    pos_man.destroy_entity_instance(e->ce);
    power_man.destroy_entity_instance(e->ce);
    power_provider_man.destroy_entity_instance(e->ce);
    pressure_man.destroy_entity_instance(e->ce);
    render_man.destroy_entity_instance(e->ce);
    surface_man.destroy_entity_instance(e->ce);
    switch_man.destroy_entity_instance(e->ce);
    switchable_man.destroy_entity_instance(e->ce);
    type_man.destroy_entity_instance(e->ce);
    door_man.destroy_entity_instance(e->ce);

    for (auto _type = 0; _type < num_wire_types; _type++) {
        auto type = (wire_type)_type;
        auto & entity_to_attach_lookup = ship->entity_to_attach_lookups[type];
        auto & wire_attachments = ship->wire_attachments[type];

        /* left side is the index of attach on entity that we're removing
        * right side is the index we moved from the end into left side
        * 0, 2 would be read as "attach at index 2 moved to index 0
        * and assumed that what was at index 0 is no longer valid in referencers
        */
        std::unordered_map<unsigned, unsigned> fixup_attaches_removed;
        auto entity_attaches = entity_to_attach_lookup.find(e->ce);
        if (entity_attaches != entity_to_attach_lookup.end()) {
            auto const & set = entity_attaches->second;
            auto attaches = std::vector<unsigned>(set.begin(), set.end());
            std::sort(attaches.begin(), attaches.end());

            auto att_size = attaches.size();
            for (size_t i = 0; i < att_size; ++i) {
                auto attach = attaches[i];

                // fill left side of fixup map
                fixup_attaches_removed[attach] = 0;
            }

            /* Remove relevant attaches from wire_attachments
            * relevant is an attach that isn't occupying a position
            * will get popped off as a result of moving before removing
            */
            unsigned att_index = (unsigned)attaches.size() - 1;
            auto swap_index = wire_attachments.size() - 1;
            for (auto s = wire_attachments.rbegin();
            s != wire_attachments.rend() && att_index != invalid_attach;) {

                auto from_attach = wire_attachments[swap_index];
                auto rem = attaches[att_index];
                if (swap_index > rem) {
                    wire_attachments[rem] = from_attach;
                    wire_attachments.pop_back();
                    fixup_attaches_removed[rem] = (unsigned)swap_index;
                    --swap_index;
                    ++s;
                }
                else if (swap_index == rem) {
                    wire_attachments.pop_back();
                    fixup_attaches_removed.erase(rem);
                    --swap_index;
                    ++s;
                }

                --att_index;
            }

            /* remove all segments that contain an attach on entity */
            for (auto remove_attach : attaches) {
                remove_segments_containing(ship, type, remove_attach);
            }

            /* remove attaches assigned to entity from ship lookup */
            entity_to_attach_lookup.erase(e->ce);

            for (auto lookup : fixup_attaches_removed) {
                /* we moved m to position r */
                auto r = lookup.first;
                auto m = lookup.second;

                relocate_segments_and_entity_attaches(ship, type, r, m);
            }

            attach_topo_rebuild(ship, type);
        }
    }

    delete e;
}
