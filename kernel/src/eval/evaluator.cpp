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
#include <numeric>
#include <memory>
#include <cmath>

#include <glm/gtc/matrix_inverse.hpp>

#include "ao/kernel/tree/cache.hpp"
#include "ao/kernel/tree/tree.hpp"
#include "ao/kernel/eval/evaluator.hpp"
#include "ao/kernel/eval/clause.hpp"

////////////////////////////////////////////////////////////////////////////////

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 5
// http://stackoverflow.com/questions/27064791/stdalign-not-supported-by-g4-9
namespace std {
    inline void *align( std::size_t alignment, std::size_t size,
                        void *&ptr, std::size_t &space ) {
        std::uintptr_t pn = reinterpret_cast< std::uintptr_t >( ptr );
        std::uintptr_t aligned = ( pn + alignment - 1 ) & - alignment;
        std::size_t padding = aligned - pn;
        if ( space < size + padding ) return nullptr;
        space -= padding;
        return ptr = reinterpret_cast< void * >( aligned );
    }
}
#endif

Evaluator::Evaluator(const Tree root_, const glm::mat4& M)
    : M(M), Mi(glm::inverse(M))
{
    auto root = root_.collapse();
    Cache* cache = root.parent.get();
    auto connected = cache->findConnected(root.id);

    // Reserve space for X, Y, Z, plus every clause in the cache
    size_t count = 3 + connected.size();

    // Then, allocate space for them (ensuring alignment if AVX is used)
    Clause* ptr;
#if __AVX__
    {   // Ensure that we have 32-byte alignment for Clauses and Results
        size_t alignment = 32;
        size_t bytes = sizeof(Clause) * count + alignment;
        void* buf = malloc(bytes);
        data = static_cast<Clause*>(buf);
        ptr = static_cast<Clause*>(
                std::align(alignment, sizeof(Clause) * count, buf, bytes));
    }
#else
    data = static_cast<Clause*>(malloc(sizeof(Clause) * count));
    ptr = data;
#endif

    // Helper function to create a new clause in the data array
    std::unordered_map<Cache::Id, Clause*> clauses = {{0, nullptr}};
    auto newClause = [&ptr, cache, &clauses](const Cache::Id t)
        { auto c = new (ptr++) Clause(
                cache->opcode(t), cache->value(t),
                clauses.at(cache->lhs(t)), clauses.at(cache->rhs(t)));
          clauses[t] = c;
          return c; };

    // Make X, Y, Z clauses and set their derivatives (which never change)
    X = newClause(cache->X());
    Y = newClause(cache->Y());
    Z = newClause(cache->Z());

    X->result.deriv(1, 0, 0);
    Y->result.deriv(0, 1, 0);
    Z->result.deriv(0, 0, 1);

    // Load constants into the array first
    //
    // CONST is guaranteed to be at the beginning of the cache
    // (by rank and enumerator ordering), so once we get to a non-CONST
    // value we can break out of the loop.
    for (auto m : cache->data.left)
    {
        if (connected.count(m.second))
        {
            if (m.first.opcode() == Opcode::CONST)
            {
                newClause(m.second);
            }
            else if (m.first.rank() > 0) // Ignore VAR_XYZ
            {
                while (m.first.rank() > rows.size())
                {
                    rows.push_back(Row());
                }
                rows[m.first.rank() - 1].push_back(newClause(m.second));
            }
        }
    }

    for (auto& row : rows)
    {
        row.setSize();
    }

    assert(clauses[root.id]);
    this->root = clauses[root.id];
}

Evaluator::~Evaluator()
{
    free(data);
}

////////////////////////////////////////////////////////////////////////////////

float Evaluator::eval(float x, float y, float z)
{
    set(x, y, z, 0);
    return values(1)[0];
}

Interval Evaluator::eval(Interval x, Interval y, Interval z)
{
    set(x, y, z);
    return interval();
}

void Evaluator::set(Interval x, Interval y, Interval z)
{
    X->result.set(M[0][0] * x + M[1][0] * y + M[2][0] * z + M[3][0]);
    Y->result.set(M[0][1] * x + M[1][1] * y + M[2][1] * z + M[3][1]);
    Z->result.set(M[0][2] * x + M[1][2] * y + M[2][2] * z + M[3][2]);
}

////////////////////////////////////////////////////////////////////////////////

void Evaluator::push()
{
    // Walk up the tree, marking every atom with ATOM_FLAG_IGNORED
    for (const auto& row : rows)
    {
        for (size_t i=0; i < row.active; ++i)
        {
            row[i]->disable();
        }
    }

    // Clear the IGNORED flag on the root
    root->enable();

    // Walk down the tree, clearing IGNORED flags as appropriate
    // and disabling atoms that still have IGNORED flags set.
    for (auto itr = rows.rbegin(); itr != rows.rend(); ++itr)
    {
        itr->push();
    }
}

void Evaluator::pop()
{
    for (auto& row : rows)
    {
        row.pop();
    }
}

////////////////////////////////////////////////////////////////////////////////

#define EVAL_LOOP for (size_t i=0; i < count; ++i)
static void clause(Opcode::Opcode op,
        const float* __restrict a, const float* __restrict b,
        float* __restrict out, size_t count)
{
    switch (op) {
        case Opcode::ADD:
            EVAL_LOOP
            out[i] = a[i] + b[i];
            break;
        case Opcode::MUL:
            EVAL_LOOP
            out[i] = a[i] * b[i];
            break;
        case Opcode::MIN:
            EVAL_LOOP
            out[i] = fmin(a[i], b[i]);
            break;
        case Opcode::MAX:
            EVAL_LOOP
            out[i] = fmax(a[i], b[i]);
            break;
        case Opcode::SUB:
            EVAL_LOOP
            out[i] = a[i] - b[i];
            break;
        case Opcode::DIV:
            EVAL_LOOP
            out[i] = a[i] / b[i];
            break;
        case Opcode::ATAN2:
            EVAL_LOOP
            out[i] = atan2(a[i], b[i]);
            break;
        case Opcode::POW:
            EVAL_LOOP
            out[i] = pow(a[i], b[i]);
            break;
        case Opcode::NTH_ROOT:
            EVAL_LOOP
            out[i] = pow(a[i], 1.0f/b[i]);
            break;
        case Opcode::MOD:
            EVAL_LOOP
            {
                out[i] = std::fmod(a[i], b[i]);
                while (out[i] < 0)
                {
                    out[i] += b[i];
                }
            }
            break;
        case Opcode::NANFILL:
            EVAL_LOOP
            out[i] = std::isnan(a[i]) ? b[i] : a[i];
            break;

        case Opcode::SQUARE:
            EVAL_LOOP
            out[i] = a[i] * a[i];
            break;
        case Opcode::SQRT:
            EVAL_LOOP
            out[i] = sqrt(a[i]);
            break;
        case Opcode::NEG:
            EVAL_LOOP
            out[i] = -a[i];
            break;
        case Opcode::ABS:
            EVAL_LOOP
            out[i] = fabs(a[i]);
            break;
        case Opcode::SIN:
            EVAL_LOOP
            out[i] = sin(a[i]);
            break;
        case Opcode::COS:
            EVAL_LOOP
            out[i] = cos(a[i]);
            break;
        case Opcode::TAN:
            EVAL_LOOP
            out[i] = tan(a[i]);
            break;
        case Opcode::ASIN:
            EVAL_LOOP
            out[i] = asin(a[i]);
            break;
        case Opcode::ACOS:
            EVAL_LOOP
            out[i] = acos(a[i]);
            break;
        case Opcode::ATAN:
            EVAL_LOOP
            out[i] = atan(a[i]);
            break;
        case Opcode::EXP:
            EVAL_LOOP
            out[i] = exp(a[i]);
            break;

        case Opcode::DUMMY_A:
            EVAL_LOOP
            out[i] = a[i];
            break;
        case Opcode::DUMMY_B:
            EVAL_LOOP
            out[i] = b[i];
            break;

        case Opcode::INVALID:
        case Opcode::CONST:
        case Opcode::VAR_X:
        case Opcode::VAR_Y:
        case Opcode::VAR_Z:
        case Opcode::AFFINE_VEC:
        case Opcode::LAST_OP: assert(false);
    }
}

static void clause(Opcode::Opcode op,
        const float* __restrict av,  const float* __restrict adx,
        const float* __restrict ady, const float* __restrict adz,

        const float* __restrict bv,  const float* __restrict bdx,
        const float* __restrict bdy, const float* __restrict bdz,

        float* __restrict ov,  float* __restrict odx,
        float* __restrict ody, float* __restrict odz,
        size_t count)
{
    // Evaluate the base operations in a single pass
    clause(op, av, bv, ov, count);

    switch (op) {
        case Opcode::ADD:
            EVAL_LOOP
            {
                odx[i] = adx[i] + bdx[i];
                ody[i] = ady[i] + bdy[i];
                odz[i] = adz[i] + bdz[i];
            }
            break;
        case Opcode::MUL:
            EVAL_LOOP
            {   // Product rule
                odx[i] = av[i]*bdx[i] + adx[i]*bv[i];
                ody[i] = av[i]*bdy[i] + ady[i]*bv[i];
                odz[i] = av[i]*bdz[i] + adz[i]*bv[i];
            }
            break;
        case Opcode::MIN:
            EVAL_LOOP
            {
                if (av[i] < bv[i])
                {
                    odx[i] = adx[i];
                    ody[i] = ady[i];
                    odz[i] = adz[i];
                }
                else
                {
                    odx[i] = bdx[i];
                    ody[i] = bdy[i];
                    odz[i] = bdz[i];
                }
            }
            break;
        case Opcode::MAX:
            EVAL_LOOP
            {
                if (av[i] < bv[i])
                {
                    odx[i] = bdx[i];
                    ody[i] = bdy[i];
                    odz[i] = bdz[i];
                }
                else
                {
                    odx[i] = adx[i];
                    ody[i] = ady[i];
                    odz[i] = adz[i];
                }
            }
            break;
        case Opcode::SUB:
            EVAL_LOOP
            {
                odx[i] = adx[i] - bdx[i];
                ody[i] = ady[i] - bdy[i];
                odz[i] = adz[i] - bdz[i];
            }
            break;
        case Opcode::DIV:
            EVAL_LOOP
            {
                const float p = pow(bv[i], 2);
                odx[i] = (bv[i]*adx[i] - av[i]*bdx[i]) / p;
                ody[i] = (bv[i]*ady[i] - av[i]*bdy[i]) / p;
                odz[i] = (bv[i]*adz[i] - av[i]*bdz[i]) / p;
            }
            break;
        case Opcode::ATAN2:
            EVAL_LOOP
            {
                const float d = pow(av[i], 2) + pow(bv[i], 2);
                odx[i] = (adx[i]*bv[i] - av[i]*bdx[i]) / d;
                ody[i] = (ady[i]*bv[i] - av[i]*bdy[i]) / d;
                odz[i] = (adz[i]*bv[i] - av[i]*bdz[i]) / d;
            }
            break;
        case Opcode::POW:
            EVAL_LOOP
            {
                const float m = pow(av[i], bv[i] - 1);

                // The full form of the derivative is
                // odx[i] = m * (bv[i] * adx[i] + av[i] * log(av[i]) * bdx[i]))
                // However, log(av[i]) is often NaN and bdx[i] is always zero,
                // (since it must be CONST), so we skip that part.
                odx[i] = m * (bv[i] * adx[i]);
                ody[i] = m * (bv[i] * ady[i]);
                odz[i] = m * (bv[i] * adz[i]);
            }
            break;
        case Opcode::NTH_ROOT:
            EVAL_LOOP
            {
                const float m = pow(av[i], 1.0f/bv[i] - 1);
                odx[i] = m * (1.0f/bv[i] * adx[i]);
                ody[i] = m * (1.0f/bv[i] * ady[i]);
                odz[i] = m * (1.0f/bv[i] * adz[i]);
            }
            break;
        case Opcode::MOD:
            EVAL_LOOP
            {
                // This isn't quite how partial derivatives of mod work,
                // but close enough normals rendering.
                odx[i] = adx[i];
                ody[i] = ady[i];
                odz[i] = adz[i];
            }
            break;
        case Opcode::NANFILL:
            EVAL_LOOP
            {
                odx[i] = std::isnan(av[i]) ? bdx[i] : adx[i];
                ody[i] = std::isnan(av[i]) ? bdy[i] : ady[i];
                odz[i] = std::isnan(av[i]) ? bdz[i] : adz[i];
            }
            break;

        case Opcode::SQUARE:
            EVAL_LOOP
            {
                odx[i] = 2 * av[i] * adx[i];
                ody[i] = 2 * av[i] * ady[i];
                odz[i] = 2 * av[i] * adz[i];
            }
            break;
        case Opcode::SQRT:
            EVAL_LOOP
            {
                if (av[i] < 0)
                {
                    odx[i] = 0;
                    ody[i] = 0;
                    odz[i] = 0;
                }
                else
                {
                    odx[i] = adx[i] / (2 * ov[i]);
                    ody[i] = ady[i] / (2 * ov[i]);
                    odz[i] = adz[i] / (2 * ov[i]);
                }
            }
            break;
        case Opcode::NEG:
            EVAL_LOOP
            {
                odx[i] = -adx[i];
                ody[i] = -ady[i];
                odz[i] = -adz[i];
            }
            break;
        case Opcode::ABS:
            EVAL_LOOP
            {
                if (av[i] < 0)
                {
                    odx[i] = -adx[i];
                    ody[i] = -ady[i];
                    odz[i] = -adz[i];
                }
                else
                {
                    odx[i] = adx[i];
                    ody[i] = ady[i];
                    odz[i] = adz[i];
                }
            }
            break;
        case Opcode::SIN:
            EVAL_LOOP
            {
                const float c = cos(av[i]);
                odx[i] = adx[i] * c;
                ody[i] = ady[i] * c;
                odz[i] = adz[i] * c;
            }
            break;
        case Opcode::COS:
            EVAL_LOOP
            {
                const float s = -sin(av[i]);
                odx[i] = adx[i] * s;
                ody[i] = ady[i] * s;
                odz[i] = adz[i] * s;
            }
            break;
        case Opcode::TAN:
            EVAL_LOOP
            {
                const float s = pow(1/cos(av[i]), 2);
                odx[i] = adx[i] * s;
                ody[i] = ady[i] * s;
                odz[i] = adz[i] * s;
            }
            break;
        case Opcode::ASIN:
            EVAL_LOOP
            {
                const float d = sqrt(1 - pow(av[i], 2));
                odx[i] = adx[i] / d;
                ody[i] = ady[i] / d;
                odz[i] = adz[i] / d;
            }
            break;
        case Opcode::ACOS:
            EVAL_LOOP
            {
                const float d = -sqrt(1 - pow(av[i], 2));
                odx[i] = adx[i] / d;
                ody[i] = ady[i] / d;
                odz[i] = adz[i] / d;
            }
            break;
        case Opcode::ATAN:
            EVAL_LOOP
            {
                const float d = pow(av[i], 2) + 1;
                odx[i] = adx[i] / d;
                ody[i] = ady[i] / d;
                odz[i] = adz[i] / d;
            }
            break;
        case Opcode::EXP:
            EVAL_LOOP
            {
                const float e = exp(av[i]);
                odx[i] = e * adx[i];
                ody[i] = e * ady[i];
                odz[i] = e * adz[i];
            }
            break;

        case Opcode::DUMMY_A:
            EVAL_LOOP
            {
                odx[i] = adx[i];
                ody[i] = ady[i];
                odz[i] = adz[i];
            }
            break;
        case Opcode::DUMMY_B:
            EVAL_LOOP
            {
                odx[i] = bdx[i];
                ody[i] = bdy[i];
                odz[i] = bdz[i];
            }
            break;
        case Opcode::INVALID:
        case Opcode::CONST:
        case Opcode::VAR_X:
        case Opcode::VAR_Y:
        case Opcode::VAR_Z:
        case Opcode::AFFINE_VEC:
        case Opcode::LAST_OP: assert(false);
    }
}

#ifdef __AVX__
static void clause(Opcode::Opcode op,
        const __m256* __restrict a, const __m256* __restrict b,
              __m256* __restrict out, size_t count)
{
    switch (op) {
        case Opcode::ADD:
            EVAL_LOOP
            out[i] = _mm256_add_ps(a[i], b[i]);
            break;
        case Opcode::MUL:
            EVAL_LOOP
            out[i] = _mm256_mul_ps(a[i], b[i]);
            break;
        case Opcode::MIN:
            EVAL_LOOP
            out[i] = _mm256_min_ps(a[i], b[i]);
            break;
        case Opcode::MAX:
            EVAL_LOOP
            out[i] = _mm256_max_ps(a[i], b[i]);
            break;
        case Opcode::SUB:
            EVAL_LOOP
            out[i] = _mm256_sub_ps(a[i], b[i]);
            break;
        case Opcode::DIV:
            EVAL_LOOP
            out[i] = _mm256_div_ps(a[i], b[i]);
            break;

        case Opcode::SQUARE:
            EVAL_LOOP
            out[i] = _mm256_mul_ps(a[i], a[i]);
            break;
        case Opcode::SQRT:
            EVAL_LOOP
            out[i] = _mm256_sqrt_ps(a[i]);
            break;
        case Opcode::NEG:
            EVAL_LOOP
            out[i] = _mm256_sub_ps(_mm256_setzero_ps(), a[i]);
            break;
        case Opcode::ABS:
            EVAL_LOOP
            out[i] = _mm256_andnot_ps(a[i], _mm256_set1_ps(-0.0f));
            break;

        case Opcode::DUMMY_A:
            EVAL_LOOP
            out[i] = a[i];
            break;
        case Opcode::DUMMY_B:
            EVAL_LOOP
            out[i] = b[i];
            break;

        // Trig functions don't have AVX equivalents, so fall back to
        // default clause evaluation
        case Opcode::ATAN2:
        case Opcode::SIN:
        case Opcode::COS:
        case Opcode::TAN:
        case Opcode::ASIN:
        case Opcode::ACOS:
        case Opcode::ATAN:
        case Opcode::EXP:
        case Opcode::POW:
        case Opcode::NTH_ROOT:
        case Opcode::MOD:
        case Opcode::NANFILL:
            clause(op, reinterpret_cast<const float*>(a),
                       reinterpret_cast<const float*>(b),
                       reinterpret_cast<float*>(out), count*8);
            break;

        case Opcode::INVALID:
        case Opcode::CONST:
        case Opcode::VAR_X:
        case Opcode::VAR_Y:
        case Opcode::VAR_Z:
        case Opcode::AFFINE_VEC:
        case Opcode::LAST_OP: assert(false);
    }
}

//  We'll use this comparison operator, which is
//      less than
//      ordered (which defines how it handles NaNs)
//      quiet (meaning it doesn't signal on NaN)
#define CMP_LT_OQ 17

static void clause(Opcode::Opcode op,
        const __m256* __restrict av,  const __m256* __restrict adx,
        const __m256* __restrict ady, const __m256* __restrict adz,

        const __m256* __restrict bv,  const __m256* __restrict bdx,
        const __m256* __restrict bdy, const __m256* __restrict bdz,

        __m256* __restrict ov,  __m256* __restrict odx,
        __m256* __restrict ody, __m256* __restrict odz,
        size_t count)
{
    // Evaluate the base operations in a single pass
    clause(op, av, bv, ov, count);

    switch (op) {
        case Opcode::ADD:
            EVAL_LOOP
            {
                odx[i] = _mm256_add_ps(adx[i], bdx[i]);
                ody[i] = _mm256_add_ps(ady[i], bdy[i]);
                odz[i] = _mm256_add_ps(adz[i], bdz[i]);
            }
            break;
        case Opcode::MUL:
            EVAL_LOOP
            {   // Product rule
                odx[i] = _mm256_add_ps(_mm256_mul_ps(av[i], bdx[i]),
                                       _mm256_mul_ps(adx[i], bv[i]));
                ody[i] = _mm256_add_ps(_mm256_mul_ps(av[i], bdy[i]),
                                       _mm256_mul_ps(ady[i], bv[i]));
                odz[i] = _mm256_add_ps(_mm256_mul_ps(av[i], bdz[i]),
                                       _mm256_mul_ps(adz[i], bv[i]));
            }
            break;
        case Opcode::MIN:
            EVAL_LOOP
            {
                __m256 cmp = _mm256_cmp_ps(av[i], bv[i], CMP_LT_OQ);
                odx[i] = _mm256_blendv_ps(bdx[i], adx[i], cmp);
                ody[i] = _mm256_blendv_ps(bdy[i], ady[i], cmp);
                odz[i] = _mm256_blendv_ps(bdz[i], adz[i], cmp);
            }
            break;
        case Opcode::MAX:
            EVAL_LOOP
            {
                __m256 cmp = _mm256_cmp_ps(av[i], bv[i], CMP_LT_OQ);
                odx[i] = _mm256_blendv_ps(adx[i], bdx[i], cmp);
                ody[i] = _mm256_blendv_ps(ady[i], bdy[i], cmp);
                odz[i] = _mm256_blendv_ps(adz[i], bdz[i], cmp);
            }
            break;
        case Opcode::SUB:
            EVAL_LOOP
            {
                odx[i] = _mm256_sub_ps(adx[i], bdx[i]);
                ody[i] = _mm256_sub_ps(ady[i], bdy[i]);
                odz[i] = _mm256_sub_ps(adz[i], bdz[i]);
            }
            break;
        case Opcode::DIV:
            EVAL_LOOP
            {
                const __m256 p = _mm256_mul_ps(bv[i], bv[i]);
                odx[i] = _mm256_div_ps(
                          _mm256_sub_ps(_mm256_mul_ps(bv[i], adx[i]),
                                        _mm256_mul_ps(av[i], bdx[i])), p);
                ody[i] = _mm256_div_ps(
                          _mm256_sub_ps(_mm256_mul_ps(bv[i], ady[i]),
                                        _mm256_mul_ps(av[i], bdy[i])), p);
                odz[i] = _mm256_div_ps(
                          _mm256_sub_ps(_mm256_mul_ps(bv[i], adz[i]),
                                        _mm256_mul_ps(av[i], bdz[i])), p);
            }
            break;
        case Opcode::SQUARE:
            EVAL_LOOP
            {
                odx[i] = _mm256_mul_ps(_mm256_set1_ps(2),
                                       _mm256_mul_ps(av[i], adx[i]));
                ody[i] = _mm256_mul_ps(_mm256_set1_ps(2),
                                       _mm256_mul_ps(av[i], ady[i]));
                odz[i] = _mm256_mul_ps(_mm256_set1_ps(2),
                                       _mm256_mul_ps(av[i], adz[i]));
            }
            break;
        case Opcode::SQRT:
            EVAL_LOOP
            {
                __m256 cmp = _mm256_cmp_ps(av[i], _mm256_setzero_ps(), CMP_LT_OQ);

                // Calculate the common denominator
                __m256 den = _mm256_mul_ps(ov[i], _mm256_set1_ps(2));

                // If the value is less than zero, clamp the derivative at zero
                odx[i] = _mm256_blendv_ps(
                        _mm256_div_ps(adx[i], den), _mm256_setzero_ps(), cmp);
                ody[i] = _mm256_blendv_ps(
                        _mm256_div_ps(ady[i], den), _mm256_setzero_ps(), cmp);
                odz[i] = _mm256_blendv_ps(
                        _mm256_div_ps(adz[i], den), _mm256_setzero_ps(), cmp);
            }
            break;
        case Opcode::NEG:
            EVAL_LOOP
            {
                odx[i] = _mm256_sub_ps(_mm256_setzero_ps(), adx[i]);
                ody[i] = _mm256_sub_ps(_mm256_setzero_ps(), ady[i]);
                odz[i] = _mm256_sub_ps(_mm256_setzero_ps(), adz[i]);
            }
            break;
        case Opcode::ABS:
            EVAL_LOOP
            {
                __m256 cmp = _mm256_cmp_ps(av[i], _mm256_setzero_ps(), CMP_LT_OQ);

                // If a value is less than zero, negate its derivative
                odx[i] = _mm256_blendv_ps(
                        adx[i], _mm256_sub_ps(_mm256_setzero_ps(), adx[i]), cmp);
                ody[i] = _mm256_blendv_ps(
                        ady[i], _mm256_sub_ps(_mm256_setzero_ps(), ady[i]), cmp);
                odz[i] = _mm256_blendv_ps(
                        adz[i], _mm256_sub_ps(_mm256_setzero_ps(), adz[i]), cmp);
            }
            break;
        case Opcode::DUMMY_A:
            EVAL_LOOP
            {
                odx[i] = adx[i];
                ody[i] = ady[i];
                odz[i] = adz[i];
            }
            break;
        case Opcode::DUMMY_B:
            EVAL_LOOP
            {
                odx[i] = bdx[i];
                ody[i] = bdy[i];
                odz[i] = bdz[i];
            }
            break;

        // Trig functions don't have AVX equivalents, so fall back to
        // default clause evaluation
        case Opcode::ATAN2:
        case Opcode::SIN:
        case Opcode::COS:
        case Opcode::TAN:
        case Opcode::ASIN:
        case Opcode::ACOS:
        case Opcode::ATAN:
        case Opcode::EXP:
        case Opcode::POW:
        case Opcode::NTH_ROOT:
        case Opcode::MOD:
        case Opcode::NANFILL:
            clause(op, reinterpret_cast<const float*>(av),
                       reinterpret_cast<const float*>(adx),
                       reinterpret_cast<const float*>(ady),
                       reinterpret_cast<const float*>(adz),

                       reinterpret_cast<const float*>(bv),
                       reinterpret_cast<const float*>(bdx),
                       reinterpret_cast<const float*>(bdy),
                       reinterpret_cast<const float*>(bdz),

                       reinterpret_cast<float*>(ov),
                       reinterpret_cast<float*>(odx),
                       reinterpret_cast<float*>(ody),
                       reinterpret_cast<float*>(odz), count*8);
            break;

        case Opcode::INVALID:
        case Opcode::CONST:
        case Opcode::VAR_X:
        case Opcode::VAR_Y:
        case Opcode::VAR_Z:
        case Opcode::AFFINE_VEC:
        case Opcode::LAST_OP: assert(false);
    }
}
#endif

static Interval clause(Opcode::Opcode op, const Interval& a, const Interval& b)
{
    switch (op) {
        case Opcode::ADD:
            return a + b;
        case Opcode::MUL:
            return a * b;
        case Opcode::MIN:
            return boost::numeric::min(a, b);
        case Opcode::MAX:
            return boost::numeric::max(a, b);
        case Opcode::SUB:
            return a - b;
        case Opcode::DIV:
            return a / b;
        case Opcode::ATAN2:
            return atan2(a, b);
        case Opcode::POW:
            return boost::numeric::pow(a, b.lower());
        case Opcode::NTH_ROOT:
            return boost::numeric::nth_root(a, b.lower());
        case Opcode::MOD:
            return Interval(0.0f, b.upper()); // YOLO
        case Opcode::NANFILL:
            return (std::isnan(a.lower()) || std::isnan(a.upper())) ? b : a;

        case Opcode::SQUARE:
            return boost::numeric::square(a);
        case Opcode::SQRT:
            return boost::numeric::sqrt(a);
        case Opcode::NEG:
            return -a;
        case Opcode::ABS:
            return boost::numeric::abs(a);
        case Opcode::SIN:
            return boost::numeric::sin(a);
        case Opcode::COS:
            return boost::numeric::cos(a);
        case Opcode::TAN:
            return boost::numeric::tan(a);
        case Opcode::ASIN:
            return boost::numeric::asin(a);
        case Opcode::ACOS:
            return boost::numeric::acos(a);
        case Opcode::ATAN:
            return boost::numeric::atan(a);
        case Opcode::EXP:
            return boost::numeric::exp(a);

        case Opcode::DUMMY_A:
            return a;
        case Opcode::DUMMY_B:
            return b;
        case Opcode::INVALID:
        case Opcode::CONST:
        case Opcode::VAR_X:
        case Opcode::VAR_Y:
        case Opcode::VAR_Z:
        case Opcode::AFFINE_VEC:
        case Opcode::LAST_OP: assert(false);
    }
    return Interval();
}

////////////////////////////////////////////////////////////////////////////////

#ifdef __AVX__
const float* Evaluator::values(size_t count, bool vectorize)
{
    if (vectorize)
    {
        count = (count - 1)/8 + 1;

        for (const auto& row : rows)
        {
            for (size_t i=0; i < row.active; ++i)
            {
                auto op = getOpcode(row[i]);
                clause(op, row[i]->ptrs.a.mf, row[i]->ptrs.b.mf,
                           row[i]->result.mf, count);
            }
        }

        return root->result.f;
    }
#else
const float* Evaluator::values(size_t count)
{
#endif
    for (const auto& row : rows)
    {
        for (size_t i=0; i < row.active; ++i)
        {
            auto op = getOpcode(row[i]);
            clause(op, row[i]->ptrs.a.f, row[i]->ptrs.b.f,
                       row[i]->result.f, count);
        }
    }
    return root->result.f;
}

#ifdef __AVX__
std::tuple<const float*, const float*,
           const float*, const float*> Evaluator::derivs(size_t count,
                                                         bool vectorize)
{
    if (vectorize)
    {
        size_t vc = (count - 1)/8 + 1;

        for (const auto& row : rows)
        {
            for (size_t i=0; i < row.active; ++i)
            {
                auto op = getOpcode(row[i]);

                clause(op, row[i]->ptrs.a.mf, row[i]->ptrs.a.mdx,
                           row[i]->ptrs.a.mdy, row[i]->ptrs.a.mdz,

                           row[i]->ptrs.b.mf, row[i]->ptrs.b.mdx,
                           row[i]->ptrs.b.mdy, row[i]->ptrs.b.mdz,

                           row[i]->result.mf, row[i]->result.mdx,
                           row[i]->result.mdy, row[i]->result.mdz,
                       vc);
            }
        }
    } else
#else
std::tuple<const float*, const float*,
           const float*, const float*> Evaluator::derivs(size_t count)
{
#endif
    for (const auto& row : rows)
    {
        for (size_t i=0; i < row.active; ++i)
        {
            auto op = getOpcode(row[i]);

            clause(op, row[i]->ptrs.a.f, row[i]->ptrs.a.dx,
                       row[i]->ptrs.a.dy, row[i]->ptrs.a.dz,

                       row[i]->ptrs.b.f, row[i]->ptrs.b.dx,
                       row[i]->ptrs.b.dy, row[i]->ptrs.b.dz,

                       row[i]->result.f, row[i]->result.dx,
                       row[i]->result.dy, row[i]->result.dz,
                   count);
        }
    }

    // Apply the inverse matrix transform to our normals
    auto o = Mi * glm::vec4(0,0,0,1);
    for (size_t i=0; i < count; ++i)
    {
        auto n = Mi * glm::vec4(root->result.dx[i],
                                root->result.dy[i],
                                root->result.dz[i], 1) - o;
        root->result.dx[i] = n.x;
        root->result.dy[i] = n.y;
        root->result.dz[i] = n.z;
    }

    return std::tuple<const float*, const float*,
                      const float*, const float*> {
                        root->result.f, root->result.dx,
                        root->result.dy, root->result.dz
                      };
}

Interval Evaluator::interval()
{
    for (const auto& row : rows)
    {
        for (size_t i=0; i < row.active; ++i)
        {
            auto op = getOpcode(row[i]);

            Interval a = row[i]->a ? row[i]->a->result.i : Interval();
            Interval b = row[i]->b ? row[i]->b->result.i : Interval();

            row[i]->result.i = clause(op, a, b);
        }
    }
    return root->result.i;
}

////////////////////////////////////////////////////////////////////////////////

void Evaluator::applyTransform(size_t count)
{
#if __AVX__
#define MM256_LOADDUP(a) _mm256_set_ps(a,a,a,a,a,a,a,a)
    __m256 M00 = MM256_LOADDUP(M[0][0]);
    __m256 M10 = MM256_LOADDUP(M[1][0]);
    __m256 M20 = MM256_LOADDUP(M[2][0]);
    __m256 M30 = MM256_LOADDUP(M[3][0]);

    __m256 M01 = MM256_LOADDUP(M[0][1]);
    __m256 M11 = MM256_LOADDUP(M[1][1]);
    __m256 M21 = MM256_LOADDUP(M[2][1]);
    __m256 M31 = MM256_LOADDUP(M[3][1]);

    __m256 M02 = MM256_LOADDUP(M[0][2]);
    __m256 M12 = MM256_LOADDUP(M[1][2]);
    __m256 M22 = MM256_LOADDUP(M[2][2]);
    __m256 M32 = MM256_LOADDUP(M[3][2]);

    for (size_t i=0; i < (count + 7) / 8; ++i)
    {
        __m256 x = X->result.mf[i];
        __m256 y = Y->result.mf[i];
        __m256 z = Z->result.mf[i];
        X->result.mf[i] = _mm256_add_ps(
            _mm256_add_ps(
                _mm256_mul_ps(x, M00), _mm256_mul_ps(y, M10)),
            _mm256_add_ps(_mm256_mul_ps(z, M20), M30));
        Y->result.mf[i] = _mm256_add_ps(
            _mm256_add_ps(
                _mm256_mul_ps(x, M01), _mm256_mul_ps(y, M11)),
            _mm256_add_ps(_mm256_mul_ps(z, M21), M31));
        Z->result.mf[i] = _mm256_add_ps(
            _mm256_add_ps(
                _mm256_mul_ps(x, M02), _mm256_mul_ps(y, M12)),
            _mm256_add_ps(_mm256_mul_ps(z, M22), M32));
    }

#undef MM256_LOADDUP
#else
    for (size_t i=0; i < count; ++i)
    {
        float x = X->result.f[i];
        float y = Y->result.f[i];
        float z = Z->result.f[i];
        X->result.f[i] = M[0][0] * x + M[1][0] * y + M[2][0] * z + M[3][0];
        Y->result.f[i] = M[0][1] * x + M[1][1] * y + M[2][1] * z + M[3][1];
        Z->result.f[i] = M[0][2] * x + M[1][2] * y + M[2][2] * z + M[3][2];
    }
#endif
}

////////////////////////////////////////////////////////////////////////////////

double Evaluator::utilization() const
{
    double total = 0;
    double active = 0;

    for (const auto& r : rows)
    {
        total += r.size();
        active += r.active;
    }

    return active / total;
}

Opcode::Opcode Evaluator::getOpcode(Clause* c)
{
    // Modify the opcode if parts of the tree are disabled
    if (c->a && c->a->disabled)
    {
        return Opcode::DUMMY_B;
    }
    else if (c->b && c->b->disabled)
    {
        return Opcode::DUMMY_A;
    }
    else
    {
        return c->op;
    }
}
