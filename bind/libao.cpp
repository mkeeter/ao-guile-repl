/*
 *  Copyright (C) 2016 Matthew Keeter  <matt.j.keeter@gmail.com>
 *
 *  This file is part of the Ao library.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  Ao is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with Ao.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <iostream>
#include <string>
#include <fstream>
#include <set>
#include <boost/algorithm/string/predicate.hpp>

#include "glm/gtc/matrix_inverse.hpp"

#include "ao/kernel/tree/opcode.hpp"
#include "ao/kernel/tree/tree.hpp"

#include "ao/kernel/eval/evaluator.hpp"

#include "ao/kernel/render/heightmap.hpp"
#include "ao/kernel/format/image.hpp"
#include "ao/kernel/format/mesh.hpp"
#include "ao/kernel/format/contours.hpp"

#include "ao/ui/window.hpp"
#include "ao/ui/watcher.hpp"

#include "libao.hpp"

void contours_delete(struct contours* cs)
{
    for (uint32_t i=0; i < cs->size; ++i)
    {
        free(cs->xs[i]);
        free(cs->ys[i]);
    }
    free(cs->sizes);
    free(cs->xs);
    free(cs->ys);
    free(cs);
}
////////////////////////////////////////////////////////////////////////////////

int opcode_enum(char* op)
{
    return Opcode::from_str(op);
}

////////////////////////////////////////////////////////////////////////////////

// Construct X, Y, Z trees in affine form
Tree* tree_x()
{
    return new Tree(Tree::affine(1, 0, 0, 0));
}

Tree* tree_y()
{
    return new Tree(Tree::affine(0, 1, 0, 0));
}

Tree* tree_z()
{
    return new Tree(Tree::affine(0, 0, 1, 0));
}

Tree* tree_const(float f)
{
    return new Tree(f);
}

Tree* tree_unary(int op, Tree* a)
{
    return new Tree(Opcode::Opcode(op), *a);
}

Tree* tree_binary(int op, Tree* a, Tree* b)
{
    return new Tree(Opcode::Opcode(op), *a, *b);
}

int tree_affine_vec(Tree* t, v4* vec)
{
    bool success = false;
    glm::vec4 v = t->getAffine(&success);

    if (success)
    {
        *vec = {v.x, v.y, v.z, v.w};
        return 1;
    }
    else
    {
        return 0;
    }
}

////////////////////////////////////////////////////////////////////////////////

void tree_delete(Tree* ptr)
{
    delete ptr;
}

float tree_eval_double(Tree* tree, float x, float y, float z)
{
    auto e = Evaluator(*tree);
    return e.eval(x, y, z);
}

void tree_eval_interval(Tree* tree, v2* x, v2* y, v2* z)
{
    auto e = Evaluator(*tree);
    auto out =  e.eval({x->lower, x->upper},
                       {y->lower, y->upper},
                       {z->lower, z->upper});

    x->lower = out.lower();
    x->upper = out.upper();
}

void tree_export_heightmap(Tree* tree, char* filename,
                           float xmin, float xmax,
                           float ymin, float ymax,
                           float zmin, float zmax, float res)
{
    auto f = std::string(filename);
    assert(f.substr(f.length() - 4, 4) == ".png");

    Region region({xmin, xmax}, {ymin, ymax}, {zmin, zmax}, res);
    std::atomic_bool abort(false);

    auto img = Heightmap::Render(*tree, region, abort);

    Image::SavePng(f, img.first);
}

void tree_export_mesh(Tree* tree, char* filename,
                      float xmin, float xmax,
                      float ymin, float ymax,
                      float zmin, float zmax, float res)
{
    auto f = std::string(filename);
    assert(boost::algorithm::ends_with(f, ".stl") ||
           boost::algorithm::ends_with(f, ".obj"));

    Region region({xmin, xmax}, {ymin, ymax}, {zmin, zmax}, res);
    auto mesh = Mesh::Render(*tree, region);

    mesh.writeMeshToFile(f);
}

void tree_export_slice(Tree* tree, char* filename,
                       float xmin, float xmax, float ymin, float ymax,
                       float z, float res)
{
    auto f = std::string(filename);
    assert(f.substr(f.length() - 4, 4) == ".svg");

    Region region({xmin, xmax}, {ymin, ymax}, {z,z}, res);
    auto cs = Contours::Render(*tree, region);

    cs.writeSVG(f, region);
}

struct contours* tree_render_slice(Tree* tree,
                       float xmin, float xmax, float ymin, float ymax,
                       float z, float res)
{
    Region region({xmin, xmax}, {ymin, ymax}, {z,z}, res);
    auto cs = Contours::Render(*tree, region);

    struct contours* out = (struct contours*)malloc(sizeof(struct contours));
    out->xs = (float**)malloc(sizeof(float*) * cs.contours.size());
    out->ys = (float**)malloc(sizeof(float*) * cs.contours.size());
    out->sizes = (uint32_t*)malloc(sizeof(uint32_t*) * cs.contours.size());
    out->size = cs.contours.size();

    size_t i=0;
    for (auto c : cs.contours)
    {
        out->xs[i] = (float*)malloc(sizeof(float) * c.size());
        out->ys[i] = (float*)malloc(sizeof(float) * c.size());
        size_t j=0;
        for (auto pt : c)
        {
            out->xs[i][j]   = pt.x;
            out->ys[i][j++] = pt.y;
        }
        out->sizes[i++] = c.size();
    }
    return out;
}

int tree_render_mesh(Tree* tree, float** out,
                     float xmin, float xmax,
                     float ymin, float ymax,
                     float zmin, float zmax, float res)
{
    Region region({xmin, xmax}, {ymin, ymax}, {zmin, zmax}, res);
    auto mesh = Mesh::Render(*tree, region);

    *out = (float*)malloc(mesh.tris.size() * 3 * 3 * sizeof(float));
    size_t index = 0;
    for (auto t : mesh.tris)
    {
        for (auto i : {t[0], t[1], t[2]})
        {
            auto v = mesh.verts[i];
            *out[index++] = v.x;
            *out[index++] = v.y;
            *out[index++] = v.z;
        }
    }

    return mesh.tris.size();
}

////////////////////////////////////////////////////////////////////////////////

void cache_reset()
{
    Cache::reset();
}

////////////////////////////////////////////////////////////////////////////////

// window_callback must be set at program startup
void (*window_watch_callback)(const char*) = nullptr;

// window_thread_init is a callback that runs once in each watcher thread
// (e.g. scm_init_guile to make a thread able to call Guile callbacks)
void (*window_thread_init)() = nullptr;
std::set<std::thread::id> initialized;

static void window_watch_callback_(std::string s)
{
    assert(window_watch_callback != nullptr);

    auto id = std::this_thread::get_id();
    if (window_thread_init && initialized.count(id) == 0)
    {
        window_thread_init();
        initialized.insert(id);
    }

    window_watch_callback(s.c_str());

    // Avoid continually accumulating into a single Cache
    Cache::reset();
}

void window_show_tree(char* filename, char* name, Tree* tree)
{
    Window::instance()->addTree(filename, name, *tree);
}

void window_watch_file(char* dir, char* file)
{
    new ScriptWatcher(Window::instance(), window_watch_callback_,
                      dir, file);
}

void window_clear_frames()
{
    Window::instance()->clearFrames();
}

void window_set_callback(void (*callback)(const char*))
{
    window_watch_callback = callback;
}

void window_set_thread_init(void (*init)())
{
    window_thread_init = init;
    window_thread_init();

    auto id = std::this_thread::get_id();
    initialized.insert(id);
}

////////////////////////////////////////////////////////////////////////////////

void matrix_invert(v4* x, v4* y, v4* z)
{
    glm::mat4 M;
    M[0] = {x->x, x->y, x->z, x->w};
    M[1] = {y->x, y->y, y->z, y->w};
    M[2] = {z->x, z->y, z->z, z->w};
    M[3] = {0, 0, 0, 1};

    auto Mi = glm::inverse(M);
    *x = {Mi[0].x, Mi[0].y, Mi[0].z, Mi[0].w};
    *y = {Mi[1].x, Mi[1].y, Mi[1].z, Mi[1].w};
    *z = {Mi[2].x, Mi[2].y, Mi[2].z, Mi[2].w};
}

////////////////////////////////////////////////////////////////////////////////

static bool running = false;
void ao_run()
{
    auto window = Window::instance();
    window->draw();
    running = true;
    window->run();
}

int ao_is_running()
{
    return running;
}

void ao_halt()
{
    Window::instance()->quit();
}
