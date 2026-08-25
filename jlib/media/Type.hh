/* -*- mode: C++ c-basic-offset: 4 -*-
 * AudioFile.h
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_MEDIA_TYPE_HH
#define JLIB_MEDIA_TYPE_HH

#include <map>
#include <list>

#include <cmath>

namespace jlib {
    namespace media {
        
        class Type {
        public:
            
            typedef float scaled;

            static const int PCM_U8      = 0x0008;
            static const int PCM_S8      = 0x0040;
            static const int PCM_S16_LE  = 0x0010;
            static const int PCM_S16_BE  = 0x0020;
            static const int PCM_U16_LE  = 0x0080;
            static const int PCM_U16_BE  = 0x0100;
            static const int PCM_MPEG    = 0x0200;
            static const int PCM_AC3     = 0x0400;
            static const int PCM_FLOAT32 = 0x0800;
            
            template<int T>
            class sample {
            public:
                typedef unsigned short buf;
                static const bool is_signed = false;
                static Type::scaled scale(buf b) { return (b-pow(2,8*sizeof(buf)-1))/pow(2,8*sizeof(buf)-1); }
                static buf descale(scaled s) { return static_cast<buf>(s * pow(2,8*sizeof(buf)-1) + pow(2,8*sizeof(buf)-1)); }
            };
            
        };
        
        template<>
        class Type::sample<Type::PCM_U8> {
        public:
            typedef unsigned char buf;
            static const bool is_signed = false;

            static Type::scaled scale(buf b) { return (b - pow(2,8*sizeof(buf)-1))/pow(2,8*sizeof(buf)-1); }
            static buf descale(Type::scaled s) { return static_cast<buf>(s*pow(2,8*sizeof(buf)-1) + pow(2,8*sizeof(buf)-1));}
        };
        
        template<>
        class Type::sample<Type::PCM_S8> {
        public:
            typedef char buf;
            static const bool is_signed = true;

            static Type::scaled scale(buf b) { return (b)/pow(2,8*sizeof(buf)-1); }
            static buf descale(Type::scaled s) { return static_cast<buf>(s*pow(2,8*sizeof(buf)-1)); }
        };
        
        template<>
        class Type::sample<Type::PCM_S16_LE> {
        public:
            typedef short buf;
            static const bool is_signed = true;

            static Type::scaled scale(buf b) { return (b)/pow(2,8*sizeof(buf)-1); }
            static buf descale(Type::scaled s) { return static_cast<buf>(s*pow(2,8*sizeof(buf)-1)); }
        };
        
        template<>
        class Type::sample<Type::PCM_U16_LE> {
        public:
            typedef unsigned short buf;
            static const bool is_signed = false;

            static Type::scaled scale(buf b) { return (b - pow(2,8*sizeof(buf)-1))/pow(2,8*sizeof(buf)-1); }
            static buf descale(Type::scaled s) { return static_cast<buf>(s*pow(2,8*sizeof(buf)-1) + pow(2,8*sizeof(buf)-1));}
        };
        
        template<>
        class Type::sample<Type::PCM_S16_BE> {
        public:
            typedef short buf;
            static const bool is_signed = true;

            static Type::scaled scale(buf b) { return (b)/pow(2,8*sizeof(buf)-1); }
            static buf descale(Type::scaled s) { return static_cast<buf>(s*pow(2,8*sizeof(buf)-1)); }
        };
        
        template<>
        class Type::sample<Type::PCM_U16_BE> {
        public:
            typedef unsigned short buf;
            static const bool is_signed = false;

            static Type::scaled scale(buf b) { return (b - pow(2,8*sizeof(buf)-1))/pow(2,8*sizeof(buf)-1); }
            static buf descale(Type::scaled s) { return static_cast<buf>(s*pow(2,8*sizeof(buf)-1) + pow(2,8*sizeof(buf)-1));}
        };
        
        /*
        template<>
        class Type::sample<Type::PCM_MPEG> {
        public:
            typedef short buf;
            static const bool is_signed = false;

            static Type::scaled scale(buf b) { return b; }
        };
        
        template<>
        class Type::sample<Type::PCM_AC3> {
        public:
            typedef short buf;
            static const bool is_signed = false;

            static Type::scaled scale(buf b) { return b; }
        };
        */
        
        template<>
        class Type::sample<Type::PCM_FLOAT32> {
        public:
            typedef Type::scaled buf;
            static const bool is_signed = true;

            static Type::scaled scale(buf b) { return b; }
            static buf descale(Type::scaled s) { return s; }
        };
        
    }
}

#endif //JLIB_MEDIA_HH
