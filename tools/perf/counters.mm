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
 *
 */

// What can this device tell us without Xcode?
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <cstdio>
int main() {
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        printf("  device: %s\n", [[d name] UTF8String]);
        printf("  counter sets:\n");
        for(id<MTLCounterSet> cs in [d counterSets]) {
            printf("    %-28s  %lu counters:", [[cs name] UTF8String],
                   (unsigned long)[[cs counters] count]);
            for(id<MTLCounter> c in [cs counters])
                printf(" %s", [[c name] UTF8String]);
            printf("\n");
        }
        printf("  timestamp sampling at dispatch boundary: %s\n",
               [d supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary]
                   ? "yes" : "NO");
        printf("  timestamp sampling at stage boundary:    %s\n",
               [d supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]
                   ? "yes" : "NO");
    }
    return 0;
}
