/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2017 Joey Yandle <xoloki@gmail.com>
 * 
 */

#include <cstddef>

namespace jlib {
namespace cuda {

void* malloc(std::size_t n);
void free(void* p);
    
}
}
