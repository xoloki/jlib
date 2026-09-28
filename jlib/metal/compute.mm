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

#include <jlib/metal/compute.hh>
#include <jlib/metal/impl.hh>

#include <cstring>
#include <map>

namespace jlib {
namespace metal {

struct compute::impl {
    std::shared_ptr<device> dev;
    id<MTLLibrary> lib = nil;

    // Built on first use and kept: creating a pipeline state is a compile
    // step of its own, not a lookup, and a caller dispatching per frame would
    // otherwise pay it every frame.
    std::map<std::string, id<MTLComputePipelineState> > pipelines;
};

struct compute::buffer::impl {
    id<MTLBuffer> buf = nil;
};

struct compute::call::impl {
    id<MTLCommandBuffer> cmd = nil;
    id<MTLComputeCommandEncoder> enc = nil;
    id<MTLComputePipelineState> pipe = nil;
    NSUInteger next = 0;
};

compute::compute() : m_impl(new impl) {}
compute::~compute() {}

compute::buffer::buffer() : m_impl(new impl) {}
compute::buffer::~buffer() {}

compute::call::call() : m_impl(new impl) {}
compute::call::~call() {}
compute::call::call(call&& c) : m_impl(std::move(c.m_impl)) {}

std::shared_ptr<compute> compute::build(const std::string& source) {
    std::shared_ptr<device> dev = device::shared();

    // Not make_shared: the constructor is private, and a would-be friend
    // declaration for the allocator is not portable across standard
    // libraries.  One extra control-block allocation, once per program.
    std::shared_ptr<compute> ret(new compute);

    ret->m_impl->dev = dev;

    NSError* err = nil;

    ret->m_impl->lib =
        [dev->m_impl->gpu newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                       options:nil
                                         error:&err];

    if(ret->m_impl->lib == nil) {
        // The compiler's own diagnostic, with the line and column in it.
        // Throwing "compilation failed" and dropping this is the difference
        // between a one-line fix and an afternoon.
        const char* what = (err != nil)
            ? [[err localizedDescription] UTF8String] : "unknown";
        throw exception(std::string("could not compile the source: ") + what);
    }

    return ret;
}

std::shared_ptr<device> compute::gpu() const {
    return m_impl->dev;
}

std::shared_ptr<compute::buffer> compute::alloc(std::size_t bytes) {
    // Metal rejects a zero-length buffer, and a caller reaching this with
    // zero is asking for an empty array rather than making a mistake --
    // round up so it gets a valid handle it can bind and never read.
    const std::size_t len = bytes ? bytes : 1;

    std::shared_ptr<buffer> ret(new buffer);

    // Shared, which on Apple silicon means the one physical copy both sides
    // address; see the unified-memory note in the header.
    ret->m_impl->buf = [m_impl->dev->m_impl->gpu newBufferWithLength:len
                                                             options:MTLResourceStorageModeShared];

    if(ret->m_impl->buf == nil)
        throw exception("could not allocate " + std::to_string(bytes) + " bytes");

    std::memset([ret->m_impl->buf contents], 0, len);

    return ret;
}

void* compute::buffer::data() {
    return [m_impl->buf contents];
}

const void* compute::buffer::data() const {
    return [m_impl->buf contents];
}

std::size_t compute::buffer::size() const {
    return [m_impl->buf length];
}

compute::call compute::kernel(const std::string& name) {
    id<MTLComputePipelineState> pipe = nil;

    std::map<std::string, id<MTLComputePipelineState> >::iterator i =
        m_impl->pipelines.find(name);

    if(i != m_impl->pipelines.end()) {
        pipe = i->second;
    } else {
        id<MTLFunction> fn =
            [m_impl->lib newFunctionWithName:[NSString stringWithUTF8String:name.c_str()]];

        if(fn == nil)
            throw exception("the source has no kernel named \"" + name + "\"");

        NSError* err = nil;

        pipe = [m_impl->dev->m_impl->gpu newComputePipelineStateWithFunction:fn error:&err];

        if(pipe == nil) {
            const char* what = (err != nil)
                ? [[err localizedDescription] UTF8String] : "unknown";
            throw exception("could not build a pipeline for \"" + name + "\": " + what);
        }

        m_impl->pipelines[name] = pipe;
    }

    call ret;

    ret.m_impl->cmd = [m_impl->dev->m_impl->queue commandBuffer];
    ret.m_impl->enc = [ret.m_impl->cmd computeCommandEncoder];
    ret.m_impl->pipe = pipe;

    [ret.m_impl->enc setComputePipelineState:pipe];

    return ret;
}

compute::call& compute::call::bind(const std::shared_ptr<buffer>& b) {
    [m_impl->enc setBuffer:b->m_impl->buf offset:0 atIndex:m_impl->next++];
    return *this;
}

compute::call& compute::call::bind(const void* bytes, std::size_t len) {
    [m_impl->enc setBytes:bytes length:len atIndex:m_impl->next++];
    return *this;
}

void compute::call::run(std::size_t threads) {
    // What this kernel can actually field, rather than a guess: a hand-picked
    // 256 is rejected outright by a pipeline whose register use caps it
    // lower, and hyper_reduce's per-thread scratch is exactly that kind of
    // kernel.  Asking the pipeline is the only way to be right for all of
    // them.
    NSUInteger width = [m_impl->pipe maxTotalThreadsPerThreadgroup];

    if(width > threads && threads > 0)
        width = threads;

    if(width < 1)
        width = 1;

    // Non-uniform threadgroups, so the grid is the thread count exactly and
    // the tail group is shortened by Metal rather than padded.  Supported on
    // every GPU that runs this library; the kernels still guard their upper
    // bound, which costs nothing and keeps them correct if that ever changes.
    [m_impl->enc dispatchThreads:MTLSizeMake(threads, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];

    [m_impl->enc endEncoding];
    [m_impl->cmd commit];
    [m_impl->cmd waitUntilCompleted];

    if([m_impl->cmd status] == MTLCommandBufferStatusError)
        throw exception(command_buffer_error(m_impl->cmd));

    // Spent: the encoder and command buffer are single-use, and a second
    // run() on the same call would be encoding into a committed buffer.
    m_impl->enc = nil;
    m_impl->cmd = nil;
}

}
}
