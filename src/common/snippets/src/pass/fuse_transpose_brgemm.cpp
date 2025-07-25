// Copyright (C) 2018-2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "snippets/pass/fuse_transpose_brgemm.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "openvino/core/except.hpp"
#include "openvino/core/node.hpp"
#include "openvino/core/node_output.hpp"
#include "openvino/core/type.hpp"
#include "openvino/op/constant.hpp"
#include "openvino/op/transpose.hpp"
#include "openvino/opsets/opset1.hpp"
#include "openvino/pass/pattern/matcher.hpp"
#include "openvino/pass/pattern/op/label.hpp"
#include "openvino/pass/pattern/op/or.hpp"
#include "openvino/pass/pattern/op/wrap_type.hpp"
#include "snippets/itt.hpp"
#include "snippets/lowered/port_descriptor.hpp"
#include "snippets/op/brgemm.hpp"
#include "snippets/utils/utils.hpp"

namespace ov::snippets::pass {

bool FuseTransposeBrgemm::is_supported_transpose(const Output<Node>& transpose_out) {
    const auto transpose = ov::as_type_ptr<const ov::opset1::Transpose>(transpose_out.get_node_shared_ptr());
    if (!transpose) {
        return false;
    }
    const auto order = ov::as_type_ptr<const ov::opset1::Constant>(transpose->get_input_node_shared_ptr(1));
    if (!order) {
        return false;
    }
    return is_supported_transpose_order(order->cast_vector<int32_t>());
}

bool FuseTransposeBrgemm::is_supported_transpose_order(const std::vector<int32_t>& order) {
    const auto size = order.size();
    return !order.empty() && order.back() == (static_cast<int32_t>(size) - 1);
}

FuseTransposeBrgemm::FuseTransposeBrgemm() {
    MATCHER_SCOPE(FuseTransposeBrgemm);
    auto m_constant = ov::pass::pattern::wrap_type<ov::op::v0::Constant>();
    auto m_transpose = ov::pass::pattern::wrap_type<ov::op::v1::Transpose>({ov::pass::pattern::any_input(), m_constant},
                                                                           is_supported_transpose);

    // Pattern 0: Transpose on 0-th input of MatMul
    auto m_brgemm_in0 = ov::pass::pattern::wrap_type<op::Brgemm>({m_transpose, ov::pass::pattern::any_input()});

    // Pattern 1: Transpose on 1-st input of MatMul
    auto m_brgemm_in1 = ov::pass::pattern::wrap_type<op::Brgemm>({ov::pass::pattern::any_input(), m_transpose});

    // Pattern 2: Transpose on output of MatMul
    auto m_brgemm_out =
        ov::pass::pattern::wrap_type<op::Brgemm>({ov::pass::pattern::any_input(), ov::pass::pattern::any_input()});
    auto m_transpose2 =
        ov::pass::pattern::wrap_type<ov::op::v1::Transpose>({m_brgemm_out, m_constant}, is_supported_transpose);

    auto m_brgemm_or_transpose =
        std::make_shared<ov::pass::pattern::op::Or>(OutputVector{m_brgemm_in0, m_brgemm_in1, m_transpose2});

    auto callback = [](ov::pass::pattern::Matcher& m) {
        OV_ITT_SCOPED_TASK(ov::pass::itt::domains::SnippetsTransform, "FuseTransposeBrgemm")
        auto brgemm = ov::as_type_ptr<op::Brgemm>(m.get_match_root());

        auto fuse_layouts = [](const std::vector<size_t>& layout_1, const std::vector<size_t>& layout_2) {
            if (layout_1.empty()) {
                return layout_2;
            }
            if (layout_2.empty()) {
                return layout_1;
            }
            OPENVINO_ASSERT(layout_1.size() == layout_2.size(), "Fused layouts must have equal ranks");
            std::vector<size_t> fused_layout(layout_1.size());
            for (size_t i = 0; i < layout_1.size(); ++i) {
                OPENVINO_ASSERT(layout_2[i] < layout_1.size(), "Fused layouts values mustn't exceed layout size");
                fused_layout[i] = layout_1[layout_2[i]];
            }
            return fused_layout;
        };

        // Transpose on the Brgemm's output
        if (!brgemm) {
            brgemm = ov::as_type_ptr<op::Brgemm>(m.get_match_root()->get_input_node_shared_ptr(0));
            const auto& brgemm_out = brgemm->output(0);
            const auto& transpose_out = m.get_match_value();
            const auto& const_order = ov::as_type_ptr<ov::op::v0::Constant>(
                transpose_out.get_node_shared_ptr()->get_input_node_shared_ptr(1));
            const auto& original_port = ov::snippets::lowered::PortDescriptorUtils::get_port_descriptor_ptr(brgemm_out);
            original_port->set_shape(utils::pshape_to_vdims(transpose_out.get_partial_shape()));
            const auto& out_layout = original_port->get_layout();
            const auto& transpose_order = const_order->cast_vector<size_t>();
            original_port->set_layout(fuse_layouts(out_layout, transpose_order));
            for (const auto& in : transpose_out.get_target_inputs()) {
                in.replace_source_output(brgemm->output(0));
            }
        }

        for (size_t i = 0; i < brgemm->get_input_size(); i++) {
            const auto& in = brgemm->input(i);
            const auto& in_value = in.get_source_output();
            if (is_supported_transpose(in_value)) {
                const auto& transpose = as_type_ptr<ov::op::v1::Transpose>(in_value.get_node_shared_ptr());
                const auto& const_order =
                    ov::as_type_ptr<ov::op::v0::Constant>(transpose->get_input_node_shared_ptr(1));
                brgemm->set_argument(i, transpose->input_value(0));
                const auto& original_port = ov::snippets::lowered::PortDescriptorUtils::get_port_descriptor_ptr(in);
                const auto& in_layout = original_port->get_layout();
                const auto& transpose_order = const_order->cast_vector<size_t>();
                original_port->set_shape(utils::pshape_to_vdims(transpose->get_input_partial_shape(0)));
                original_port->set_layout(fuse_layouts(transpose_order, in_layout));
            }
        }

        // need to run validate_and_infer_types manually: either input shapes were updated or
        // output Layout was updated (out shape will be updated in validate_and_infer_types())
        brgemm->validate_and_infer_types();
        return true;
    };

    register_matcher(std::make_shared<ov::pass::pattern::Matcher>(m_brgemm_or_transpose, matcher_name), callback);
}

}  // namespace ov::snippets::pass
