/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2017 Joey Yandle <xoloki@gmail.com>
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
 *
 */

#include <jlib/math/matrix.hh>

#include <jlib/cuda/cuda.hh>

#include <cuda_runtime.h>
#include <cublas_v2.h>


namespace jlib {
namespace cublas {

template<typename T>
struct gemm_base {
    //gemm_base(cublasHandle_t handle) : m_handle(handle) {}
    //cublasHandle_t m_handle;
    static void get_matrix(const T* cd, math::matrix<T> c);
    static T* set_matrix(math::matrix<T> m);
    static void set_matrix(math::matrix<T> m, T* ret);
};
    
template<typename T>
struct gemm : public gemm_base<T> {

    void operator()(cublasHandle_t handle, cublasOperation_t tra, cublasOperation_t trb, T alpha, math::matrix<T> a, const T* da, math::matrix<T> b, const T* db, T beta, math::matrix<T> c, T* dc) {}

};

template<>
struct gemm<float> : public gemm_base<float> {
    void operator()(cublasHandle_t handle, cublasOperation_t tra, cublasOperation_t trb, float alpha, math::matrix<float> a, const float* ad, math::matrix<float> b, const float* bd, float beta, math::matrix<float> c, float* cd) {
        cublasStatus_t stat;
        uint k = (tra == CUBLAS_OP_N ? a.N : a.M);
        
        stat = cublasSgemm (handle, tra, trb, c.M, c.N, k, &alpha, ad, a.M, bd, b.M, &beta, cd, c.M);
        if (stat != CUBLAS_STATUS_SUCCESS) {
            throw std::runtime_error ("CUBLAS multiplication failed\n");
        }
        
        this->get_matrix(cd, c);
    }
};

template<>
struct gemm<double> : public gemm_base<double> {
    void operator()(cublasHandle_t handle, cublasOperation_t tra, cublasOperation_t trb, double alpha, math::matrix<double> a, const double* ad, math::matrix<double> b, const double* bd, double beta, math::matrix<double> c, double* cd) {
        cublasStatus_t stat;
        uint k = (tra == CUBLAS_OP_N ? a.N : a.M);
        
        stat = cublasDgemm (handle, tra, trb, c.M, c.N, k, &alpha, ad, a.M, bd, b.M, &beta, cd, c.M);
        if (stat != CUBLAS_STATUS_SUCCESS) {
            throw std::runtime_error ("CUBLAS multiplication failed\n");
        }
        
        this->get_matrix(cd, c);
    }
};


template<typename T>
void gemm_base<T>::get_matrix(const T* cd, math::matrix<T> c) {
    math::buffer<T> cb = c;
    cublasStatus_t stat = cublasGetMatrix (c.M, c.N, sizeof(float), cd, c.M, cb.data(), c.M);
    if (stat != CUBLAS_STATUS_SUCCESS) {
        std::ostringstream os;
        os << "data upload failed: " << (int)stat;
        throw std::runtime_error(os.str());
    }
}

template<typename T>    
T* gemm_base<T>::set_matrix(math::matrix<T> m) {
    T* ret = (T*)cuda::malloc(m.M * m.N * sizeof(T));
    set_matrix(m, ret);
    return ret;
}

template<typename T>
void gemm_base<T>::set_matrix(math::matrix<T> m, T* ret) {
    math::buffer<T> buffer = m;
    
    cublasStatus_t stat = cublasSetMatrix (m.M, m.N, sizeof(T), buffer.data(), m.M, ret, m.M);
    
    if (stat != CUBLAS_STATUS_SUCCESS) {
        throw std::runtime_error ("data download failed");
    }
}
    
}
}
