#pragma once

#include <epoxy/gl.h>
#include <vector>

struct metrics
{
    int x, y, w, h;

    /* glyph metrics relative to baseline */
    float xoffset, advance, yoffset;
};


struct sprite_metrics
{
    int x, y, w, h;
};


struct text_vertex
{
    float x, y;
    float u, v;
    float r, g, b;
};


struct sprite_vertex
{
    float x, y;
    float u, v;
};


struct texture_atlas
{
    GLuint tex;
    unsigned x, y, h;
    unsigned char *buf;
    unsigned channels;
    unsigned width, height;

    texture_atlas(unsigned channels, unsigned width, unsigned height);
    void add_bitmap(unsigned char *src, int pitch, unsigned width, unsigned height, int *out_x, int *out_y);
    void upload();
    void bind(int texunit);
};

#define USE_TEXT 0
struct text_renderer
{
#if USE_TEXT
    text_renderer(char const *font, int size);

    metrics ms[256];

    GLuint bo;
    GLuint bo_vertex_count;
    GLuint bo_capacity;
    GLuint vao;

    std::vector<text_vertex> verts;

    texture_atlas *atlas;

    void add(char const *str, float x, float y, float r, float g, float b);
    void measure(char const *str, float *x, float *y);
    void upload();
    void reset();
    void draw();

#else

    text_renderer(char const *font, int size) {}

    metrics ms[256];

    GLuint bo;
    GLuint bo_vertex_count;
    GLuint bo_capacity;
    GLuint vao;

    std::vector<text_vertex> verts;

    texture_atlas *atlas;

    void add(char const *str, float x, float y, float r, float g, float b) {}
    void measure(char const *str, float *x, float *y) {}
    void upload() {}
    void reset() {}
    void draw() {}
#endif
};

struct sprite_renderer
{
    sprite_renderer();

    GLuint bo;
    GLuint bo_vertex_count;
    GLuint bo_capacity;
    GLuint vao;

    std::vector<sprite_vertex> verts;

    texture_atlas *atlas;

    sprite_metrics load(char const *str);
    void add(sprite_metrics const *m, float x, float y);
    void upload();
    void reset();
    void draw();
};
