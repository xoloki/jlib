/* -*- mode: ObjC++ c-basic-offset: 4 -*-
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

#include <jlib/metal/backend.hh>

namespace jlib {
namespace metal {

namespace {

/** ai::backend<T>::tensor over a metal::tensor<T>. */
template<typename T>
class device_tensor : public ai::backend<T>::tensor {
public:
    device_tensor(std::shared_ptr<device> d, unsigned int rows, unsigned int cols)
        : t(d, rows, cols) {}

    device_tensor(std::shared_ptr<device> d, const math::matrix<T>& m)
        : t(d, m) {}

    unsigned int rows() const { return t.rows(); }
    unsigned int cols() const { return t.cols(); }

    math::matrix<T> read() const { return t.read(); }
    void write(const math::matrix<T>& m) { t.write(m); }

    metal::tensor<T> t;
};

template<typename T>
metal::tensor<T>& at(const std::shared_ptr<typename ai::backend<T>::tensor>& p) {
    device_tensor<T>* d = dynamic_cast<device_tensor<T>*>(p.get());

    // A host tensor handed to the GPU backend.  Checked rather than
    // static_cast: the two are the same type at the call site and the
    // consequence otherwise is reading host memory as a device buffer.
    if(d == 0)
        throw ai::backend_error("that tensor did not come from this backend");

    return d->t;
}

/** ai's activation and metal's are separate enums with the same values. */
inline metal::activation as_metal(ai::activation a) {
    return static_cast<metal::activation>(static_cast<int>(a));
}

}

template<typename T>
backend<T>::backend(std::shared_ptr<device> d)
    : m_device(d)
{
    if(!d)
        throw ai::backend_error("no device");

    m_stream.reset(new stream<T>(d));
}

template<typename T>
backend<T>::~backend() = default;

template<typename T>
std::string backend<T>::name() const {
    return m_device->name() + (m_device->unified() ? " (unified memory)" : "");
}

template<typename T>
typename backend<T>::tensor_ptr backend<T>::make(unsigned int rows, unsigned int cols) {
    return tensor_ptr(new device_tensor<T>(m_device, rows, cols));
}

template<typename T>
typename backend<T>::tensor_ptr backend<T>::make(const math::matrix<T>& m) {
    return tensor_ptr(new device_tensor<T>(m_device, m));
}

template<typename T>
void backend<T>::multiply(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c,
                          T alpha, T beta)
{
    m_stream->multiply(at<T>(a), at<T>(b), at<T>(c), float(alpha), float(beta));
}

template<typename T>
void backend<T>::multiply_tn(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c,
                             T alpha, T beta)
{
    m_stream->multiply_tn(at<T>(a), at<T>(b), at<T>(c), float(alpha), float(beta));
}

template<typename T>
void backend<T>::multiply_nt(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c,
                             T alpha, T beta)
{
    m_stream->multiply_nt(at<T>(a), at<T>(b), at<T>(c), float(alpha), float(beta));
}

template<typename T>
void backend<T>::activate(ai::activation kind, const tensor_ptr& in, tensor_ptr& out) {
    m_stream->activate(as_metal(kind), at<T>(in), at<T>(out));
}

template<typename T>
void backend<T>::slope(ai::activation kind, const tensor_ptr& out_of_layer,
                       tensor_ptr& out)
{
    m_stream->slope(as_metal(kind), at<T>(out_of_layer), at<T>(out));
}

template<typename T>
void backend<T>::hadamard(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c) {
    m_stream->hadamard(at<T>(a), at<T>(b), at<T>(c));
}

template<typename T>
void backend<T>::subtract(const tensor_ptr& a, const tensor_ptr& b, tensor_ptr& c) {
    m_stream->subtract(at<T>(a), at<T>(b), at<T>(c));
}

template<typename T>
void backend<T>::add_scaled(T alpha, const tensor_ptr& x, tensor_ptr& y) {
    m_stream->add_scaled(float(alpha), at<T>(x), at<T>(y));
}

template<typename T>
void backend<T>::assign(const tensor_ptr& src, tensor_ptr& dst) {
    // Through add_scaled with the destination zeroed first would need a zero
    // kernel; subtracting a tensor from itself is one op and no new kernel.
    m_stream->subtract(at<T>(src), at<T>(src), at<T>(dst));   // dst = 0
    m_stream->add_scaled(1.0f, at<T>(src), at<T>(dst));       // dst += src
}

template<typename T>
void backend<T>::softmax(const tensor_ptr& in, tensor_ptr& out) {
    m_stream->softmax(at<T>(in), at<T>(out));
}

template<typename T>
void backend<T>::causal_mask(tensor_ptr& s) {
    m_stream->causal_mask(at<T>(s));
}

template<typename T>
void backend<T>::rms_norm(const tensor_ptr& in, const tensor_ptr& weight,
                          tensor_ptr& out, float eps)
{
    m_stream->rms_norm(at<T>(in), at<T>(weight), at<T>(out), eps);
}

template<typename T>
void backend<T>::wait() {
    m_stream->wait();
}

// Objective-C++ cannot be a template header; see tensor.mm.
template class backend<float>;
template class backend<_Float16>;

}
}
