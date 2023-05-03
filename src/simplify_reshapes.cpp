/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2022 Advanced Micro Devices, Inc. All rights reserved.
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
#include <iterator>
#include <migraphx/simplify_reshapes.hpp>
#include <migraphx/program.hpp>
#include <migraphx/instruction.hpp>
#include <migraphx/op/as_shape.hpp>
#include <migraphx/op/transpose.hpp>
#include <migraphx/op/concat.hpp>
#include <migraphx/op/slice.hpp>
#include <migraphx/iterator_for.hpp>
#include <migraphx/ranges.hpp>
#include <migraphx/matcher.hpp>
#include <migraphx/permutation.hpp>
#include <migraphx/dead_code_elimination.hpp>
#include <unordered_set>
#include <migraphx/make_op.hpp>
#include <migraphx/tune_axis.hpp>
#include <migraphx/common_dims.hpp>
#include <migraphx/dom_info.hpp>

#include <map>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {

const auto& reshaper_names()
{
    // clang-format off
    static const std::unordered_set<std::string> names = {
        "flatten",
        "reshape",
        "squeeze",
        "unsqueeze"
    };
    // clang-format on
    return names;
}

bool is_reshaper(instruction_ref ins) { return contains(reshaper_names(), ins->name()); }

instruction_ref find_transpose_input(instruction_ref ins)
{
    if(ins->inputs().size() != 1)
        return ins;
    if(ins->inputs().front()->name() == "contiguous")
        return find_transpose_input(ins->inputs().front());
    if(ins->inputs().front()->name() == "transpose")
        return ins->inputs().front();
    return ins;
}

auto get_transpose_dims(instruction_ref ins)
{
    return any_cast<const op::transpose&>(ins->get_operator()).dims;
}

bool is_no_transpose(const std::vector<int64_t>& dims)
{
    if(dims.empty())
        return true;
    if(dims.front() != 0)
        return false;
    return std::adjacent_find(
               dims.begin(), dims.end(), [](auto x, auto y) { return (y - x) != 1; }) == dims.end();
}

struct find_reshaper
{
    auto matcher() const
    {
        auto no_output_reshape = match::none_of[match::outputs()](match::name(reshaper_names()));
        auto input_reshape =
            match::arg(0)(match::skip(match::name("contiguous"))(match::name(reshaper_names())));
        auto input = match::skip(match::name(reshaper_names()),
                                 match::name("contiguous"))(match::arg(0).bind("x"));
        return match::name(reshaper_names())(no_output_reshape, input_reshape, input);
    }

    void apply(module& m, const match::matcher_result& mr) const
    {
        auto ins   = mr.result;
        auto input = mr.instructions["x"];
        auto dims  = ins->get_shape().lens();

        if(not input->get_shape().standard())
            input = m.insert_instruction(ins, make_op("contiguous"), input);
        m.replace_instruction(ins, make_op("reshape", {{"dims", dims}}), input);
    }
};

struct find_nop_reshapes
{
    auto matcher() const
    {
        auto reshapes = reshaper_names();
        reshapes.insert("as_shape");
        reshapes.insert("broadcast");
        reshapes.insert("concat");
        reshapes.insert("convert");
        reshapes.insert("multibroadcast");
        reshapes.insert("pad");
        reshapes.insert("slice");
        reshapes.insert("transpose");
        return match::name(reshapes)(match::same_shape(match::arg(0)));
    }

    void apply(module& m, const match::matcher_result& mr) const
    {
        auto ins = mr.result;
        m.replace_instruction(ins, ins->inputs().front());
    }
};

struct find_transpose
{
    auto matcher() const
    {
        auto output_not_transpose =
            match::none_of(match::skip_output(match::name("contiguous"))(match::name("transpose")));
        auto input_has_transpose =
            match::args(match::skip(match::name("contiguous"))(match::name("transpose")));
        return match::name("transpose")(output_not_transpose, input_has_transpose);
    }

    void apply(module& m, const match::matcher_result& mr) const
    {
        auto ins = mr.result;
        auto x   = ins;
        auto t   = ins;
        std::vector<std::int64_t> dims(ins->get_shape().lens().size());
        std::iota(dims.begin(), dims.end(), 0);
        do
        {
            dims = reorder_dims(get_transpose_dims(t), dims);
            x    = t;
            t    = find_transpose_input(x);
        } while(x != t and t->name() == "transpose");
        if(t == ins or t->name() != "transpose")
            return;
        if(is_no_transpose(dims))
        {
            m.replace_instruction(ins, t->inputs().front());
        }
        else
        {
            m.replace_instruction(
                ins, make_op("transpose", {{"permutation", dims}}), t->inputs().front());
        }
    }
};

struct find_nested_convert
{
    auto matcher() const { return match::name("convert")(match::arg(0)(match::name("convert"))); }

    void apply(module& m, const match::matcher_result& mr) const
    {
        auto ins   = mr.result;
        auto x     = ins->inputs().front();
        auto input = x->inputs().front();

        if(ins->get_shape() != input->get_shape())
            return;

        m.replace_instruction(ins, input);
    }
};

struct find_nested_slice
{
    auto matcher() const { return match::name("slice")(match::arg(0)(match::name("slice"))); }

    using axes_map = std::map<std::size_t, std::pair<std::size_t, std::size_t>>;

    static axes_map get_axes(instruction_ref ins)
    {
        axes_map result;
        auto op = any_cast<op::slice>(ins->get_operator());
        for(std::size_t i = 0; i < op.axes.size(); i++)
        {
            result[op.axes[i]] = std::make_pair(op.starts[i], op.ends[i]);
        }
        return result;
    }

    static axes_map merge(const axes_map& m1, const axes_map& m2)
    {
        axes_map result;
        // Non overlapping
        for(auto&& p : m1)
        {
            if(contains(m2, p.first))
                continue;
            result[p.first] = p.second;
        }
        for(auto&& p : m2)
        {
            if(contains(m1, p.first))
                continue;
            result[p.first] = p.second;
        }
        // Overlapping
        for(auto&& p1 : m1)
        {
            if(not contains(m2, p1.first))
                continue;
            auto&& v1        = p1.second;
            auto&& v2        = m2.at(p1.first);
            auto start       = v1.first + v2.first;
            auto end         = start + (v2.second - v2.first);
            result[p1.first] = std::make_pair(start, end);
        }
        return result;
    }

    void apply(module& m, const match::matcher_result& mr) const
    {
        auto ins   = mr.result;
        auto slice = ins->inputs().front();
        auto input = slice->inputs().front();

        auto a1 = get_axes(ins);
        auto a2 = get_axes(slice);

        auto axes = merge(a2, a1);

        auto op = op::slice{};
        for(auto&& pp : axes)
        {
            op.axes.push_back(pp.first);
            op.starts.push_back(pp.second.first);
            op.ends.push_back(pp.second.second);
        }
        m.replace_instruction(ins, op, input);
    }
};

struct find_concat_multibroadcasts
{
    auto matcher() const
    {
        return match::name("concat")(match::all_of[match::inputs()](match::name("multibroadcast")));
    }

    void apply(module& m, const match::matcher_result& mr) const
    {
        auto ins        = mr.result;
        auto op         = any_cast<op::concat>(ins->get_operator());
        auto out_lens   = ins->get_shape().lens();
        auto inputs     = ins->inputs();
        auto in_strides = inputs.front()->get_shape().strides();

        // Only apply when concat axis is not a broadcasted dimension
        if(std::any_of(inputs.begin(), inputs.end(), [&](auto i) {
               return i->get_shape().strides()[op.axis] == 0;
           }))
        {
            return;
        }

        // Use inputs of multibroadcast ops as inputs to new concat op
        std::transform(inputs.begin(), inputs.end(), inputs.begin(), [](auto i) {
            return i->inputs().front();
        });

        // Reduce axis by number of leading broadcasted dimensions
        if(inputs.front()->get_shape().lens().size() < out_lens.size())
            op.axis -= std::count(in_strides.begin(), in_strides.begin() + op.axis, 0);

        auto concat = m.insert_instruction(ins, op, inputs);
        m.replace_instruction(
            ins, migraphx::make_op("multibroadcast", {{"out_lens", out_lens}}), concat);
    }
};

struct find_concat_transpose
{
    auto matcher() const
    {
        return match::name("concat")(match::all_of[match::inputs()](match::name("transpose")));
    }

    void apply(module& m, const match::matcher_result& mr) const
    {
        auto ins          = mr.result;
        auto trans_inputs = ins->inputs();
        auto s            = trans_inputs.front()->get_shape();
        assert(s.transposed());
        auto op          = any_cast<op::concat>(ins->get_operator());
        auto permutation = find_permutation(s);

        // permutation should be the same for all inputs
        if(not std::all_of(trans_inputs.begin(), trans_inputs.end(), [&](auto in) {
               return (find_permutation(in->get_shape()) == permutation);
           }))
        {
            return;
        }

        // axis could be a negative value
        int64_t n_dim = static_cast<int64_t>(s.lens().size());
        op.axis       = tune_axis(n_dim, op.axis, op.name());

        auto ipermutation = invert_permutation(permutation);
        op.axis           = ipermutation[op.axis];

        std::vector<instruction_ref> inputs;
        std::transform(
            ins->inputs().begin(), ins->inputs().end(), std::back_inserter(inputs), [&](auto i) {
                return m.insert_instruction(
                    ins, make_op("transpose", {{"permutation", permutation}}), i);
            });
        auto concat = m.insert_instruction(ins, op, inputs);
        auto t      = m.insert_instruction(
            ins, make_op("transpose", {{"permutation", ipermutation}}), concat);
        assert(ins->get_shape().lens() == t->get_shape().lens());
        m.replace_instruction(ins, t);
    }
};

struct find_nested_concat
{
    auto matcher() const
    {
        return match::name("concat")(match::any_of[match::inputs()](match::name("concat")));
    }

    static std::size_t get_axis(instruction_ref ins)
    {
        auto op = any_cast<op::concat>(ins->get_operator());
        return op.axis;
    }

    void apply(module& m, const match::matcher_result& mr) const
    {
        auto ins  = mr.result;
        auto axis = get_axis(ins);
        std::vector<instruction_ref> args;
        fix([&](auto self, auto&& inputs) {
            for(auto&& i : inputs)
            {
                if(i->name() == "concat" and get_axis(i) == axis and i->outputs().size() == 1)
                    self(i->inputs());
                else
                    args.push_back(i);
            }
        })(ins->inputs());
        m.replace_instruction(ins, ins->get_operator(), args);
    }
};

struct find_resize
{
    auto matcher() const
    {
        return match::name("gather")(
            match::args(match::name("reshape").bind("data"), match::is_constant().bind("ind")));
    }

    void apply(module& m, const match::matcher_result& r) const
    {
        auto ins     = r.result;
        auto ins_rsp = r.instructions["data"];
        auto ins_ind = r.instructions["ind"];

        // resize input shape
        if(ins_rsp->get_shape().lens().size() != 1)
        {
            return;
        }

        // resize output shape
        const auto& in_shape  = ins_rsp->inputs().front()->get_shape();
        const auto& out_shape = ins->get_shape();
        // check if output shape is multiple of input shape
        const auto& in_lens  = in_shape.lens();
        const auto& out_lens = out_shape.lens();
        if(in_lens.size() != out_lens.size())
        {
            return;
        }

        // output shape must be multiple of input shape
        std::vector<bool> is_multi(in_lens.size());
        std::transform(
            in_lens.begin(), in_lens.end(), out_lens.begin(), is_multi.begin(), [](auto x, auto y) {
                return (y % x == 0);
            });
        if(not std::all_of(is_multi.begin(), is_multi.end(), [](auto b) { return b; }))
        {
            return;
        }

        // output must be multiple of inputs
        std::vector<std::size_t> scales(in_lens.size());
        std::transform(
            in_lens.begin(), in_lens.end(), out_lens.begin(), scales.begin(), [](auto x, auto y) {
                return y / x;
            });

        // if ind is not constant, cannot optimize
        std::vector<int> vec_ind;
        auto arg_ind = ins_ind->eval();
        if(arg_ind.empty())
        {
            return;
        }
        arg_ind.visit([&](auto v) { vec_ind.assign(v.begin(), v.end()); });
        if(not all_of(range(out_shape.elements()), [&](auto i) {
               auto out_idx = out_shape.multi(i);
               auto in_idx  = out_idx;
               std::transform(out_idx.begin(),
                              out_idx.end(),
                              scales.begin(),
                              in_idx.begin(),
                              [&](auto io, auto scale) { return io - (io % scale); });
               return vec_ind[i] == vec_ind[out_shape.index(in_idx)];
           }))
        {
            return;
        }

        // wrap up shapes for multibroadcast
        std::vector<std::pair<std::size_t, std::size_t>> dim_scales;
        std::transform(in_lens.begin(),
                       in_lens.end(),
                       out_lens.begin(),
                       std::back_inserter(dim_scales),
                       [](auto x, auto y) { return std::make_pair(x, y / x); });

        std::vector<int64_t> in_dims;
        std::vector<int64_t> out_dims;
        for(auto& isp : dim_scales)
        {
            in_dims.push_back(isp.first);
            out_dims.push_back(isp.first * isp.second);
            if(isp.first == 1 or isp.second == 1)
            {
                continue;
            }

            out_dims.back() = isp.first;
            in_dims.push_back(1);
            out_dims.push_back(isp.second);
        }

        auto in_rsp   = ins_rsp->inputs().front();
        auto rsp_data = m.insert_instruction(
            ins_rsp, migraphx::make_op("reshape", {{"dims", in_dims}}), in_rsp);
        auto mb_rsp = m.insert_instruction(
            ins_rsp, migraphx::make_op("multibroadcast", {{"out_lens", out_dims}}), rsp_data);
        auto std_mb = m.insert_instruction(ins, migraphx::make_op("contiguous"), mb_rsp);
        std::vector<int64_t> rsp_dims(out_lens.begin(), out_lens.end());
        m.replace_instruction(ins, migraphx::make_op("reshape", {{"dims", rsp_dims}}), std_mb);
    }
};

struct find_where_op
{
    auto matcher() const
    {
        return match::name("gather")(
            match::args(match::name("reshape")(match::arg(0)(match::name("concat").bind("data"))),
                        match::is_constant().bind("ind")));
    }

    void apply(module& m, const match::matcher_result& r) const
    {
        auto ins     = r.result;
        auto concat  = r.instructions["data"];
        auto ins_ind = r.instructions["ind"];
        std::vector<bool> vec_ind;
        auto arg_ind = ins_ind->eval();
        arg_ind.visit([&](auto v) { vec_ind.assign(v.begin(), v.end()); });
        // ind has to be the same value
        auto val = vec_ind.front();
        if(not std::all_of(vec_ind.begin(), vec_ind.end(), [&](auto v) { return (v == val); }))
        {
            return;
        }

        // concat axis must be 0
        auto op = any_cast<op::concat>(concat->get_operator());
        if(op.axis != 0)
        {
            return;
        }

        // check concat inputs, it has to be 2 and have the same shape
        const auto& inputs = concat->inputs();
        if(inputs.size() != 2)
        {
            return;
        }
        if(inputs.at(0)->get_shape() != inputs.at(1)->get_shape())
        {
            return;
        }
        if(inputs.at(0)->get_shape().lens() != ins_ind->get_shape().lens())
        {
            return;
        }

        if(val)
        {
            m.replace_instruction(ins, inputs.at(0));
        }
        else
        {
            m.replace_instruction(ins, inputs.at(1));
        }
    }
};

struct find_reshape_cont
{
    auto matcher() const
    {
        return match::pointwise(
            match::nargs(2),
            match::either_arg(0, 1)(
                match::name("reshape")(match::args(match::name("contiguous").bind("cont")))
                    .bind("rsp"),
                match::any()));
    }

    void apply(module& m, const match::matcher_result& r) const
    {
        auto ins      = r.result;
        auto ins_cont = r.instructions["cont"];
        auto in_ins   = r.instructions["rsp"];

        auto cont_input = ins_cont->inputs().front();
        auto lens       = cont_input->get_shape().lens();
        std::vector<int64_t> dims(lens.begin(), lens.end());

        if(in_ins->get_shape() != ins->get_shape())
        {
            return;
        }

        if(not std::all_of(ins->inputs().begin(), ins->inputs().end(), [](auto i) {
               return i->get_shape().standard();
           }))
        {
            return;
        }

        auto out_lens = ins->get_shape().lens();
        std::vector<int64_t> out_dims(out_lens.begin(), out_lens.end());
        std::vector<instruction_ref> inputs;
        for(const auto& in : ins->inputs())
        {
            if(in == in_ins)
            {
                inputs.push_back(cont_input);
            }
            else
            {
                inputs.push_back(
                    m.insert_instruction(ins, make_op("reshape", {{"dims", dims}}), in));
            }
        }
        auto out = m.insert_instruction(ins, ins->get_operator(), inputs);
        m.replace_instruction(ins, make_op("reshape", {{"dims", out_dims}}), out);
    }
};

// match sequence of transpose --> contiguous --> reshaper_op
template <class... Ms>
auto match_transpose_contiguous_reshaper(Ms... ms)
{
    return match::name({"reshape", "squeeze", "unsqueeze"})(
               match::used_once(),
               match::args(match::name("contiguous")(
                               match::used_once(),
                               match::args(match::transpose_shape(ms...).bind("trans_ins")))
                               .bind("cont_ins")))
        .bind("reshaper_ins");
};

// finds the pattern of transpose --> contiguous --> reshaper_op --> unary
// application of this matcher moves the unary operation before the contiguous so it becomes
// transpose --> unary --> contiguous --> reshaper_op. later pointwise sub-module can be created out
// of unary --> contiguous --> reshaper_op. Such pattern appears in depthToSpace or spaceToDepth
// operator.
struct find_transpose_contiguous_reshaper_unary
{
    auto matcher() const
    {
        return pointwise(match::used_once(),
                         match::nargs(1),
                         match::args(match_transpose_contiguous_reshaper()));
    }

    void apply(module& m, const match::matcher_result& r) const
    {
        auto ins           = r.result;
        auto reshaper_ins  = r.instructions["reshaper_ins"];
        auto trans_ins     = r.instructions["trans_ins"];
        auto cont_ins      = r.instructions["cont_ins"];
        auto unary_op_name = ins->get_operator().name();
        auto unary_ins     = m.insert_instruction(cont_ins, make_op(unary_op_name), trans_ins);
        auto new_cont_ins  = m.insert_instruction(cont_ins, make_op("contiguous"), unary_ins);
        // older cont and reshape are removed by deadcode elimination
        m.replace_instruction(ins, reshaper_ins->get_operator(), new_cont_ins);
    }
};

struct find_mul_add_transpose_contiguous_reshaper_gemm
{
    auto matcher() const
    {
        auto pw = match::name("mul", "add")(
            match::used_once(),
            match::either_arg(0, 1)(match::is_constant().bind("c"), match::any().bind("x")));
        return match::name("dot")(match::either_arg(0, 1)(
            match_transpose_contiguous_reshaper(match::args(pw.bind("pointwise"))),
            match::is_constant()));
    }

    void apply(module& m, const match::matcher_result& r) const
    {
        auto ins             = r.result;
        auto reshaper_ins    = r.instructions["reshaper_ins"];
        auto trans_ins       = r.instructions["trans_ins"];
        auto x_ins           = r.instructions["x"];
        auto c_ins           = r.instructions["c"];
        auto pw_ins          = r.instructions["pointwise"];
        auto insert_reshapes = [&](auto x) {
            auto t = m.insert_instruction(ins, trans_ins->get_operator(), x);
            auto c = m.insert_instruction(ins, make_op("contiguous"), t);
            return m.insert_instruction(ins, reshaper_ins->get_operator(), c);
        };
        if(x_ins->name() == "mul")
        {
            x_ins = m.insert_instruction(
                ins,
                make_op("mul"),
                {insert_reshapes(x_ins->inputs()[0]), insert_reshapes(x_ins->inputs()[1])});
        }

        auto y_ins =
            m.insert_instruction(ins, pw_ins->get_operator(), {x_ins, insert_reshapes(c_ins)});
        m.replace_instruction(reshaper_ins, y_ins);
    }
};

struct find_slice_transpose
{
    auto matcher() const
    {
        return match::any(match::any_of[match::outputs()](
            match::name("slice")(match::output(match::name("transpose")))));
    }

    static std::vector<int64_t> find_common_perm(const std::vector<instruction_ref>& transposes)
    {
        std::map<std::vector<int64_t>, int64_t> count;
        for(auto t : transposes)
        {
            auto perm = t->get_operator().to_value()["permutation"].to_vector<int64_t>();
            count[perm]++;
        }
        return std::max_element(
                   count.begin(), count.end(), by(std::less<>{}, [](auto&& p) { return p.second; }))
            ->first;
    }

    void apply(module& m, const match::matcher_result& r) const
    {
        auto ins = r.result;
        std::vector<instruction_ref> splits;
        std::copy_if(ins->outputs().begin(),
                     ins->outputs().end(),
                     std::back_inserter(splits),
                     [&](instruction_ref out) {
                         return out->name() == "slice" and out->outputs().size() == 1 and
                                out->outputs().front()->name() == "transpose";
                     });
        if(splits.size() < 2)
            return;
        std::vector<instruction_ref> transposes;
        std::transform(splits.begin(),
                       splits.end(),
                       std::back_inserter(transposes),
                       [](auto split) { return split->outputs().front(); });
        auto perm  = find_common_perm(transposes);
        auto iperm = invert_permutation(perm);
        auto pre   = m.insert_instruction(
            std::next(ins), make_op("transpose", {{"permutation", perm}}), ins);
        for(auto i : range(transposes.size()))
        {
            auto split = splits[i];
            auto t     = transposes[i];
            auto op    = any_cast<op::slice>(split->get_operator());
            std::transform(op.axes.begin(), op.axes.end(), op.axes.begin(), [&](auto axis) {
                return iperm[axis];
            });
            auto new_ins = m.insert_instruction(t, op, pre);
            if(t->get_operator() != pre->get_operator())
            {
                auto curr = t->get_operator().to_value()["permutation"].to_vector<int64_t>();
                new_ins   = m.insert_instruction(
                    t, make_op("transpose", {{"permutation", reorder_dims(iperm, curr)}}), new_ins);
            }
            m.replace_instruction(t, new_ins);
        }
    }
};

struct find_transpose_slice
{
    auto matcher() const
    {
        return match::name("transpose")(match::all_of[match::outputs()](match::name("slice")));
    }

    static std::vector<int64_t> slice_distance(const op::slice& op)
    {
        assert(op.starts.size() == op.ends.size());
        std::vector<int64_t> result(op.starts.size());
        std::transform(
            op.ends.begin(), op.ends.end(), op.starts.begin(), result.begin(), std::minus<>{});
        return result;
    }

    void apply(module& m, const match::matcher_result& r) const
    {
        auto ins    = r.result;
        auto slices = ins->outputs();
        if(slices.empty())
            return;
        auto slice     = any_cast<op::slice>(slices.front()->get_operator());
        auto sdistance = slice_distance(slice);
        // Check all distances and axes are the same
        if(std::any_of(slices.begin(), slices.end(), [&](auto sins) {
               auto s = any_cast<op::slice>(sins->get_operator());
               return s.axes != slice.axes or slice_distance(s) != sdistance;
           }))
            return;
        // Check distances are divisible by lens of corresponding axes
        auto mod_by_distance = [&](const auto& v, auto f) {
            return std::inner_product(v.begin(),
                                      v.end(),
                                      sdistance.begin(),
                                      0,
                                      std::plus<>{},
                                      [&](auto x, auto d) -> uint64_t {
                                          if(d == 0)
                                              return 1;
                                          return f(x) % d;
                                      });
        };
        if(mod_by_distance(slice.axes, [&](auto x) { return ins->get_shape().lens()[x]; }) != 0 or
           mod_by_distance(slice.starts, id{}) != 0 or mod_by_distance(slice.ends, id{}) != 0)
            return;
        // TODO: Handle multiple axes
        if(sdistance.size() != 1)
            return;
        auto axis = slice.axes.front();
        // Skip if axis would be packed
        if(std::all_of(ins->get_shape().lens().begin(),
                       ins->get_shape().lens().begin() + axis,
                       [](auto x) { return x == 1; }))
            return;
        // Compute axis before transpose to use for unsqueeze
        auto perm    = ins->get_operator().to_value()["permutation"].to_vector<int64_t>();
        auto preaxis = perm[axis];
        // Make unsqueeze
        std::vector<int64_t> steps(sdistance.size());
        std::transform(
            slice.axes.begin(),
            slice.axes.end(),
            sdistance.begin(),
            steps.begin(),
            [&](const auto ax, const auto sdis) { return ins->get_shape().lens().at(ax) / sdis; });
        auto unsqueeze = m.insert_instruction(
            ins, make_op("unsqueeze", {{"axes", {preaxis}}, {"steps", steps}}), ins->inputs());
        // Make transpose
        std::transform(perm.begin(), perm.end(), perm.begin(), [&](auto i) {
            if(i >= preaxis)
                return i + 1;
            return i;
        });
        perm.insert(perm.begin(), preaxis);
        auto transpose =
            m.insert_instruction(ins, make_op("transpose", {{"permutation", perm}}), unsqueeze);
        // Slice and squeeze
        for(auto s : slices)
        {
            auto op        = any_cast<op::slice>(s->get_operator());
            op.axes        = {0};
            op.starts      = {op.starts.front() / sdistance.front()};
            op.ends        = {op.ends.front() / sdistance.front()};
            auto slice_ins = m.insert_instruction(ins, op, transpose);
            auto squeeze =
                m.insert_instruction(ins, make_op("squeeze", {{"axes", {0}}}), slice_ins);
            m.replace_instruction(s, squeeze);
        }
    }
};

struct find_reshape_gemm
{
    auto matcher() const { return match::name("reshape")(match::arg(0)(match::name("dot"))); }

    static bool is_batched_unsqueeze(instruction_ref ins)
    {
        auto input  = ins->inputs().front()->get_shape().lens();
        auto output = ins->get_shape().lens();
        if(output.size() <= input.size())
            return false;
        if(not std::equal(input.end() - 2, input.end(), output.end() - 2, output.end()))
            return false;
        return true;
    }

    static operation make_reshape(std::vector<std::size_t> batches, instruction_ref ins)
    {
        batches.insert(
            batches.end(), ins->get_shape().lens().end() - 2, ins->get_shape().lens().end());
        return make_op("reshape", {{"dims", batches}});
    }

    void apply(module& m, const match::matcher_result& r) const
    {
        auto reshape_ins = r.result;
        auto dot_ins     = reshape_ins->inputs().front();

        // TODO: Put this in the matcher
        if(not is_batched_unsqueeze(reshape_ins))
            return;

        std::vector<std::size_t> batches;
        std::copy(reshape_ins->get_shape().lens().begin(),
                  reshape_ins->get_shape().lens().end() - 2,
                  std::back_inserter(batches));

        auto input0 = m.insert_instruction(
            dot_ins, make_reshape(batches, dot_ins->inputs()[0]), dot_ins->inputs()[0]);
        auto input1 = m.insert_instruction(
            dot_ins, make_reshape(batches, dot_ins->inputs()[1]), dot_ins->inputs()[1]);
        m.replace_instruction(dot_ins, make_op("dot"), input0, input1);
    }
};

struct find_broadcast_reshaper
{
    auto matcher() const
    {
        auto broadcast =
            match::broadcast_shape(match::skip(match::broadcast_shape())(match::any().bind("x")));
        return match::name(reshaper_names())(
            match::args(match::skip(match::name("contiguous"))(broadcast.bind("broadcast"))));
    }

    void apply(module& m, const match::matcher_result& r) const
    {
        auto ins           = r.result;
        auto broadcast_ins = r.instructions["broadcast"];
        auto x_ins         = r.instructions["x"];

        auto broadcast_shape = broadcast_ins->get_shape();
        auto result_shape    = ins->get_shape();

        if(std::accumulate(broadcast_shape.strides().begin(), broadcast_shape.strides().end(), 0) !=
           1)
            return;

        auto baxis =
            std::find(broadcast_shape.strides().begin(), broadcast_shape.strides().end(), 1) -
            broadcast_shape.strides().begin();
        auto relements = result_shape.lens();
        std::partial_sum(
            relements.begin(), relements.end(), relements.begin(), std::multiplies<>{});
        auto prefix_elements = std::accumulate(broadcast_shape.lens().begin(),
                                               broadcast_shape.lens().begin() + baxis + 1,
                                               1,
                                               std::multiplies<>{});
        auto axis =
            std::find(relements.begin(), relements.end(), prefix_elements) - relements.begin();
        if(axis >= relements.size())
            return;

        if(x_ins->get_shape().lens().size() > 1)
            x_ins = m.insert_instruction(ins, make_op("squeeze"), x_ins);

        m.replace_instruction(
            ins,
            make_op("broadcast", {{"axis", axis}, {"out_lens", ins->get_shape().lens()}}),
            x_ins);
    }
};

struct find_poinwise_reduce_reshape
{
    template<class... Ms>
    static auto match_reshaper(Ms... ms)
    {
        return match::name({"reshape", "squeeze", "unsqueeze"})(match::arg(0)(match::skip(match::name("contiguous"))(ms...)));
    }
    auto matcher() const
    {
        auto pointwise_or_reduce = match::any_of(match::pointwise(), match::reduce());
        auto reshape_pointwise_or_reduce =
            match_reshaper(match::pointwise().bind("x")).bind("reshape");
        return pointwise_or_reduce(match::any_of[match::inputs()](reshape_pointwise_or_reduce));
    }

    static bool is_broadcast(const operation& op)
    {
        return contains({"broadcast", "multibroadcast"}, op.name());
    }

    static bool is_broadcast(instruction_ref ins)
    {
        return is_broadcast(ins->get_operator());
    }

    static bool is_pointwise(instruction_ref ins)
    {
        auto a = ins->get_operator().attributes();
        return a.get("pointwise", false);
    }

    static bool is_reduce(instruction_ref ins)
    {
        return is_reduce(ins->get_operator());
    }

    static bool is_reduce(const operation& op)
    {
        auto a = op.attributes();
        return a.get("reduce", false);
    }

    static bool is_pointwise_or_reduce(instruction_ref ins)
    {
        auto a = ins->get_operator().attributes();
        return a.get("pointwise", false) or a.get("reduce", false);
    }

    static std::vector<instruction_ref> topo_sort(instruction_ref entry, const std::unordered_set<instruction_ref>& inss, std::unordered_set<instruction_ref>& aux)
    {
        std::vector<instruction_ref> instructions;
        bool has_entry = contains(inss, entry);
        fix([&](auto self, instruction_ref ins) {
            if (ins != entry or has_entry)
                instructions.push_back(ins);
            for(auto input:ins->inputs())
            {
                if(not contains(inss, input))
                    aux.insert(input);
            }
            for(auto output : ins->outputs())
            {
                if (contains(instructions, output))
                    continue;
                if (not contains(inss, output))
                    continue;
                self(output);
            }
        })(entry);
        assert(instructions.size() == inss.size());
        return instructions;
    }

    static std::vector<instruction_ref> topo_sort(const std::unordered_set<instruction_ref>& inss, std::unordered_set<instruction_ref>& aux)
    {
        std::vector<instruction_ref> instructions;
        std::unordered_set<instruction_ref> visited;
        for(auto ins:inss)
        {
            fix([&](auto self, instruction_ref child) {
                if (contains(visited, child))
                    return;
                for(auto output:child->outputs())
                {
                    if (not contains(inss, output))
                        continue;
                    self(output);
                }
                visited.insert(child);
                for(auto input:child->inputs())
                {
                    if(not contains(inss, input))
                        aux.insert(input);
                }
                instructions.push_back(child);
            })(ins);
        }
        std::reverse(instructions.begin(), instructions.end());
        assert(instructions.size() == inss.size());
        return instructions;
    }

    void apply(module& m, const match::matcher_result& r) const
    {
        // std::cout << "find_poinwise_reduce_reshape" << std::endl;
        auto ins         = r.result;
        auto x_ins       = r.instructions["x"];
        auto reshape_ins = r.instructions["reshape"];

        auto nelements = x_ins->get_shape().elements();
        auto dims1 = x_ins->get_shape().lens();
        auto dims2 = reshape_ins->get_shape().lens();

        auto cd = common_dims::compute(dims1, dims2);
        if (cd.empty())
            return;

        // m.debug_print();
        // m.debug_print(reshape_ins);
        // m.debug_print(ins);
        // Collect from inputs
        std::unordered_set<instruction_ref> input_inss;
        instruction_ref entry;
        fix([&](auto self, instruction_ref i) {
            if(contains(input_inss, i))
                return;
            input_inss.insert(i);
            entry                    = i;
            auto pointwise_or_reduce = [](instruction_ref input) {
                if(input->can_eval())
                    return false;
                return is_pointwise(input);
            };
            auto it = std::find_if(i->inputs().begin(), i->inputs().end(), pointwise_or_reduce);
            if(it == i->inputs().end())
                return;
            auto it2 = std::find_if(it, i->inputs().end(), pointwise_or_reduce);
            // If there is more than one pointwise_reduce than stop
            if(it2 != i->inputs().end())
                return;
            self(*it);
        })(x_ins);

        std::vector<int64_t> axes;
        auto dom = compute_post_dominator(m);
        std::unordered_set<instruction_ref> output_inss;
        // Collect from output
        fix([&](auto self, instruction_ref out) {
            // if(contains(inss, out))
                // return;
            // std::cout << "Visit: ";
            // m.debug_print(out);
            // m.debug_print(out->inputs());
            auto outputs = out->outputs();
            std::sort(outputs.begin(), outputs.end(), by(std::less<>{}, [&](instruction_ref i) {
                return std::distance(reshape_ins, i);
            }));
            // m.debug_print(outputs);
            for(auto output : outputs)
            {
                if(not std::all_of(
                       output->inputs().begin(), output->inputs().end(), [&](auto input) {
                           return input->can_eval() or reshape_ins == input or contains(output_inss, input);// or dom.strictly_dominate(reshape_ins, input);
                       }))
                    continue;
                if(not is_pointwise_or_reduce(output) and not is_broadcast(output))
                    continue;
                if (is_reduce(output))
                {
                    auto op_axes = output->get_operator().to_value()["axes"].to_vector<int64_t>();
                    if (axes.empty())
                        axes = op_axes;
                    if(axes != op_axes)
                        return;
                }
                output_inss.insert(output);
                self(output);
            }
        })(reshape_ins);

        std::vector<int64_t> common_axes;
        for(auto axis:axes)
        {
            common_axes.insert(common_axes.end(), cd.axes_map2[axis].begin(), cd.axes_map2[axis].end());
        }
        auto common_rdims = cd.dims;
        for(auto axis:common_axes)
        {
            common_rdims[axis] = 1;
        }
        // Topological sort
        std::unordered_set<instruction_ref> aux;
        auto input_instructions = topo_sort(input_inss, aux);
        auto output_instructions = topo_sort(output_inss, aux);
        // std::cout << "output_inss:\n";
        // m.debug_print({output_inss.begin(), output_inss.end()});
        // std::cout << "Output instructions:\n";
        // m.debug_print(output_instructions);
        // std::cout << "aux:\n";
        // m.debug_print({aux.begin(), aux.end()});

        auto last = output_instructions.back();
        auto insert_reshape = [&](instruction_ref input) {
            auto use_rdims = input->get_shape().elements() < nelements;
            auto c = m.insert_instruction(last, make_op("contiguous"), input);
            return m.insert_instruction(last, make_op("reshape", {{"dims", use_rdims ? common_rdims : cd.dims}}), c);
        };

        std::unordered_map<instruction_ref, instruction_ref> map_ins;
        // map_ins[entry] = insert_reshape(entry);
        for(auto i:aux)
        {
            map_ins[i] = insert_reshape(i);
        }
        auto inserter = [&](module& mm, instruction_ref i, operation op, const std::vector<instruction_ref>& args, const std::vector<module_ref>& module_args) {
            if (is_reduce(op))
                op.from_value({{"axes", common_axes}});
            if (is_broadcast(op))
                op.from_value({{"out_lens", cd.dims}});
            // std::cout << op << std::endl;
            // m.debug_print(args);
            return mm.insert_instruction(i, op, args, module_args);
        };
        auto new_x_ins = m.insert_instructions(inserter, last, input_instructions, map_ins).front();
        map_ins[reshape_ins] = new_x_ins;
        auto new_last = m.insert_instructions(inserter, last, output_instructions, map_ins).front();
        auto new_c = m.insert_instruction(last, make_op("contiguous"), new_last);
        auto new_reshape = m.insert_instruction(last, make_op("reshape", {{"dims", dims2}}), new_c);
        m.debug_print();
        m.debug_print(last);
        m.debug_print(new_reshape);
        m.replace_instruction(last, new_reshape);
        std::abort();
    }
};

void simplify_reshapes::apply(module& m) const
{
    for(int i = 0; i < 4; i++)
    {
        match::find_matches(m,
                            find_where_op{},
                            find_resize{},
                            find_nop_reshapes{},
                            find_reshaper{},
                            // find_broadcast_reshaper{},
                            // find_reshape_cont{},
                            find_transpose{},
                            find_concat_transpose{},
                            find_concat_multibroadcasts{},
                            find_nested_convert{},
                            find_nested_slice{},
                            find_nested_concat{},
                            find_transpose_slice{},
                            find_slice_transpose{},
                            find_transpose_contiguous_reshaper_unary{},
                            find_mul_add_transpose_contiguous_reshaper_gemm{},
                            find_reshape_gemm{},
                            find_poinwise_reduce_reshape{});
        dead_code_elimination{}.apply(m);
    }
}

} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
