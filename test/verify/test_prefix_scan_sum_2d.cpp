/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2024 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "verify_program.hpp"
#include <migraphx/program.hpp>
#include <migraphx/shape.hpp>
#include <migraphx/generate.hpp>
#include <migraphx/make_op.hpp>

template <migraphx::shape::type_t DType>
struct test_prefix_scan_sum_2d_small : verify_program<test_prefix_scan_sum_2d_small<DType>>
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto* mm = p.get_main_module();
        migraphx::shape s{DType, {1}};
        auto x = mm->add_parameter("x", s);
        auto xb =
            mm->add_instruction(migraphx::make_op("multibroadcast", {{"out_lens", {3, 3}}}), x);
        mm->add_instruction(
            migraphx::make_op("prefix_scan_sum", {{"axis", 1}, {"exclusive", false}}), xb);
        return p;
    }
};

template struct test_prefix_scan_sum_2d_small<migraphx::shape::float_type>;
template struct test_prefix_scan_sum_2d_small<migraphx::shape::half_type>;
template struct test_prefix_scan_sum_2d_small<migraphx::shape::fp8e4m3fnuz_type>;

template <migraphx::shape::type_t DType>
struct test_prefix_scan_sum_2d_large : verify_program<test_prefix_scan_sum_2d_large<DType>>
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto* mm = p.get_main_module();
        migraphx::shape s{DType, {3, 1000}};
        auto x = mm->add_parameter("x", s);
        mm->add_instruction(
            migraphx::make_op("prefix_scan_sum", {{"axis", 1}, {"exclusive", false}}), x);
        return p;
    }
};

template struct test_prefix_scan_sum_2d_large<migraphx::shape::float_type>;
template struct test_prefix_scan_sum_2d_large<migraphx::shape::half_type>;
template struct test_prefix_scan_sum_2d_large<migraphx::shape::fp8e4m3fnuz_type>;
