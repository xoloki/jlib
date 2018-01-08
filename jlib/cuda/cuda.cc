/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2017 Joey Yandle <xoloki@gmail.com>
 * 
 */

#include <jlib/cuda/cuda.hh>

#include <stdexcept>

#include <cuda_runtime.h>

namespace jlib {
namespace cuda {

void* malloc(std::size_t n) {
    void* ret = nullptr;
    cudaError_t err = cudaMalloc (&ret, n);
    if (err != cudaSuccess) {
        throw std::runtime_error ("device memory allocation failed");
    }
    return ret;
}

void free(void* p) {
    cudaFree(p);
}
    
}
}
