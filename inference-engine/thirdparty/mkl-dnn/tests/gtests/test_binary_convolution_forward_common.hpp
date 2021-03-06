/*******************************************************************************
* Copyright 2019 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef TEST_BINARY_CONVOLUTION_FORWARD_COMMON_HPP
#define TEST_BINARY_CONVOLUTION_FORWARD_COMMON_HPP

#include "mkldnn_test_common.hpp"
#include "gtest/gtest.h"
#include "math_utils.hpp"
#include "mkldnn.hpp"

using namespace mkldnn::impl::math;

namespace {

}

namespace mkldnn {

void compute_ref_bin_conv_fwd(const test_binary_convolution_params_t &p,
        const memory::desc &src_d,
        const memory::desc &weights_d,
        const memory::desc &dst_d,
        const memory &src,
        const memory &weights,
        const memory &dst,
        const memory &depthwise_weights,
        const memory &depthwise_bias)
{
    auto c = p.sizes;

    uint8_t* src_data = (uint8_t*)src.get_data_handle();
    uint8_t* weights_data = (uint8_t*)weights.get_data_handle();
    float* dst_data = (float*)dst.get_data_handle();

    float *d_weights_data = (float *)depthwise_weights.get_data_handle();
    float *d_bias_data = (float *)depthwise_bias.get_data_handle();

    int nbits = 8;

    size_t padded_ic = src_d.data.layout_desc.blocking.padding_dims[1];
    size_t padded_ic_w = weights_d.data.layout_desc.blocking.padding_dims[1];
    size_t padded_oc_w = weights_d.data.layout_desc.blocking.padding_dims[0];

    auto extract_bit = [](uint8_t val, uint8_t bit) -> uint8_t {
        return (uint8_t) ((val >> bit) & 0x0001);
    };

    mkldnn::impl::parallel_nd(c.mb, c.ng, c.oc / c.ng, c.oh, c.ow,
        [&](int n, int g, int oc, int oh, int ow) {
            int32_t a = 0;
            int roi = 0;
            for (int ic = 0; ic < c.ic; ic++) {
                for (int kh = 0; kh < c.kh; kh++) {
                    for (int kw = 0; kw < c.kw; kw++) {
                        int ih = oh * c.strh - c.padh + kh * (1 + c.dilh);
                        int iw = ow * c.strw - c.padw + kw * (1 + c.dilw);

                        size_t iidx = n * padded_ic * c.ih * c.iw
                                      + g * padded_ic / c.ng * c.ih * c.iw
                                      + ic * c.ih * c.iw + ih * c.iw + iw;
                        iidx = map_index(src_d, iidx);

                        uint8_t s;
                        if (ih < 0 || ih >= c.ih || iw < 0 || iw >= c.iw) {
                            if (p.pad_value == 0.0f) {
                                continue;
                            } else {
                                s = p.pad_value == 1.0f ? (uint8_t)1 : (uint8_t)0;
                            }
                        } else {
                             s = extract_bit(src_data[iidx/nbits], (uint8_t)(iidx % nbits));
                        }

                        size_t widx = g * padded_oc_w / c.ng * padded_ic_w
                                      / c.ng * c.kh * c.kw
                                      + oc * padded_ic_w / c.ng * c.kh * c.kw
                                      + ic * c.kh * c.kw + kh * c.kw + kw;
                        widx = map_index(weights_d, widx);

                        uint8_t w = extract_bit(weights_data[widx/nbits], (uint8_t)(widx % nbits));

                        a += (int32_t)(s ^ w);

                        roi++;
                    }
                }
            }

            float a_fp = (float)(roi - 2*a);

            size_t oidx = n * c.oc * c.oh * c.ow +
                          g * c.oc / c.ng * c.oh * c.ow +
                          oc * c.oh * c.ow +
                          oh * c.ow +
                          ow;

            if (p.with_sum)
                a_fp += dst_data[map_index(dst_d, oidx)];

            switch (p.eltwise_algorithm) {
                case algorithm_undef:
                    break;
                case eltwise_relu:
                    a_fp = relu_fwd(a_fp, p.eltwise_alpha);
                    break;
                case eltwise_tanh:
                    a_fp = tanh_fwd(a_fp);
                    break;
                case eltwise_elu:
                    a_fp = elu_fwd(a_fp, p.eltwise_alpha);
                    break;
                case eltwise_square:
                    a_fp = square_fwd(a_fp);
                    break;
                case eltwise_abs:
                    a_fp = abs_fwd(a_fp);
                    break;
                case eltwise_sqrt:
                    a_fp = sqrt_fwd(a_fp);
                    break;
                case eltwise_linear:
                    a_fp = linear_fwd(a_fp, p.eltwise_alpha, p.eltwise_beta);
                    break;
                case eltwise_bounded_relu:
                    a_fp = bounded_relu_fwd(a_fp, p.eltwise_alpha);
                    break;
                case eltwise_soft_relu:
                    a_fp = soft_relu_fwd(a_fp);
                    break;
                case eltwise_logistic:
                    a_fp = logistic_fwd(a_fp);
                    break;
                case eltwise_clamp:
                    a_fp = clamp_fwd(a_fp, p.eltwise_alpha, p.eltwise_beta);
                    break;
                default:
                    assert(!"unknown alg_kind");
            }

            switch (p.depthwise_algorithm) {
                case algorithm_undef:
                    break;
                case depthwise_scale_shift:
                    a_fp = scale_shift_fwd(a_fp, d_weights_data[g * c.oc / c.ng + oc], d_bias_data[g * c.oc / c.ng + oc]);
                    break;
                case depthwise_prelu:
                    a_fp = prelu_fwd(a_fp, d_weights_data[g * c.oc / c.ng + oc]);
                    break;
                default: assert(!"unknown alg_kind");
            }

            dst_data[map_index(dst_d, oidx)] = a_fp;
        }
    );
}

void compute_ref_binarization_fwd(const test_binary_convolution_params_t &p,
    const memory::desc &src_md, const memory &src, const memory &weights, const memory &dst) {
    auto src_data = (float*)src.get_data_handle();
    auto weights_data = (float*)weights.get_data_handle();
    auto dst_data = (uint8_t*)dst.get_data_handle();

    const memory::desc src_d = src.get_primitive_desc().desc();
    const memory::desc weights_d = weights.get_primitive_desc().desc();
    const memory::desc dst_d = dst.get_primitive_desc().desc();

    int N = src_md.data.ndims > 0 ? src_md.data.dims[0] : 1;
    int C = src_md.data.ndims > 1 ? src_md.data.dims[1] : 1;
    int H = src_md.data.ndims > 2 ? src_md.data.dims[2] : 1;
    int W = src_md.data.ndims > 3 ? src_md.data.dims[3] : 1;

    int nbits = 8;
    int CB = div_up(C, nbits);

    int padded_ic = src_d.data.layout_desc.blocking.padding_dims[1];
    int padded_oc = dst_d.data.layout_desc.blocking.padding_dims[1];

    for (int n = 0; n < N; ++n) {
        for (int cb = 0; cb < CB; ++cb) {
            for (int h = 0; h < H; ++h) {
                for (int w = 0; w < W; ++w) {

                    uint8_t bin_val = 0x00;
                    for (int c = cb * nbits, shift = 0; c < std::min(C, (cb + 1) * nbits); c++, shift++) {
                        int src_idx = n*padded_ic*H*W + c*H*W + h*W + w;
                        int wei_idx = c;

                        float s_val = src_data[map_index(src_d, src_idx)];
                        float w_val = weights_data[map_index(weights_d, wei_idx)];

                        auto bit = uint8_t((s_val > w_val) ? 0x01 : 0x00);
                        bin_val |= (bit << shift);
                    }

                    int dst_idx = n*padded_oc*H*W + cb*nbits*H*W + h*W + w;
                    dst_idx = map_index(dst_d, dst_idx);
                    dst_data[dst_idx / nbits] = bin_val;
                }
            }
        }
    }
}

class binary_convolution_forward_test : public ::testing::TestWithParam<test_binary_convolution_params_t>
{
protected:
    virtual void SetUp()
    {
        test_binary_convolution_params_t p = ::testing::TestWithParam<test_binary_convolution_params_t>::GetParam();

        ASSERT_TRUE(p.engine_kind == engine::kind::cpu);
        ASSERT_EQ(p.aalgorithm, algorithm::binary_convolution_direct);

        test_convolution_sizes_t cd = p.sizes;

        auto eng = engine(p.engine_kind, 0);
        auto aprop_kind = prop_kind::forward;
        bool with_binarization = p.binarization_algorithm != algorithm_undef;

        memory::data_type data_type_src = memory::data_type::bin;
        memory::data_type data_type_wei = memory::data_type::bin;
        memory::data_type data_type_bia = memory::data_type::f32;
        memory::data_type data_type_dst = with_binarization ? memory::data_type::bin
                                                            : data_traits<float>::data_type;

        auto c_src_desc = create_md({ cd.mb, cd.ic, cd.ih, cd.iw }, data_type_src, p.formats.src_format);
        auto c_weights_desc = cd.ng > 1
                ? create_md({ cd.ng, cd.oc / cd.ng, cd.ic / cd.ng, cd.kh, cd.kw }, data_type_wei, p.formats.weights_format)
                : create_md({ cd.oc, cd.ic, cd.kh, cd.kw }, data_type_wei, p.formats.weights_format);
        auto c_dst_desc = create_md({ cd.mb, cd.oc, cd.oh, cd.ow }, data_type_dst, p.formats.dst_format);

        auto c_src = test_memory(c_src_desc, eng);
        auto c_weights = test_memory(c_weights_desc, eng);
        auto c_dst = test_memory(c_dst_desc, eng);

        // Only true for dense format
        if (with_binarization)
            fill_data<uint8_t>(c_dst.get_size() / sizeof(uint8_t), (uint8_t*)c_dst.get().get_data_handle());
        else
            fill_data<float>(c_dst.get_size() / sizeof(float), (float*)c_dst.get().get_data_handle());
        fill_data<uint8_t>(c_src.get_size() / sizeof(uint8_t), (uint8_t*)c_src.get().get_data_handle());
        fill_data<uint8_t>(c_weights.get_size() / sizeof(uint8_t), (uint8_t*)c_weights.get().get_data_handle());

        std::vector<ptrdiff_t> padR = {
            right_padding(cd.ih, cd.oh, cd.kh, cd.padh, cd.strh, cd.dilh),
            right_padding(cd.iw, cd.ow, cd.kw, cd.padw, cd.strw, cd.dilw)
        };

        auto bin_conv_desc = binary_convolution_forward::desc(aprop_kind, p.aalgorithm,
                    c_src_desc, c_weights_desc, c_dst_desc,
                    { cd.strh, cd.strw }, { cd.dilh, cd.dilw },
                    { cd.padh, cd.padw }, padR, p.pad_value);

        mkldnn::post_ops ops;

        if (p.with_sum)
            ops.append_sum();

        if (p.eltwise_algorithm != algorithm_undef)
            ops.append_eltwise(1.0, p.eltwise_algorithm, p.eltwise_alpha, p.eltwise_beta);

        auto c_depthwise_weights_desc = create_md({ cd.oc }, data_type_bia, memory::x);
        auto c_depthwise_bias_desc = create_md({ cd.oc }, data_type_bia, memory::x);

        auto c_depthwise_weights = memory({c_depthwise_weights_desc, eng});
        auto c_depthwise_bias = memory({c_depthwise_bias_desc, eng});

        if (p.depthwise_algorithm != algorithm_undef) {
            fill_data<float>(c_depthwise_weights.get_primitive_desc().get_size() / sizeof(float),
                             (float *)c_depthwise_weights.get_data_handle(), 1., true);
            fill_data<float>(c_depthwise_bias.get_primitive_desc().get_size() / sizeof(float),
                             (float *)c_depthwise_bias.get_data_handle(), 1., true);

            ops.append_depthwise(p.depthwise_algorithm, static_cast<const float*>(c_depthwise_weights.get_data_handle()),
                                                        static_cast<const float*>(c_depthwise_bias.get_data_handle()));
        }

        auto c_binarization_weights_desc = create_md({ cd.oc }, memory::data_type::f32, memory::x);
        auto c_binarization_weights = memory({c_binarization_weights_desc, eng});

        if (p.binarization_algorithm != algorithm_undef) {
            fill_data<float>(c_binarization_weights.get_primitive_desc().get_size() / sizeof(float),
                             (float *)c_binarization_weights.get_data_handle(), 1., true);

            ops.append_binarization(p.binarization_algorithm, static_cast<const float*>(c_binarization_weights.get_data_handle()));
        }

        mkldnn::primitive_attr attr;
        attr.set_post_ops(ops);

        auto bin_conv_primitive_desc = binary_convolution_forward::primitive_desc(bin_conv_desc, attr, eng);

        auto bin_conv = binary_convolution_forward(bin_conv_primitive_desc, c_src.get(), c_weights.get(), c_dst.get());

        if (with_binarization) {
            auto c_dst_desc_ref = create_md({ cd.mb, cd.oc, cd.oh, cd.ow }, memory::data_type::f32, p.formats.dst_format);
            auto c_dst_ref = test_memory(c_dst_desc_ref, eng);

            std::vector<float> ref_dst_conv_data(c_dst_ref.get_size() / sizeof(float));
            auto ref_conv_memory = memory(memory::primitive_desc(c_dst_desc_ref, eng), &ref_dst_conv_data[0]);

            std::vector<uint8_t > ref_dst_data(c_dst.get_size() / sizeof(uint8_t));
            auto ref_memory = memory(memory::primitive_desc(c_dst_desc, eng), &ref_dst_data[0]);

            compute_ref_bin_conv_fwd(p, c_src_desc, c_weights_desc, c_dst_desc_ref,
                                     c_src.get(), c_weights.get(), ref_conv_memory,
                                     c_depthwise_weights, c_depthwise_bias);

            compute_ref_binarization_fwd(p, c_dst_desc_ref, ref_conv_memory, c_binarization_weights, ref_memory);

            std::vector<primitive> pipeline;
            pipeline.push_back(bin_conv);
            auto s = stream(stream::kind::lazy);
            s.submit(pipeline).wait();

            compare_data<uint8_t>(ref_memory, c_dst.get(), 0, true);
        } else {
            std::vector<float> ref_dst_data(c_dst.get_size() / sizeof(float));
            memcpy(&ref_dst_data[0], (float*)c_dst.get().get_data_handle(), ref_dst_data.size() * sizeof(float));
            auto ref_memory = memory(memory::primitive_desc(c_dst_desc, eng), &ref_dst_data[0]);

            compute_ref_bin_conv_fwd(p, c_src_desc, c_weights_desc, c_dst_desc,
                                     c_src.get(), c_weights.get(), ref_memory,
                                     c_depthwise_weights, c_depthwise_bias);

            std::vector<primitive> pipeline;
            pipeline.push_back(bin_conv);
            auto s = stream(stream::kind::lazy);
            s.submit(pipeline).wait();

            compare_data<float>(ref_memory, c_dst.get(), 1e-3);
        }
    }
};

}

#endif
