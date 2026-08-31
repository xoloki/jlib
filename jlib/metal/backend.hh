/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
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
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef JLIB_METAL_BACKEND_HH
#define JLIB_METAL_BACKEND_HH

#include <jlib/metal/tensor.hh>

#include <jlib/ai/backend.hh>

#include <memory>

namespace jlib {
namespace metal {

/**
 * ai::backend on the GPU.
 *
 * The dependency runs this way round on purpose: jlib/metal knows about
 * jlib/ai and not the reverse, so a machine without Metal builds the neural
 * library unchanged and a network that never asks for a GPU never links one.
 *
 * **Everything is deferred.**  Operations encode into one command buffer and
 * nothing has happened until wait() returns -- which is the entire point, and
 * measured at up to 7.9x against synchronising per operation.  A tensor read
 * before then holds whatever it held before.
 */
template<typename T>
class backend : public ai::backend<T> {
public:
    typedef typename ai::backend<T>::tensor_ptr tensor_ptr;

    explicit backend(std::shared_ptr<device> d = device::shared());

    ~backend();

    std::string name() const;

    tensor_ptr make(unsigned int rows, unsigned int cols);
    tensor_ptr make(const math::matrix<T>& m);

    typename ai::backend<T>::quantised_ptr make_q8_0(unsigned int rows,
                                                     unsigned int cols,
                                                     const void* blocks,
                                                     std::size_t bytes);

    void multiply_tn(const typename ai::backend<T>::quantised_ptr& a,
                     const tensor_ptr& b, tensor_ptr& c,
                     T alpha = T(1), T beta = T(0));

    void multiply(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c,
                  T alpha = T(1), T beta = T(0));
    void multiply_tn(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c,
                     T alpha = T(1), T beta = T(0));
    void multiply_nt(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c,
                     T alpha = T(1), T beta = T(0));

    void activate(ai::activation kind, const tensor_ptr& in, tensor_ptr& out);
    void slope(ai::activation kind, const tensor_ptr& out_of_layer, tensor_ptr& out);
    void hadamard(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c);
    void subtract(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c);
    void add_scaled(T alpha, const tensor_ptr& x, tensor_ptr& y);
    void assign(const tensor_ptr& src, tensor_ptr& dst);
    void softmax(const tensor_ptr& in, tensor_ptr& out);
    void causal_mask(tensor_ptr& s, unsigned int key_offset = 0,
                     unsigned int queries = 0);
    void attention_scores(const tensor_ptr& q, const tensor_ptr& k,
                          tensor_ptr& scores, unsigned int heads,
                          unsigned int kv_heads, unsigned int d_head, T scale);
    void attention_weighted(const tensor_ptr& v, const tensor_ptr& probs,
                            tensor_ptr& out, unsigned int heads,
                            unsigned int kv_heads, unsigned int d_head);
    void copy_columns(const tensor_ptr& src, tensor_ptr& dst,
                      unsigned int dst_first);
    void gather(const tensor_ptr& table, const std::vector<int>& ids,
                tensor_ptr& out);
    void rope(tensor_ptr& x, unsigned int base_pos = 0, float theta = 10000.0f,
              ai::rope_layout layout = ai::rope_layout::interleaved,
              unsigned int d_head = 0);
    void rms_norm(const tensor_ptr& in, const tensor_ptr& weight,
                  tensor_ptr& out, float eps = 1e-5f);

    void wait();

private:
    std::shared_ptr<device> m_device;
    std::unique_ptr<stream<T> > m_stream;
};

}
}

#endif // JLIB_METAL_BACKEND_HH
