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

#include <onnx_test.hpp>

TEST_CASE(gemm_dyn_bias_test)
{
    migraphx::program p;
    auto* mm = p.get_main_module();
    auto x0 =
        mm->add_parameter("A", migraphx::shape{migraphx::shape::float_type, {{8, 8}, {1, 10}}});
    auto x1   = mm->add_parameter("B", migraphx::shape{migraphx::shape::float_type, {8, 7}});
    auto x2   = mm->add_parameter("C", migraphx::shape{migraphx::shape::float_type, {1, 7}});
    auto x0_t = mm->add_instruction(migraphx::make_op("transpose", {{"permutation", {1, 0}}}), x0);
    auto dot  = mm->add_instruction(migraphx::make_op("dot"), x0_t, x1);
    auto x2_b = mm->add_instruction(migraphx::make_op("multibroadcast"), x2, dot);
    auto ret  = mm->add_instruction(migraphx::make_op("add"), dot, x2_b);
    mm->add_return({ret});

    migraphx::onnx_options options;
    options.default_dyn_dim_value = {1, 10};
    auto prog                     = read_onnx("gemm_dyn_bias_test.onnx", options);
    EXPECT(p == prog);
}
