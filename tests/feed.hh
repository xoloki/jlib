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

#ifndef JLIB_TESTS_FEED_HH
#define JLIB_TESTS_FEED_HH

#include <unistd.h>

#include <cstddef>
#include <stdexcept>
#include <string>

/**
 * Put octets into a descriptor, all of them, or throw.
 *
 * Every async test feeds a pipe or a socket to make a reader wake up, and a
 * bare ::write there is wrong twice.  It ignores a short write, which on a
 * small pipe is not hypothetical -- and it ignores a *failed* one, which makes
 * the test that follows assert against input that never arrived and pass or
 * fail for reasons unrelated to what it is testing.
 *
 * gcc's warn_unused_result on write(2) is pointing at exactly that, which is
 * why it is worth a function rather than a cast.
 */
inline void feed(int fd, const char* data, std::size_t n) {
    std::size_t at = 0;

    while(at < n) {
        const ssize_t took = ::write(fd, data + at, n - at);

        if(took <= 0) {
            throw std::runtime_error("a test could not feed its descriptor");
        }

        at += static_cast<std::size_t>(took);
    }
}

inline void feed(int fd, const std::string& s) { feed(fd, s.data(), s.size()); }

/** The other half: drain what a test wrote, so a reader sees an empty buffer. */
inline std::string drain(int fd, std::size_t most = 65536) {
    std::string out;

    char buf[4096];

    while(out.size() < most) {
        const ssize_t got = ::read(fd, buf, sizeof buf);

        if(got <= 0) break;

        out.append(buf, static_cast<std::size_t>(got));
    }

    return out;
}

#endif // JLIB_TESTS_FEED_HH
