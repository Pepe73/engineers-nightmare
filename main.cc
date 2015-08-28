#include <stdio.h>
#include <SDL.h>

#ifndef _WIN32
#include <err.h> /* errx */
#else
#include "src/winerr.h"
#endif

#include <epoxy/gl.h>
#include <algorithm>
#include <functional>

#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "src/common.h"
#include "src/config.h"
#include "src/input.h"
#include "src/mesh.h"
#include "src/physics.h"
#include "src/player.h"
#include "src/shader.h"
#include "src/ship_space.h"
#include "src/text.h"
#include "src/textureset.h"
#include "src/tools.h"
#include "src/shader_params.h"
#include "src/light_field.h"

#include "src/component/component_manager.h"
#include "src/component/gas_production_component.h"
#include "src/component/light_component.h"
#include "src/component/power_component.h"
#include "src/component/relative_position.h"
#include "src/component/renderable_component.h"
#include "src/component/switch_component.h"
#include "src/component/switchable_component.h"

#include "src/projectile/projectile.h"

#include "src/scopetimer.h"


#define APP_NAME    "Engineer's Nightmare"
#define DEFAULT_WIDTH   1024
#define DEFAULT_HEIGHT  768


#define WORLD_TEXTURE_DIMENSION     32
#define MAX_WORLD_TEXTURES          64

#define MOUSE_Y_LIMIT   1.54f
#define MAX_AXIS_PER_EVENT 128

bool exit_requested = false;

bool draw_hud = true;

auto hfov = DEG2RAD(90.f);

en_settings game_settings;

struct wire_attachment {
    glm::mat4 transform;
};

struct wire_segment {
    unsigned first = -1;
    unsigned second = -1;
};

struct wire {
    std::vector<wire_segment> segments;
};

struct {
    SDL_Window *ptr;
    SDL_GLContext gl_ctx;
    int width;
    int height;
    bool has_focus;
} wnd;

struct {
    Timer timer;

    const float fps_duration = 0.25f;

    unsigned int frame = 0;

    unsigned int fps_frame = 0;
    float fps_time = 0.f;

    float dt = 0.f;
    float fps = 0.f;

    void tick() {
        auto t = timer.touch();

        dt = (float) t.delta;   /* narrowing */
        frame++;

        fps_frame++;
        fps_time += dt;

        if (fps_time >= fps_duration) {
            fps = 1 / (fps_time / fps_frame);
            fps_time = 0.f;
            fps_frame = 0;
        }
    }
} frame_info;

void GLAPIENTRY
gl_debug_callback(GLenum source __unused,
                  GLenum type __unused,
                  GLenum id __unused,
                  GLenum severity __unused,
                  GLsizei length __unused,
                  GLchar const *message,
                  void const *userParam __unused)
{
    printf("GL: %s\n", message);
}

bool segment_finished(wire_segment *segment) {
    if (!segment) {
        return true;
    }

    return segment->first != ~0u && segment->second != ~0u;
}


// 16M per frame
#define FRAME_DATA_SIZE     (16u * 1024 * 1024)
#define NUM_INFLIGHT_FRAMES 3

struct frame_data {
    GLuint bo;
    void *base_ptr;
    size_t offset;
    GLsync fence;
    GLint hw_align;

    frame_data() : bo(0), base_ptr(0), offset(0), fence(0), hw_align(1) {
        glGenBuffers(1, &bo);
        glBindBuffer(GL_UNIFORM_BUFFER, bo);
        glBufferStorage(GL_UNIFORM_BUFFER, FRAME_DATA_SIZE, nullptr,
            GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        base_ptr = glMapBufferRange(GL_UNIFORM_BUFFER, 0, FRAME_DATA_SIZE,
            GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &hw_align);

        printf("frame_data base=%p hw_align=%d\n", base_ptr, hw_align);
    }

    /* Prepare for filling this frame_data. If the backing BO is still in flight,
     * this may stall waiting for the frame to retire.
     */
    void begin() {
        if (fence) {
            /* Wait on the fence if this frame_data might be in flight. */

            /* TODO: detect case where we are getting throttled by this wait -- it suggests
             * that either we have insufficient frame_data buffers, or that the driver is
             * trying to run the GPU an unacceptable number of frames behind.
             */
            glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(fence);
            fence = 0;
        }

        offset = 0;
    }

    /* Signal that all uses of this frame_data have been submitted to the hardware. */
    void end() {
        /* All gpu commands using this frame_data have now been issued. Drop a fence
         * into the pipeline so we know when the buffer can be reused.
         */
        fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }

    /* A transient GPU memory allocation. Usable until end() is called.
     */
    template<typename T>
    struct alloc
    {
        T* ptr;
        size_t off;
        size_t size;

        void bind(GLuint index, frame_data *f) {
            glBindBufferRange(GL_UNIFORM_BUFFER, index, f->bo, off, size);
        }
    };

    /* Allocate a chunk of frame_data for an array of T*, aligned appropriately for
     * use as both a constant buffer, and for CPU access.
     */
    template<typename T>
    alloc<T> alloc_aligned(size_t count, size_t align = alignof(T)) {
        align = std::max(align, (size_t)hw_align);
        offset = (offset + align - 1) & ~(align - 1);
        alloc<T> a{ (T*)(offset + (size_t)base_ptr), offset, count * sizeof(T) };
        offset += a.size;
        return a;
    }
};

frame_data *frames, *frame;
unsigned frame_index;

sw_mesh *scaffold_sw;
sw_mesh *surfs_sw[6];
sw_mesh *projectile_sw;
sw_mesh *attachment_sw;
sw_mesh *wire_sw;
GLuint simple_shader, unlit_shader, add_overlay_shader, remove_overlay_shader, ui_shader, ui_sprites_shader;
GLuint sky_shader, unlit_instanced_shader, lit_instanced_shader;
shader_params<per_object_params> *per_object;
texture_set *world_textures;
texture_set *skybox;
ship_space *ship;
player pl;
physics *phy;
unsigned char const *keys;
unsigned int mouse_buttons[input_mouse_buttons_count];
int mouse_axes[input_mouse_axes_count];
hw_mesh *scaffold_hw;
hw_mesh *surfs_hw[6];
hw_mesh *projectile_hw;
hw_mesh *attachment_hw;
hw_mesh *wire_hw;
text_renderer *text;
sprite_renderer *ui_sprites;
light_field *light;
entity *use_entity = nullptr;

sprite_metrics unlit_ui_slot_sprite, lit_ui_slot_sprite;

std::vector<wire_attachment> wire_attachments;
std::vector<wire> wires;

template<typename T>
T
clamp(T t, T lower, T upper) {
    if (t < lower) return lower;
    if (t > upper) return upper;
    return t;
}

gas_production_component_manager gas_man;
light_component_manager light_man;
power_component_manager power_man;
relative_position_component_manager pos_man;
renderable_component_manager render_man;
switch_component_manager switch_man;
switchable_component_manager switchable_man;

projectile_linear_manager proj_man;

glm::ivec3
get_block_containing(glm::vec3 v) {
    /* truncating via int cast */
    int x = (int) v.x; if (v.x < 0) x--;
    int y = (int) v.y; if (v.y < 0) y--;
    int z = (int) v.z; if (v.z < 0) z--;

    return glm::ivec3(x, y, z);
}

glm::mat4
mat_rotate_mesh(glm::vec3 pt, glm::vec3 dir) {
    auto abs_normal = glm::abs(dir);
    glm::vec3 temp_up(1, 0, 0);
    if (abs_normal.x > abs_normal.y && abs_normal.x > abs_normal.z) {
        /* avoid degeneracy at the `poles` */
        temp_up = glm::vec3(0, 1, 0);
    }
    glm::mat4 m = glm::transpose(glm::lookAt(dir, glm::vec3(0, 0, 0), temp_up));
    m[3][0] = pt.x;
    m[3][1] = pt.y;
    m[3][2] = pt.z;
    m[0][3] = 0;
    m[1][3] = 0;
    m[2][3] = 0;
    return m;
}

glm::mat4
mat_position(glm::vec3 pos)
{
    return glm::translate(glm::mat4(1), pos);
}

glm::mat4
mat_position(float x, float y, float z)
{
    return mat_position(glm::vec3(x, y, z));
}

glm::mat4
mat_scale(glm::vec3 scale) {
    return glm::scale(glm::mat4(1), scale);
}

glm::mat4
mat_scale(float x, float y, float z) {
    return mat_scale(glm::vec3(x, y, z));
}

glm::mat4
mat_block_face(int x, int y, int z, int face)
{
    switch (face) {
    case surface_zp:
        return glm::rotate(glm::translate(glm::mat4(1), glm::vec3(x, y + 1, z + 1)), (float)M_PI, glm::vec3(1.0f, 0.0f, 0.0f));
    case surface_zm:
        return glm::translate(glm::mat4(1), glm::vec3(x, y, z));
    case surface_xp:
        return glm::rotate(glm::translate(glm::mat4(1), glm::vec3(x + 1, y, z)), -(float)M_PI/2, glm::vec3(0.0f, 1.0f, 0.0f));
    case surface_xm:
        return glm::rotate(glm::translate(glm::mat4(1), glm::vec3(x, y, z + 1)), (float)M_PI/2, glm::vec3(0.0f, 1.0f, 0.0f));
    case surface_yp:
        return glm::rotate(glm::translate(glm::mat4(1), glm::vec3(x, y + 1, z)), (float)M_PI/2, glm::vec3(1.0f, 0.0f, 0.0f));
    case surface_ym:
        return glm::rotate(glm::translate(glm::mat4(1), glm::vec3(x, y, z + 1)), -(float)M_PI/2, glm::vec3(1.0f, 0.0f, 0.0f));

    default:
        return glm::mat4(1);    /* unreachable */
    }
}


struct entity_type
{
    sw_mesh *sw;
    hw_mesh *hw;
    char const *name;
    btTriangleMesh *phys_mesh;
    btCollisionShape *phys_shape;
};


entity_type entity_types[4];

/* fwd for temp spawn logic just below */
void
mark_lightfield_update(int x, int y, int z);


struct entity
{
    /* TODO: replace this completely, it's silly. */
    int x, y, z;
    entity_type *type;
    btRigidBody *phys_body;
    int face;
    c_entity ce;

    entity(int x, int y, int z, entity_type *type, int face)
        : x(x), y(y), z(z), type(type), phys_body(nullptr), face(face) {
        auto mat = mat_block_face(x, y, z, face);

        build_static_physics_rb_mat(&mat, type->phys_shape, &phys_body);

        /* so that we can get back to the entity from a phys raycast */
        phys_body->setUserPointer(this);

        pos_man.assign_entity(ce);
        pos_man.position(ce) = glm::vec3(x, y, z);
        pos_man.mat(ce) = mat;

        render_man.assign_entity(ce);
        render_man.mesh(ce) = *type->hw;

        // frobnicator
        if (type == &entity_types[0]) {
            power_man.assign_entity(ce);
            //default to powered state for now
            power_man.powered(ce) = true;

            switchable_man.assign_entity(ce);
            switchable_man.enabled(ce) = false;

            gas_man.assign_entity(ce);
            gas_man.flow_rate(ce) = 0.1f;
            gas_man.max_pressure(ce) = 1.0f;
        }
        // display panel
        else if (type == &entity_types[1]) {
            power_man.assign_entity(ce);
            //default to powered state for now
            power_man.powered(ce) = true;

            light_man.assign_entity(ce);
            light_man.intensity(ce) = 0.15f;
        }
        // light
        else if (type == &entity_types[2]) {
            power_man.assign_entity(ce);
            //default to powered state for now
            power_man.powered(ce) = true;

            switchable_man.assign_entity(ce);
            switchable_man.enabled(ce) = true;

            light_man.assign_entity(ce);
            light_man.intensity(ce) = 1.f;
        }
        // switch
        else if (type == &entity_types[3]) {
            switch_man.assign_entity(ce);
        }
    }

    ~entity() {
        teardown_static_physics_setup(nullptr, nullptr, &phys_body);
    }

    void use() {
        /* used by the player */
        printf("player using the %s at %d %d %d\n",
               type->name, x, y, z);

        assert(pos_man.exists(ce) || !"All [usable] entities probably need position");

        // hacks abound until we get wiring in
        if (switchable_man.exists(ce)) {
            // gas producer toggles directly
            if (gas_man.exists(ce)) {
                switchable_man.enabled(ce) ^= true;
            }
        }

        if (switch_man.exists(ce)) {
            // switch toggles directly
            // finds any switchable in the 6 adjacent blocks and toggles
            auto pos = pos_man.position(ce);

            for (auto i = 0u; i < pos_man.buffer.num; ++i) {
                auto pos2 = pos_man.instance_pool.position[i];
                auto entity = pos_man.instance_pool.entity[i];
                if (pos == glm::vec3(pos2.x + 1, pos2.y, pos2.z) ||
                    pos == glm::vec3(pos2.x - 1, pos2.y, pos2.z) ||
                    pos == glm::vec3(pos2.x, pos2.y + 1, pos2.z) ||
                    pos == glm::vec3(pos2.x, pos2.y - 1, pos2.z) ||
                    pos == glm::vec3(pos2.x, pos2.y, pos2.z + 1) ||
                    pos == glm::vec3(pos2.x, pos2.y, pos2.z - 1)) {
                    if (light_man.exists(entity) && switchable_man.exists(entity)) {
                        switchable_man.enabled(entity) ^= true;
                        mark_lightfield_update(x, y, z);
                    }
                }
            }
        }
    }
};


void
tick_gas_producers()
{
    for (auto i = 0u; i < gas_man.buffer.num; i++) {
        auto ce = gas_man.instance_pool.entity[i];

        /* gas producers require: power, position */
        assert(switchable_man.exists(ce) || !"gas producer must be switchable");

        auto should_produce = switchable_man.enabled(ce) && power_man.powered(ce);
        if (!should_produce) {
            return;
        }

        auto pos = get_block_containing(pos_man.position(ce));

        /* topo node containing the entity */
        topo_info *t = topo_find(ship->get_topo_info(pos.x, pos.y, pos.z));
        zone_info *z = ship->get_zone_info(t);
        if (!z) {
            /* if there wasn't a zone, make one */
            z = ship->zones[t] = new zone_info(0);
        }

        /* add some gas if we can, up to our pressure limit */
        float max_gas = gas_man.max_pressure(ce) * t->size;
        if (z->air_amount < max_gas)
            z->air_amount = std::min(max_gas, z->air_amount + gas_man.flow_rate(ce));
    }
}

void
draw_renderables(frame_data *frame)
{
    for (auto i = 0u; i < render_man.buffer.num; i++) {
        auto ce = render_man.instance_pool.entity[i];

        auto mat = pos_man.mat(ce);
        auto mesh = render_man.mesh(ce);

        auto entity_matrix = frame->alloc_aligned<glm::mat4>(1);
        *entity_matrix.ptr = mat;
        entity_matrix.bind(1, frame);

        draw_mesh(&mesh);
    }
}


#define INSTANCE_BATCH_SIZE 256u        /* needs to be <= the value in the shader */

void
draw_projectiles(frame_data *frame)
{
    for (auto i = 0u; i < proj_man.buffer.num; i += INSTANCE_BATCH_SIZE) {
        auto batch_size = std::min(INSTANCE_BATCH_SIZE, proj_man.buffer.num - i);
        auto projectile_matrices = frame->alloc_aligned<glm::mat4>(batch_size);

        for (auto j = 0u; j < batch_size; j++) {
            projectile_matrices.ptr[j] = mat_position(proj_man.position(i + j));
        }

        projectile_matrices.bind(1, frame);
        draw_mesh_instanced(projectile_hw, batch_size);
    }
}

void
draw_attachments(frame_data *frame)
{
    auto count = wire_attachments.size();
    for (auto i = 0u; i < count; i += INSTANCE_BATCH_SIZE) {
        auto batch_size = std::min(INSTANCE_BATCH_SIZE, (unsigned)(count - i));
        auto attachment_matrices = frame->alloc_aligned<glm::mat4>(batch_size);

        for (auto j = 0u; j < batch_size; j++) {
            attachment_matrices.ptr[j] = wire_attachments[i + j].transform;
        }

        attachment_matrices.bind(1, frame);
        draw_mesh_instanced(attachment_hw, batch_size);
    }
}

glm::mat4
calc_segment_matrices(const wire_attachment &start, const wire_attachment &end) {
    auto a1 = start.transform;
    auto a2 = end.transform;

    auto p1_4 = a1[3];
    auto p2_4 = a2[3];

    auto p1 = glm::vec3(p1_4.x, p1_4.y, p1_4.z);
    auto p2 = glm::vec3(p2_4.x, p2_4.y, p2_4.z);

    auto seg = p1 - p2;
    auto len = glm::length(seg);

    auto scale = mat_scale(1, 1, len);

    auto rot = mat_rotate_mesh(p2, glm::normalize(p2 - p1));

    return rot * scale;
}

void
draw_segments(frame_data *frame)
{
    if (wires.size() <= 0) {
        return;
    }

    for (auto h = 0u; h < wires.size(); ++h) {
        auto count = wires[h].segments.size();
        for (auto i = 0u; i < count; i += INSTANCE_BATCH_SIZE) {
            auto batch_size = std::min(INSTANCE_BATCH_SIZE, (unsigned)(count - i));
            auto segment_matrices = frame->alloc_aligned<glm::mat4>(batch_size);

            auto added = 0u;
            for (auto j = 0u; j < batch_size; j++) {
                auto segment = wires[h].segments[i + j];
                if (!segment_finished(&segment))
                    continue;

                auto a1 = wire_attachments[segment.first];
                auto a2 = wire_attachments[segment.second];

                auto mat = calc_segment_matrices(a1, a2);

                segment_matrices.ptr[added] = mat;
                ++added;
            }

            if (added) {
                segment_matrices.bind(1, frame);
                draw_mesh_instanced(wire_hw, added);
            }
        }
    }
}


void
set_light_level(int x, int y, int z, int level)
{
    if (x < 0 || x >= 128) return;
    if (y < 0 || y >= 128) return;
    if (z < 0 || z >= 128) return;

    int p = x + y * 128 + z * 128 * 128;
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    light->data[p] = level;
}


unsigned char
get_light_level(int x, int y, int z)
{
    if (x < 0 || x >= 128) return 0;
    if (y < 0 || y >= 128) return 0;
    if (z < 0 || z >= 128) return 0;

    return light->data[x + y*128 + z*128*128];
}


const int light_atten = 50;
/* as far as we can ever light from a light source */
const int max_light_prop = (255 + light_atten - 1) / light_atten;

bool need_lightfield_update = false;
glm::ivec3 lightfield_update_mins;
glm::ivec3 lightfield_update_maxs;


void
mark_lightfield_update(int x, int y, int z)
{
    glm::ivec3 center = glm::ivec3(x, y, z);
    glm::ivec3 half_extent = glm::ivec3(max_light_prop, max_light_prop, max_light_prop);
    if (need_lightfield_update) {
        lightfield_update_mins = center - half_extent;
        lightfield_update_maxs = center + half_extent;
    }
    else {
        lightfield_update_mins = glm::min(lightfield_update_mins,
                center - half_extent);
        lightfield_update_maxs = glm::max(lightfield_update_maxs,
                center + half_extent);
        need_lightfield_update = true;
    }
}


void
update_lightfield()
{
    if (!need_lightfield_update) {
        /* nothing to do here */
        return;
    }

    /* TODO: opt for case where we're JUST adding light -- no need to clear & rebuild */
    /* This is general enough to cope with occluders & lights being added and removed. */

    /* 1. remove all existing light in the box */
    for (int k = lightfield_update_mins.z; k <= lightfield_update_maxs.z; k++)
        for (int j = lightfield_update_mins.y; j <= lightfield_update_maxs.y; j++)
            for (int i = lightfield_update_mins.x; i <= lightfield_update_maxs.x; i++)
                set_light_level(i, j, k, 0);

    /* 2. inject sources. the box is guaranteed to be big enough for max propagation
     * for all sources we'll add here. */
    for (auto i = 0u; i < light_man.buffer.num; i++) {
        auto ce = light_man.instance_pool.entity[i];
        auto pos = get_block_containing(pos_man.position(ce));
        auto exists = switchable_man.exists(ce);
        auto should_emit = exists ? switchable_man.enabled(ce) && power_man.powered(ce) : power_man.powered(ce);
        if (should_emit) {
            set_light_level(pos.x, pos.y, pos.z, (int)(255 * light_man.intensity(ce)));
        }
    }

    /* 3. propagate max_light_prop times. this is guaranteed to be enough to cover
     * the sources' area of influence. */
    for (int pass = 0; pass < max_light_prop; pass++) {
        for (int k = lightfield_update_mins.z; k <= lightfield_update_maxs.z; k++) {
            for (int j = lightfield_update_mins.y; j <= lightfield_update_maxs.y; j++) {
                for (int i = lightfield_update_mins.x; i <= lightfield_update_maxs.x; i++) {
                    int level = get_light_level(i, j, k);

                    block *b = ship->get_block(i, j, k);
                    if (!b)
                        continue;

                    if (light_permeable(b->surfs[surface_xm]))
                        level = std::max(level, get_light_level(i - 1, j, k) - light_atten);
                    if (light_permeable(b->surfs[surface_xp]))
                        level = std::max(level, get_light_level(i + 1, j, k) - light_atten);

                    if (light_permeable(b->surfs[surface_ym]))
                        level = std::max(level, get_light_level(i, j - 1, k) - light_atten);
                    if (light_permeable(b->surfs[surface_yp]))
                        level = std::max(level, get_light_level(i, j + 1, k) - light_atten);

                    if (light_permeable(b->surfs[surface_zm]))
                        level = std::max(level, get_light_level(i, j, k - 1) - light_atten);
                    if (light_permeable(b->surfs[surface_zp]))
                        level = std::max(level, get_light_level(i, j, k + 1) - light_atten);

                    set_light_level(i, j, k, level);
                }
            }
        }
    }

    /* All done. */
    light->upload();
    need_lightfield_update = false;
}


struct game_state {
    virtual ~game_state() {}

    virtual void handle_input() = 0;
    virtual void update(float dt) = 0;
    virtual void rebuild_ui() = 0;

    static game_state *create_play_state();
    static game_state *create_menu_state();
    static game_state *create_menu_settings_state();
};


game_state *state = game_state::create_play_state();

void
set_game_state(game_state *s)
{
    if (state)
        delete state;

    state = s;
    pl.ui_dirty = true; /* state change always requires a ui rebuild. */
}

void
prepare_chunks()
{
    /* walk all the chunks -- TODO: only walk chunks that might contribute to the view */
    for (int k = ship->min_z; k <= ship->max_z; k++) {
        for (int j = ship->min_y; j <= ship->max_y; j++) {
            for (int i = ship->min_x; i <= ship->max_x; i++) {
                chunk *ch = ship->get_chunk(i, j, k);
                if (ch) {
                    ch->prepare_render(i, j, k);
                }
            }
        }
    }
}

void
init()
{
    gas_man.create_component_instance_data(20);
    light_man.create_component_instance_data(20);
    pos_man.create_component_instance_data(20);
    power_man.create_component_instance_data(20);
    render_man.create_component_instance_data(20);
    switch_man.create_component_instance_data(20);
    switchable_man.create_component_instance_data(20);

    proj_man.create_projectile_data(1000);

    printf("%s starting up.\n", APP_NAME);
    printf("OpenGL version: %.1f\n", epoxy_gl_version() / 10.0f);

    if (epoxy_gl_version() < 33) {
        errx(1, "At least OpenGL 3.3 is required\n");
    }

    /* Enable GL debug extension */
    if (!epoxy_has_gl_extension("GL_KHR_debug"))
        errx(1, "No support for GL debugging, life isn't worth it.\n");

    glEnable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(reinterpret_cast<GLDEBUGPROC>(&gl_debug_callback), nullptr);

    /* Check for ARB_texture_storage */
    if (!epoxy_has_gl_extension("GL_ARB_texture_storage"))
        errx(1, "No support for ARB_texture_storage\n");

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);         /* pointers given by other libs may not be aligned */
    glEnable(GL_DEPTH_TEST);

    mesher_init();

    projectile_sw = load_mesh("mesh/cube.obj");
    set_mesh_material(projectile_sw, 3);
    projectile_hw = upload_mesh(projectile_sw);

    attachment_sw = load_mesh("mesh/attach.obj");
    set_mesh_material(attachment_sw, 10);
    attachment_hw = upload_mesh(attachment_sw);

    wire_sw = load_mesh("mesh/wire.obj");
    set_mesh_material(wire_sw, 11);
    wire_hw = upload_mesh(wire_sw);

    scaffold_sw = load_mesh("mesh/initial_scaffold.obj");

    surfs_sw[surface_xp] = load_mesh("mesh/x_quad_p.obj");
    surfs_sw[surface_xm] = load_mesh("mesh/x_quad.obj");
    surfs_sw[surface_yp] = load_mesh("mesh/y_quad_p.obj");
    surfs_sw[surface_ym] = load_mesh("mesh/y_quad.obj");
    surfs_sw[surface_zp] = load_mesh("mesh/z_quad_p.obj");
    surfs_sw[surface_zm] = load_mesh("mesh/z_quad.obj");

    for (int i = 0; i < 6; i++)
        surfs_hw[i] = upload_mesh(surfs_sw[i]);

    entity_types[0].sw = load_mesh("mesh/frobnicator.obj");
    set_mesh_material(entity_types[0].sw, 3);
    entity_types[0].hw = upload_mesh(entity_types[0].sw);
    entity_types[0].name = "Frobnicator";
    build_static_physics_mesh(entity_types[0].sw, &entity_types[0].phys_mesh, &entity_types[0].phys_shape);

    entity_types[1].sw = load_mesh("mesh/panel_4x4.obj");
    set_mesh_material(entity_types[1].sw, 7);
    entity_types[1].hw = upload_mesh(entity_types[1].sw);
    entity_types[1].name = "Display Panel (4x4)";
    build_static_physics_mesh(entity_types[1].sw, &entity_types[1].phys_mesh, &entity_types[1].phys_shape);

    entity_types[2].sw = load_mesh("mesh/panel_4x4.obj");
    set_mesh_material(entity_types[2].sw, 8);
    entity_types[2].hw = upload_mesh(entity_types[2].sw);
    entity_types[2].name = "Light (4x4)";
    build_static_physics_mesh(entity_types[2].sw, &entity_types[2].phys_mesh, &entity_types[2].phys_shape);

    entity_types[3].sw = load_mesh("mesh/panel_1x1.obj");
    set_mesh_material(entity_types[3].sw, 9);
    entity_types[3].hw = upload_mesh(entity_types[3].sw);
    entity_types[3].name = "Switch";
    build_static_physics_mesh(entity_types[3].sw, &entity_types[3].phys_mesh, &entity_types[3].phys_shape);

    simple_shader = load_shader("shaders/simple.vert", "shaders/simple.frag");
    unlit_shader = load_shader("shaders/simple.vert", "shaders/unlit.frag");
    unlit_instanced_shader = load_shader("shaders/simple_instanced.vert", "shaders/unlit.frag");
    lit_instanced_shader = load_shader("shaders/simple_instanced.vert", "shaders/simple.frag");
    add_overlay_shader = load_shader("shaders/add_overlay.vert", "shaders/unlit.frag");
    remove_overlay_shader = load_shader("shaders/remove_overlay.vert", "shaders/unlit.frag");
    ui_shader = load_shader("shaders/ui.vert", "shaders/ui.frag");
    ui_sprites_shader = load_shader("shaders/ui_sprites.vert", "shaders/ui_sprites.frag");
    sky_shader = load_shader("shaders/sky.vert", "shaders/sky.frag");

    scaffold_hw = upload_mesh(scaffold_sw);         /* needed for overlay */

    glUseProgram(simple_shader);

    per_object = new shader_params<per_object_params>;

    per_object->val.world_matrix = glm::mat4(1);    /* identity */

    per_object->bind(1);

    world_textures = new texture_set(GL_TEXTURE_2D_ARRAY, WORLD_TEXTURE_DIMENSION, MAX_WORLD_TEXTURES);
    world_textures->load(0, "textures/white.png");
    world_textures->load(1, "textures/scaffold.png");
    world_textures->load(2, "textures/plate.png");
    world_textures->load(3, "textures/frobnicator.png");
    world_textures->load(4, "textures/grate.png");
    world_textures->load(5, "textures/red.png");
    world_textures->load(6, "textures/glass.png");
    world_textures->load(7, "textures/display.png");
    world_textures->load(8, "textures/light.png");
    world_textures->load(9, "textures/switch.png");
    world_textures->load(10, "textures/attach.png");
    world_textures->load(11, "textures/wire.png");

    skybox = new texture_set(GL_TEXTURE_CUBE_MAP, 2048, 6);
    skybox->load(0, "textures/sky_right1.png");
    skybox->load(1, "textures/sky_left2.png");
    skybox->load(2, "textures/sky_top3.png");
    skybox->load(3, "textures/sky_bottom4.png");
    skybox->load(4, "textures/sky_front5.png");
    skybox->load(5, "textures/sky_back6.png");

    ship = ship_space::mock_ship_space();
    if( ! ship )
        errx(1, "Ship_space::mock_ship_space failed\n");

    ship->rebuild_topology();

    printf("Ship is %u chunks, %d..%d %d..%d %d..%d\n",
            (unsigned) ship->chunks.size(),
            ship->min_x, ship->max_x,
            ship->min_y, ship->max_y,
            ship->min_z, ship->max_z);

    ship->validate();

    game_settings = load_settings(en_config_base);
    en_settings user_settings = load_settings(en_config_user);
    game_settings.merge_with(user_settings);

    frames = new frame_data[NUM_INFLIGHT_FRAMES];
    frame_index = 0;

    pl.angle = 0;
    pl.elev = 0;
    pl.pos = glm::vec3(3,2,2);
    pl.selected_slot = 1;
    pl.ui_dirty = true;
    pl.disable_gravity = false;

    phy = new physics(&pl);

    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CW);

    text = new text_renderer("fonts/pixelmix.ttf", 16);

    ui_sprites = new sprite_renderer();
    unlit_ui_slot_sprite = ui_sprites->load("textures/ui-slot.png");
    lit_ui_slot_sprite = ui_sprites->load("textures/ui-slot-lit.png");

    printf("World vertex size: %lu bytes\n", sizeof(vertex));

    light = new light_field();
    light->bind(1);

    /* put some crap in the lightfield */
    memset(light->data, 0, sizeof(light->data));
    light->upload();

    /* prepare the chunks -- this populates the physics data */
    prepare_chunks();
}


void
resize(int width, int height)
{
    /* TODO: resize offscreen (but screen-sized) surfaces, etc. */
    glViewport(0, 0, width, height);
    wnd.width = width;
    wnd.height = height;
    printf("Resized to %dx%d\n", width, height);
}


void
remove_ents_from_surface(int x, int y, int z, int face)
{
    chunk *ch = ship->get_chunk_containing(x, y, z);
    for (auto it = ch->entities.begin(); it != ch->entities.end(); /* */) {
        entity *e = *it;
        if (e->x == x && e->y == y && e->z == z && e->face == face) {
            if (pos_man.exists(e->ce)) {
                pos_man.destroy_entity_instance(e->ce);
            }

            if (render_man.exists(e->ce)) {
                render_man.destroy_entity_instance(e->ce);
            }

            if (power_man.exists(e->ce)) {
                power_man.destroy_entity_instance(e->ce);
            }

            if (gas_man.exists(e->ce)) {
                gas_man.destroy_entity_instance(e->ce);
            }

            if (light_man.exists(e->ce)) {
                light_man.destroy_entity_instance(e->ce);
            }

            if (switch_man.exists(e->ce)) {
                switch_man.destroy_entity_instance(e->ce);
            }

            if (switchable_man.exists(e->ce)) {
                switchable_man.destroy_entity_instance(e->ce);
            }

            delete e;
            it = ch->entities.erase(it);
        }
        else {
            it++;
        }
    }

    block *bl = ship->get_block(x, y, z);
    assert(bl);

    bl->surf_space[face] = 0;   /* we've popped *everything* off, it must be empty now */

    if (face == surface_zm) {

        if (bl->type == block_entity)
            bl->type = block_empty;

        ch->render_chunk.valid = false;
    }
}


struct add_block_entity_tool : tool
{
    entity_type *type;

    explicit add_block_entity_tool(entity_type *type) : type(type) {}

    bool can_use(raycast_info *rc) {
        if (!rc->hit || rc->inside)
            return false;

        /* don't allow placements that would cause the player to end up inside the ent and get stuck */
        glm::ivec3 pos(rc->px, rc->py, rc->pz);
        if (pos == get_block_containing(pl.eye) ||
            pos == get_block_containing(pl.pos))
            return false;

        block *bl = ship->get_block(rc->px, rc->py, rc->pz);

        if (bl) {
            /* check for surface ents that would conflict */
            for (int face = 0; face < face_count; face++)
                if (bl->surf_space[face])
                    return false;
        }

        /* block ents can only be placed in empty space, on a scaffold */
        return bl && rc->block->type == block_support;
    }

    void use(raycast_info *rc) override {
        if (!can_use(rc))
            return;

        /* dirty the chunk -- TODO: do we really have to do this when changing a cell from
         * empty -> entity? */
        chunk *ch = ship->get_chunk_containing(rc->px, rc->py, rc->pz);
        ch->render_chunk.valid = false;
        ch->entities.push_back(
            new entity(rc->px, rc->py, rc->pz, type, surface_zm)
            );

        block *bl = ship->get_block(rc->px, rc->py, rc->pz);
        bl->type = block_entity;

        /* consume ALL the space on the surfaces */
        for (int face = 0; face < face_count; face++)
            bl->surf_space[face] = ~0;
    }

    void preview(raycast_info *rc) override {
        if (!can_use(rc))
            return;

        per_object->val.world_matrix = mat_position((float)rc->px, (float)rc->py, (float)rc->pz);
        per_object->upload();

        draw_mesh(type->hw);

        /* draw a block overlay as well around the frobnicator */
        glUseProgram(add_overlay_shader);
        draw_mesh(scaffold_hw);
        glUseProgram(simple_shader);
    }

    void get_description(char *str) override {
        sprintf(str, "Place %s", type->name);
    }
};


struct add_surface_entity_tool : tool
{
    entity_type *type;

    add_surface_entity_tool(entity_type *type) : type(type) {}

    bool can_use(raycast_info *rc) {
        if (!rc->hit)
            return false;

        block *bl = rc->block;

        if (!bl)
            return false;

        int index = normal_to_surface_index(rc);

        if (bl->surfs[index] == surface_none)
            return false;

        block *other_side = ship->get_block(rc->px, rc->py, rc->pz);
        unsigned short required_space = ~0; /* TODO: make this a prop of the type + subblock placement */

        if (other_side->surf_space[index ^ 1] & required_space) {
            /* no room on the surface */
            return false;
        }

        return true;
    }

    void use(raycast_info *rc) override {
        if (!can_use(rc))
            return;

        int index = normal_to_surface_index(rc);

        block *other_side = ship->get_block(rc->px, rc->py, rc->pz);
        unsigned short required_space = ~0; /* TODO: make this a prop of the type + subblock placement */

        chunk *ch = ship->get_chunk_containing(rc->px, rc->py, rc->pz);
        /* the chunk we're placing into is guaranteed to exist, because there's
         * a surface facing into it */
        assert(ch);
        ch->entities.push_back(
            new entity(rc->px, rc->py, rc->pz, type, index ^ 1)
            );

        /* take the space. */
        other_side->surf_space[index ^ 1] |= required_space;

        /* mark lighting for rebuild around this point */
        mark_lightfield_update(rc->px, rc->py, rc->pz);
    }

    void preview(raycast_info *rc) override {
        if (!can_use(rc))
            return;

        int index = normal_to_surface_index(rc);

        per_object->val.world_matrix = mat_block_face(rc->px, rc->py, rc->pz, index ^ 1);
        per_object->upload();

        draw_mesh(type->hw);

        /* draw a surface overlay here too */
        /* TODO: sub-block placement granularity -- will need a different overlay */
        per_object->val.world_matrix = mat_position((float)rc->x, (float)rc->y, (float)rc->z);
        per_object->upload();

        glUseProgram(add_overlay_shader);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-0.1f, -0.1f);
        draw_mesh(surfs_hw[index]);
        glPolygonOffset(0.0f, 0.0f);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glUseProgram(simple_shader);
    }

    void get_description(char *str) override {
        sprintf(str, "Place %s on surface", type->name);
    }
};


struct remove_surface_entity_tool : tool
{
    bool can_use(raycast_info *rc) {
        return rc->hit;
    }

    void use(raycast_info *rc) override {
        if (!can_use(rc))
            return;

        int index = normal_to_surface_index(rc);
        remove_ents_from_surface(rc->px, rc->py, rc->pz, index^1);
        mark_lightfield_update(rc->px, rc->py, rc->pz);
    }

    void preview(raycast_info *rc) override {
        if (!can_use(rc))
            return;

        int index = normal_to_surface_index(rc);
        block *other_side = ship->get_block(rc->px, rc->py, rc->pz);

        if (!other_side || !other_side->surf_space[index ^ 1]) {
            return;
        }

        per_object->val.world_matrix = mat_position((float)rc->x, (float)rc->y, (float)rc->z);
        per_object->upload();

        glUseProgram(remove_overlay_shader);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-0.1f, -0.1f);
        draw_mesh(surfs_hw[index]);
        glPolygonOffset(0.0f, 0.0f);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glUseProgram(simple_shader);
    }

    void get_description(char *str) override {
        strcpy(str, "Remove surface entity");
    }
};

struct add_wiring_tool : tool
{
    wire *active_wire = nullptr;
    wire_segment *current_segment = nullptr;

    bool get_attach_point(bool & is_entity, glm::vec3 & pt, glm::vec3 & normal) {
        auto end = pl.eye + pl.dir * 5.0f;

        // don't clamp to edges if we're looking at entity

        is_entity = nullptr != phys_raycast(pl.eye, end,
                                                  phy->ghostObj, phy->dynamicsWorld);

        auto hit = phys_raycast_generic(pl.eye, end,
                                        phy->ghostObj, phy->dynamicsWorld);

        if (!hit.hit)
            return false;

        // offset 0.05 as that's how model is
        pt = hit.hitCoord + hit.hitNormal * 0.05f;
        normal = hit.hitNormal;

        if (!is_entity) {
            auto frac = pt - glm::floor(pt);
            auto diff = glm::abs(glm::vec3(0.5f) - frac);

            /* snap the two components closest to the edge, to the edge */
            if (diff.x > diff.y || diff.x > diff.z) {
                frac.x = frac.x < 0.5f ? 0.1f : 0.9f;
            }
            if (diff.y >= diff.x || diff.y > diff.z) {
                frac.y = frac.y < 0.5f ? 0.1f : 0.9f;
            }
            if (diff.z >= diff.x || diff.z >= diff.y) {
                frac.z = frac.z < 0.5f ? 0.1f : 0.9f;
            }

            pt = frac + glm::floor(pt);
        }
        return true;
    }

    void preview(raycast_info *rc) override {
        /* do a real, generic raycast */

        /* TODO: handle hitting ents -- we want to connect to the ent. */
        /* TODO: ignore the player properly. this involves fixing the phys
         * raycast code slightly */
        /* TODO: handle some weird cases in negative space. current impl is
         * not quite correct */

        bool hit_entity;
        glm::vec3 pt;
        glm::vec3 normal;

        if (!get_attach_point(hit_entity, pt, normal))
            return;

        if (!hit_entity && (!active_wire || !active_wire->segments.size()))
            return;

        per_object->val.world_matrix = mat_rotate_mesh(pt, normal);
        per_object->upload();

        glUseProgram(unlit_shader);
        draw_mesh(attachment_hw);
        glUseProgram(simple_shader);

        if (!active_wire || !current_segment)
            return;

        if (current_segment->first == ~0u)
            return;

        wire_attachment a1;
        if (current_segment->second == ~0u)
            a1 = wire_attachments[current_segment->first];
        else
            a1 = wire_attachments[current_segment->second];

        wire_attachment a2 = { mat_position(pt) };

        auto mat = calc_segment_matrices(a1, a2);

        per_object->val.world_matrix = mat;
        per_object->upload();

        glUseProgram(unlit_shader);
        draw_mesh(wire_hw);
        glUseProgram(simple_shader);
    }

    void use(raycast_info *rc) override {
        bool hit_entity;
        glm::vec3 pt;
        glm::vec3 normal;

        if (!get_attach_point(hit_entity, pt, normal))
            return;

        if (!hit_entity && (!active_wire || !active_wire->segments.size()))
            return;

        if (!active_wire && hit_entity) {
            wire w;
            wires.push_back(w);
            active_wire = &wires[wires.size() - 1];
        }

        if (!current_segment || segment_finished(current_segment)) {
            active_wire->segments.push_back(wire_segment());
            current_segment = &active_wire->segments[active_wire->segments.size() - 1];
            if (active_wire->segments.size() > 1) {
                current_segment->first =
                    active_wire->segments[active_wire->segments.size() - 2].second;
            }
        }

        wire_attachment wa;
        wa.transform = mat_rotate_mesh(pt, normal);
        wire_attachments.push_back(wa);

        if (current_segment->first == ~0u) {
            current_segment->first = wire_attachments.size() - 1;
        }
        else {
            current_segment->second = wire_attachments.size() - 1;

            if (hit_entity)
                active_wire = nullptr;
        }
    }

    void get_description(char *str) override {
        strcpy(str, "Place wiring");
    }
};


tool *tools[] = {
    tool::create_fire_projectile_tool(&pl),
    tool::create_add_block_tool(),
    tool::create_remove_block_tool(),
    tool::create_add_surface_tool(surface_wall),
    tool::create_add_surface_tool(surface_grate),
    tool::create_add_surface_tool(surface_glass),
    tool::create_remove_surface_tool(),
    new add_block_entity_tool(&entity_types[0]),
    new add_surface_entity_tool(&entity_types[1]),
    new add_surface_entity_tool(&entity_types[2]),
    new add_surface_entity_tool(&entity_types[3]),
    new remove_surface_entity_tool(),
    new add_wiring_tool(),
};


void
add_text_with_outline(char const *s, float x, float y, float r = 1, float g = 1, float b = 1)
{
    text->add(s, x - 2, y, 0, 0, 0);
    text->add(s, x + 2, y, 0, 0, 0);
    text->add(s, x, y - 2, 0, 0, 0);
    text->add(s, x, y + 2, 0, 0, 0);
    text->add(s, x, y, r, g, b);
}


struct time_accumulator
{
    float period;
    float accum;

    time_accumulator(float period) :
        period(period), accum(0.0f) {}

    void add(float dt) { accum += dt; }

    bool tick()
    {
        if (accum >= period) {
            accum -= period;
            return true;
        }

        return false;
    }
};


time_accumulator main_tick_accum(1/15.0f);  /* 15Hz tick for game logic */
time_accumulator fast_tick_accum(1/60.0f);  /* 60Hz tick for motion */

void
update()
{
    frame_info.tick();
    auto dt = frame_info.dt;

    frame = &frames[frame_index++];
    if (frame_index >= NUM_INFLIGHT_FRAMES) {
        frame_index = 0;
    }

    frame->begin();

    float depthClearValue = 1.0f;
    glClearBufferfv(GL_DEPTH, 0, &depthClearValue);

    pl.dir = glm::vec3(
            cosf(pl.angle) * cosf(pl.elev),
            sinf(pl.angle) * cosf(pl.elev),
            sinf(pl.elev)
            );

    pl.eye = pl.pos + glm::vec3(0, 0, EYE_OFFSET_Z);

    auto vfov = hfov * (float)wnd.height / wnd.width;

    glm::mat4 proj = glm::perspective(vfov, (float)wnd.width / wnd.height, 0.01f, 1000.0f);
    glm::mat4 view = glm::lookAt(pl.eye, pl.eye + pl.dir, glm::vec3(0, 0, 1));
    glm::mat4 centered_view = glm::lookAt(glm::vec3(0), pl.dir, glm::vec3(0, 0, 1));

    auto camera_params = frame->alloc_aligned<per_camera_params>(1);

    camera_params.ptr->view_proj_matrix = proj * view;
    camera_params.ptr->inv_centered_view_proj_matrix = glm::inverse(proj * centered_view);
    camera_params.ptr->aspect = (float)wnd.width / wnd.height;
    camera_params.bind(0, frame);

    main_tick_accum.add(dt);
    fast_tick_accum.add(dt);

    /* this absolutely must run every frame */
    state->update(dt);

    /* things that can run at a pretty slow rate */
    while (main_tick_accum.tick()) {

        /* rebuild lighting if needed */
        update_lightfield();

        /* remove any air that someone managed to get into the outside */
        {
            topo_info *t = topo_find(&ship->outside_topo_info);
            zone_info *z = ship->get_zone_info(t);
            if (z) {
                /* try as hard as you like, you cannot fill space with your air system */
                z->air_amount = 0;
            }
        }

        /* allow the entities to tick */
        tick_gas_producers();

        /* HACK: dirty this every frame for now while debugging atmo */
        if (1 || pl.ui_dirty) {
            text->reset();
            ui_sprites->reset();
            state->rebuild_ui();

            char buf[3][256];
            float w[3] = { 0, 0, 0 }, h = 0;

            sprintf(buf[0], "%.2f", frame_info.dt * 1000);
            sprintf(buf[1], "%.2f", 1.f / frame_info.dt);
            sprintf(buf[2], "%.2f", frame_info.fps);

            text->measure(buf[0], &w[0], &h);
            text->measure(buf[1], &w[1], &h);
            text->measure(buf[2], &w[2], &h);

            add_text_with_outline(buf[0], -DEFAULT_WIDTH / 2 + (100 - w[0]), DEFAULT_HEIGHT / 2 + 100);
            add_text_with_outline(buf[1], -DEFAULT_WIDTH / 2 + (100 - w[1]), DEFAULT_HEIGHT / 2 + 82);
            add_text_with_outline(buf[2], -DEFAULT_WIDTH / 2 + (100 - w[2]), DEFAULT_HEIGHT / 2 + 64);

            text->upload();
            ui_sprites->upload();
            pl.ui_dirty = false;
        }
    }

    /* character controller tick: we'd LIKE to run this off the fast_tick_accum, but it has all kinds of
     * every-frame assumptions baked in (player impulse state, etc) */
    phy->tick_controller(dt);

    while (fast_tick_accum.tick()) {

        proj_man.simulate(fast_tick_accum.period);

        phy->tick(fast_tick_accum.period);

    }

    world_textures->bind(0);

    prepare_chunks();

    for (int k = ship->min_z; k <= ship->max_z; k++) {
        for (int j = ship->min_y; j <= ship->max_y; j++) {
            for (int i = ship->min_x; i <= ship->max_x; i++) {
                /* TODO: prepare all the matrices first, and do ONE upload */
                chunk *ch = ship->get_chunk(i, j, k);
                if (ch) {
                    auto chunk_matrix = frame->alloc_aligned<glm::mat4>(1);
                    *chunk_matrix.ptr = mat_position(
                                (float)i * CHUNK_SIZE, (float)j * CHUNK_SIZE, (float)k * CHUNK_SIZE);
                    chunk_matrix.bind(1, frame);
                    draw_mesh(ch->render_chunk.mesh);
                }
            }
        }
    }

    draw_renderables(frame);

    /* draw the projectiles */
    glUseProgram(unlit_instanced_shader);
    draw_projectiles(frame);
    glUseProgram(lit_instanced_shader);
    draw_attachments(frame);
    draw_segments(frame);

    per_object->bind(1);

    /* draw the sky */
    glUseProgram(sky_shader);
    skybox->bind(0);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    if (draw_hud) {
        /* draw the ui */
        glDisable(GL_DEPTH_TEST);

        glUseProgram(ui_shader);
        text->draw();
        glUseProgram(ui_sprites_shader);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ui_sprites->draw();
        glDisable(GL_BLEND);

        glEnable(GL_DEPTH_TEST);
    }

    glUseProgram(simple_shader);

    frame->end();
}


action const* get_input(en_action a) {
    return &game_settings.bindings.bindings[a];
}


struct play_state : game_state {
    play_state() {
    }

    void rebuild_ui() override {
        float w = 0;
        float h = 0;
        char buf[256];
        char buf2[512];

        {
            /* Tool name down the bottom */
            tool *t = tools[pl.selected_slot];

            if (t) {
                t->get_description(buf);
            }
            else {
                strcpy(buf, "(no tool)");
            }
        }

        text->measure(".", &w, &h);
        add_text_with_outline(".", -w/2, -w/2);

        sprintf(buf2, "Left mouse button: %s", buf);
        text->measure(buf2, &w, &h);
        add_text_with_outline(buf2, -w/2, -400);

        /* Gravity state (temp) */
        w = 0; h = 0;
        sprintf(buf, "Gravity: %s (G to toggle)", pl.disable_gravity ? "OFF" : "ON");
        text->measure(buf, &w, &h);
        add_text_with_outline(buf, -w/2, -430);

        /* Use key affordance */
        if (use_entity) {
            sprintf(buf2, "(E) Use the %s", use_entity->type->name);
            w = 0; h = 0;
            text->measure(buf2, &w, &h);
            add_text_with_outline(buf2, -w/2, -200);
        }

        {
            /* Atmo status */
            glm::ivec3 eye_block = get_block_containing(pl.eye);

            topo_info *t = topo_find(ship->get_topo_info(eye_block.x, eye_block.y, eye_block.z));
            topo_info *outside = topo_find(&ship->outside_topo_info);
            zone_info *z = ship->get_zone_info(t);
            float pressure = z ? (z->air_amount / t->size) : 0.0f;

            if (t != outside) {
                sprintf(buf2, "[INSIDE %p %d %.1f atmo]", t, t->size, pressure);
            }
            else {
                sprintf(buf2, "[OUTSIDE %p %d %.1f atmo]", t, t->size, pressure);
            }

            w = 0; h = 0;
            text->measure(buf2, &w, &h);
            add_text_with_outline(buf2, -w/2, -100);

            w = 0; h = 0;
            sprintf(buf2, "full: %d fast-unify: %d fast-nosplit: %d false-split: %d",
                    ship->num_full_rebuilds,
                    ship->num_fast_unifys,
                    ship->num_fast_nosplits,
                    ship->num_false_splits);
            text->measure(buf2, &w, &h);
            add_text_with_outline(buf2, -w/2, -150);
        }

        unsigned num_tools = sizeof(tools) / sizeof(tools[0]);
        for (unsigned i = 0; i < num_tools; i++) {
            ui_sprites->add(pl.selected_slot == i ? &lit_ui_slot_sprite : &unlit_ui_slot_sprite,
                    (i - num_tools/2.0f) * 34, -220);
        }
    }

    void update(float dt) override {
        if (wnd.has_focus && SDL_GetRelativeMouseMode() == SDL_FALSE) {
            SDL_SetRelativeMouseMode(SDL_TRUE);
        }

        if (!wnd.has_focus && SDL_GetRelativeMouseMode() != SDL_FALSE) {
            SDL_SetRelativeMouseMode(SDL_FALSE);            
        }

        tool *t = tools[pl.selected_slot];

        /* both tool use and overlays need the raycast itself */
        raycast_info rc;
        ship->raycast(pl.eye, pl.dir, &rc);

        /* tool use */
        if (pl.use_tool && t) {
            t->use(&rc);
        }

        /* interact with ents */
        entity *hit_ent = phys_raycast(pl.eye, pl.eye + 2.f * pl.dir,
                                       phy->ghostObj, phy->dynamicsWorld);

        if (hit_ent != use_entity) {
            use_entity = hit_ent;
            pl.ui_dirty = true;
        }

        if (pl.use && hit_ent) {
            hit_ent->use();
        }

        /* tool preview */
        if (rc.hit && t) {
            t->preview(&rc);
        }
    }

    void set_slot(int slot) {
        pl.selected_slot = slot;
        pl.ui_dirty = true;
    }

    void cycle_slot(int d) {
        auto num_tools = sizeof(tools) / sizeof(tools[0]);
        unsigned int cur_slot = pl.selected_slot;
        cur_slot = (cur_slot + num_tools + d) % num_tools;

        pl.selected_slot = cur_slot;
        pl.ui_dirty = true;
    }

    void handle_input() override {
        /* look */
        auto look_x     = get_input(action_look_x)->value;
        auto look_y     = get_input(action_look_y)->value;

        /* movement */
        auto moveX      = get_input(action_right)->active - get_input(action_left)->active;
        auto moveY      = get_input(action_forward)->active - get_input(action_back)->active;

        /* crouch */
        auto crouch     = get_input(action_crouch)->active;
        auto crouch_end = get_input(action_crouch)->just_inactive;

        /* momentary */
        auto jump       = get_input(action_jump)->just_active;
        auto reset      = get_input(action_reset)->just_active;
        auto use        = get_input(action_use)->just_active;
        auto slot1      = get_input(action_slot1)->just_active;
        auto slot2      = get_input(action_slot2)->just_active;
        auto slot3      = get_input(action_slot3)->just_active;
        auto slot4      = get_input(action_slot4)->just_active;
        auto slot5      = get_input(action_slot5)->just_active;
        auto slot6      = get_input(action_slot6)->just_active;
        auto slot7      = get_input(action_slot7)->just_active;
        auto slot8      = get_input(action_slot8)->just_active;
        auto slot9      = get_input(action_slot9)->just_active;
        auto slot0      = get_input(action_slot0)->just_active;
        auto gravity    = get_input(action_gravity)->just_active;
        auto use_tool   = get_input(action_use_tool)->just_active;
        auto next_tool  = get_input(action_tool_next)->just_active;
        auto prev_tool  = get_input(action_tool_prev)->just_active;

        /* persistent */

        float mouse_invert = game_settings.input.mouse_invert;

        pl.angle += game_settings.input.mouse_x_sensitivity * look_x;
        pl.elev += game_settings.input.mouse_y_sensitivity * mouse_invert * look_y;

        if (pl.elev < -MOUSE_Y_LIMIT)
            pl.elev = -MOUSE_Y_LIMIT;
        if (pl.elev > MOUSE_Y_LIMIT)
            pl.elev = MOUSE_Y_LIMIT;

        pl.move = glm::vec2((float) moveX, (float) moveY);

        pl.jump       = jump;
        pl.crouch     = crouch;
        pl.reset      = reset;
        pl.crouch_end = crouch_end;
        pl.use        = use;
        pl.gravity    = gravity;
        pl.use_tool   = use_tool;

        // blech. Tool gets used below, then fire projectile gets hit here
        if (pl.fire_projectile) {
            auto below_eye = glm::vec3(pl.eye.x, pl.eye.y, pl.eye.z - 0.1);
            proj_man.spawn(below_eye, pl.dir, *projectile_hw);
            pl.fire_projectile = false;
        }

        if (next_tool) {
            cycle_slot(1);
        }
        if (prev_tool) {
            cycle_slot(-1);
        }

        if (slot1) set_slot(1);
        if (slot2) set_slot(2);
        if (slot3) set_slot(3);
        if (slot4) set_slot(4);
        if (slot5) set_slot(5);
        if (slot6) set_slot(6);
        if (slot7) set_slot(7);
        if (slot8) set_slot(8);
        if (slot9) set_slot(9);
        if (slot0) set_slot(0);

        /* limit to unit vector */
        float len = glm::length(pl.move);
        if (len > 0.0f)
            pl.move = pl.move / len;

        if (get_input(action_menu)->just_active) {
            set_game_state(create_menu_state());
        }
    }
};


struct menu_state : game_state
{
    typedef std::pair<char const *, std::function<void()>> menu_item;
    std::vector<menu_item> items;
    int selected = 0;

    menu_state() : items() {
        items.push_back(menu_item("Resume Game", []{ set_game_state(create_play_state()); }));
        items.push_back(menu_item("Settings", []{ set_game_state(create_menu_settings_state()); }));
        items.push_back(menu_item("Exit Game", []{ exit_requested = true; }));
    }

    void update(float dt) override {
        if (wnd.has_focus && SDL_GetRelativeMouseMode() == SDL_TRUE) {
            SDL_SetRelativeMouseMode(SDL_FALSE);
        }
    }

    void put_item_text(char *dest, char const *src, int index) {
        if (index == selected)
            sprintf(dest, "> %s <", src);
        else
            strcpy(dest, src);
    }

    void rebuild_ui() override {
        float w = 0;
        float h = 0;
        char buf[256];

        sprintf(buf, "Engineer's Nightmare");
        text->measure(buf, &w, &h);
        add_text_with_outline(buf, -w/2, 300);

        float y = 50;
        float dy = -100;

        for (auto it = items.begin(); it != items.end(); it++) {
            w = 0;
            h = 0;
            put_item_text(buf, it->first, it - items.begin());
            text->measure(buf, &w, &h);
            add_text_with_outline(buf, -w/2, y);
            y += dy;
        }
    }

    void handle_input() override {
        if (get_input(action_menu_confirm)->just_active) {
            items[selected].second();
        }

        if (get_input(action_menu_down)->just_active) {
            selected = (selected + 1) % items.size();
            pl.ui_dirty = true;
        }

        if (get_input(action_menu_up)->just_active) {
            selected = (selected + items.size() - 1) % items.size();
            pl.ui_dirty = true;
        }

        if (get_input(action_menu)->just_active) {
            set_game_state(create_play_state());
        }
    }
};


struct menu_settings_state : game_state
{
    typedef std::tuple<char const *, char const *, std::function<void()>> menu_item;
    std::vector<menu_item> items;
    int selected = 0;

    char const *on_text = "On";
    char const *off_text = "Off";

    char const *invert_mouse_text = "Invert Mouse: ";

    Uint32 mouse_invert_mi = 0;

    menu_settings_state() {
        mouse_invert_mi = items.size();
        items.push_back(menu_item(invert_mouse_text, "",
            []{ toggle_mouse_invert(); }));
            // ^^ Not real keen on requiring these to be static

        items.push_back(
            menu_item("Save Settings", "",
            []{ save_settings(game_settings); }));

        items.push_back(
            menu_item("Back", "",
            []{ set_game_state(create_menu_state()); }));
    }

    static void toggle_mouse_invert() {
    // ^^ Not real keen on requiring these to be static
        game_settings.input.mouse_invert *= -1;
    }

    void update(float dt) override {
        if (wnd.has_focus && SDL_GetRelativeMouseMode() == SDL_TRUE) {
            SDL_SetRelativeMouseMode(SDL_FALSE);
        }
    }

    void put_item_text(char *dest, char const *src, int index) {
        if (index == selected)
            sprintf(dest, "> %s <", src);
        else
            strcpy(dest, src);
    }

    void rebuild_ui() override {
        menu_item *invert_item = &items.at(mouse_invert_mi);
        std::get<1>(*invert_item) = game_settings.input.mouse_invert > 0 ? off_text : on_text;

        float w = 0;
        float h = 0;
        char buf[256];
        char buf2[256];

        sprintf(buf, "Engineer's Nightmare");
        text->measure(buf, &w, &h);
        add_text_with_outline(buf, -w / 2, 300);

        float y = 50;
        float dy = -100;

        for (auto it = items.begin(); it != items.end(); ++it) {
            w = 0;
            h = 0;
            sprintf(buf2, "%s%s", std::get<0>(*it), std::get<1>(*it));
            put_item_text(buf, buf2, it - items.begin());
            text->measure(buf, &w, &h);
            add_text_with_outline(buf, -w / 2, y);
            y += dy;
        }
    }


    void handle_input() override {
        if (get_input(action_menu_confirm)->just_active) {
            std::get<2>(items[selected])();
            pl.ui_dirty = true;
        }

        if (get_input(action_menu_down)->just_active) {
            selected = (selected + 1) % items.size();
            pl.ui_dirty = true;
        }

        if (get_input(action_menu_up)->just_active) {
            selected = (selected + items.size() - 1) % items.size();
            pl.ui_dirty = true;
        }

        if (get_input(action_menu)->just_active) {
            set_game_state(create_play_state());
        }
    }
};


game_state *game_state::create_play_state() { return new play_state; }
game_state *game_state::create_menu_state() { return new menu_state; }
game_state *game_state::create_menu_settings_state() { return new menu_settings_state; }


void
handle_input()
{
    if (wnd.has_focus) {
        set_inputs(keys, mouse_buttons, mouse_axes, game_settings.bindings.bindings);
        state->handle_input();
    }
}


void
run()
{
    for (;;) {
        auto sdl_buttons = SDL_GetRelativeMouseState(nullptr, nullptr);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_left)]      = sdl_buttons & EN_SDL_BUTTON(input_mouse_left);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_middle)]    = sdl_buttons & EN_SDL_BUTTON(input_mouse_middle);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_right)]     = sdl_buttons & EN_SDL_BUTTON(input_mouse_right);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_thumb1)]    = sdl_buttons & EN_SDL_BUTTON(input_mouse_thumb1);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_thumb2)]    = sdl_buttons & EN_SDL_BUTTON(input_mouse_thumb2);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_wheeldown)] = false;
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_wheelup)]   = false;

        mouse_axes[EN_MOUSE_AXIS(input_mouse_x)] = 0;
        mouse_axes[EN_MOUSE_AXIS(input_mouse_y)] = 0;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                printf("Quit event caught, shutting down.\n");
                return;

            case SDL_WINDOWEVENT:
                /* We MUST support resize events even if we
                 * don't really care about resizing, because a tiling
                 * WM isn't going to give us what we asked for anyway!
                 */
                switch (e.window.event) {
                case SDL_WINDOWEVENT_RESIZED:
                    resize(e.window.data1, e.window.data2);
                    break;

                case SDL_WINDOWEVENT_FOCUS_LOST:
                    wnd.has_focus = false;
                    break;

                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    wnd.has_focus = true;
                    break;
                }

            case SDL_MOUSEMOTION:
            {
                auto x = e.motion.xrel;
                auto y = e.motion.yrel;

                x = clamp(x, -MAX_AXIS_PER_EVENT, MAX_AXIS_PER_EVENT);
                y = clamp(y, -MAX_AXIS_PER_EVENT, MAX_AXIS_PER_EVENT);

                mouse_axes[EN_MOUSE_AXIS(input_mouse_x)] += x;
                mouse_axes[EN_MOUSE_AXIS(input_mouse_y)] += y;
                break;
            }

            case SDL_MOUSEWHEEL:
                if (e.wheel.y != 0) {
                    e.wheel.y > 0
                        ? mouse_buttons[EN_MOUSE_BUTTON(input_mouse_wheelup)] = true
                        : mouse_buttons[EN_MOUSE_BUTTON(input_mouse_wheeldown)] = true;
                }
                break;
            }
        }

        /* SDL_PollEvent above has already pumped the input, so current key state is available */
        handle_input();

        update();

        SDL_GL_SwapWindow(wnd.ptr);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        if (exit_requested) return;
    }
}

int
main(int, char **)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        errx(1, "Error initializing SDL: %s\n", SDL_GetError());

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    wnd.ptr = SDL_CreateWindow(APP_NAME,
                               SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED,
                               DEFAULT_WIDTH,
                               DEFAULT_HEIGHT,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);

    if (!wnd.ptr)
        errx(1, "Failed to create window.\n");

    wnd.gl_ctx = SDL_GL_CreateContext(wnd.ptr);

    keys = SDL_GetKeyboardState(nullptr);

    resize(DEFAULT_WIDTH, DEFAULT_HEIGHT);

    init();

    run();

    return 0;
}
