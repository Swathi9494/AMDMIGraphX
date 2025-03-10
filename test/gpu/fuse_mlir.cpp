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
#include <migraphx/dead_code_elimination.hpp>
#include <migraphx/gpu/fuse_mlir.hpp>
#include <migraphx/instruction.hpp>
#include <migraphx/pass_manager.hpp>
#include <migraphx/program.hpp>
#include <basic_ops.hpp>
#include <migraphx/make_op.hpp>

#include <test.hpp>
#include <pointwise.hpp>

MIGRAPHX_DECLARE_ENV_VAR(MIGRAPHX_ENABLE_MLIR_INPUT_FUSION);

void run_pass(migraphx::program& p)
{
    migraphx::run_passes(
        p, {migraphx::gpu::fuse_mlir{.enable_extra = true}, migraphx::dead_code_elimination{}});
}

template <class F>
migraphx::instruction_ref add_mlir(migraphx::program& p,
                                   const std::string& name,
                                   std::vector<migraphx::instruction_ref> inputs,
                                   std::vector<std::string> arg_names,
                                   F f)
{
    assert(inputs.size() == arg_names.size() && "One interior parameter name given per input.");
    auto* mm = p.get_main_module();
    auto* pm = p.create_module(name);
    pm->set_bypass();
    std::vector<migraphx::instruction_ref> params;
    for(size_t i = 0, e = inputs.size(); i < e; ++i)
    {
        params.push_back(pm->add_parameter(arg_names[i], inputs[i]->get_shape()));
    }
    auto values = f(pm, params);
    auto root   = std::get<0>(values);
    auto r      = std::get<1>(values);
    pm->add_return({r});
    return mm->add_instruction(
        migraphx::make_op("gpu::mlir_op", {{"op", migraphx::to_value(root->get_operator())}}),
        inputs,
        {pm});
}

TEST_CASE(dot_add)
{
    migraphx::shape s{migraphx::shape::float_type, {1, 3, 3}};
    migraphx::program p1;
    {
        auto* mm = p1.get_main_module();
        auto a   = mm->add_parameter("a", s);
        auto b   = mm->add_parameter("b", s);
        auto x   = mm->add_parameter("x", s);
        auto dot = mm->add_instruction(migraphx::make_op("dot"), a, b);
        auto add = add_pointwise(p1, "main:pointwise0", {dot, x}, single_pointwise("add"));
        mm->add_return({add});
    }
    run_pass(p1);
    migraphx::program p2;
    {
        auto* mm = p2.get_main_module();
        auto a   = mm->add_parameter("a", s);
        auto b   = mm->add_parameter("b", s);
        auto x   = mm->add_parameter("x", s);
        auto fused =
            add_mlir(p2,
                     "mlir_main:pointwise0",
                     {x, a, b},
                     {"x1", "y0", "y1"},
                     [=](auto* pm, const auto& inputs) {
                         auto dot =
                             pm->add_instruction(migraphx::make_op("dot"), inputs[1], inputs[2]);
                         auto add = pm->add_instruction(migraphx::make_op("add"), dot, inputs[0]);
                         return std::make_tuple(dot, add);
                     });
        mm->add_return({fused});
    }
    EXPECT(p1.sort() == p2.sort());
}

TEST_CASE(add_dot)
{
    migraphx::shape s{migraphx::shape::float_type, {1, 3, 3}};
    migraphx::program p1;
    {
        auto* mm = p1.get_main_module();
        auto b   = mm->add_parameter("b", s);
        auto x   = mm->add_parameter("x", s);
        auto y   = mm->add_parameter("y", s);
        auto add = add_pointwise(p1, "main:pointwise0", {x, y}, single_pointwise("add"));
        auto dot = mm->add_instruction(migraphx::make_op("dot"), add, b);
        mm->add_return({dot});
    }
    run_pass(p1);
    migraphx::program p2;
    {
        auto* mm = p2.get_main_module();
        auto b   = mm->add_parameter("b", s);
        auto x   = mm->add_parameter("x", s);
        auto y   = mm->add_parameter("y", s);
        auto fused =
            add_mlir(p2,
                     "main:pointwise0:mlir_dot1",
                     {x, y, b},
                     {"x0", "x1", "x2"},
                     [=](auto* pm, const auto& inputs) {
                         auto add =
                             pm->add_instruction(migraphx::make_op("add"), inputs[0], inputs[1]);
                         auto dot = pm->add_instruction(migraphx::make_op("dot"), add, inputs[2]);
                         return std::make_tuple(dot, dot);
                     });
        mm->add_return({fused});
    }
    if(not migraphx::enabled(MIGRAPHX_ENABLE_MLIR_INPUT_FUSION{}))
        return;
    EXPECT(p1.sort() == p2.sort());
}

TEST_CASE(int_quant_dot_abs)
{
    migraphx::shape s_a{migraphx::shape::int8_type, {5, 4}};
    migraphx::shape s_b{migraphx::shape::int8_type, {4, 3}};
    migraphx::program p1;
    {
        auto* mm = p1.get_main_module();
        auto a   = mm->add_parameter("a", s_a);
        auto b   = mm->add_parameter("b", s_b);
        auto dot = mm->add_instruction(migraphx::make_op("quant_dot"), a, b);
        auto abs = add_pointwise(p1, "main:pointwise0", {dot}, single_pointwise("abs"));
        mm->add_return({abs});
    }
    run_pass(p1);
    migraphx::program p2;
    {
        auto* mm   = p2.get_main_module();
        auto a     = mm->add_parameter("a", s_a);
        auto b     = mm->add_parameter("b", s_b);
        auto fused = add_mlir(
            p2, "mlir_main:pointwise0", {a, b}, {"y0", "y1"}, [=](auto* pm, const auto& inputs) {
                auto dot =
                    pm->add_instruction(migraphx::make_op("quant_dot"), inputs[0], inputs[1]);
                auto abs = pm->add_instruction(migraphx::make_op("abs"), dot);
                return std::make_tuple(dot, abs);
            });
        mm->add_return({fused});
    }
    EXPECT(p1.sort() == p2.sort());
}

TEST_CASE(int_quant_dot_tanh_fails)
{
    migraphx::shape s_a{migraphx::shape::int8_type, {5, 4}};
    migraphx::shape s_b{migraphx::shape::int8_type, {4, 3}};
    migraphx::program p1;
    {
        auto* mm  = p1.get_main_module();
        auto a    = mm->add_parameter("a", s_a);
        auto b    = mm->add_parameter("b", s_b);
        auto dot  = mm->add_instruction(migraphx::make_op("quant_dot"), a, b);
        auto tanh = add_pointwise(p1, "main:pointwise0", {dot}, single_pointwise("tanh"));
        mm->add_return({tanh});
    }
    // This pass should not fuse as int32_t tanh isn't supported.
    run_pass(p1);
    auto* mm = p1.get_main_module();
    bool has_pointwise =
        std::any_of(mm->begin(), mm->end(), [&](const auto& i) { return i.name() == "pointwise"; });
    EXPECT(has_pointwise);
}

int main(int argc, const char* argv[])
{
    if(migraphx::gpu::mlir_enabled())
    {
        test::run(argc, argv);
    }
    return 0;
}
