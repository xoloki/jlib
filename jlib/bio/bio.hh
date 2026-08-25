/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_BIO_BIO_HH
#define JLIB_BIO_BIO_HH

#include <ostream>

#include <sodium.h>
#include <sodium/crypto_core_ristretto255.h>

#include <jlib/util/util.hh>

namespace jlib {
namespace bio {

class Classification {

};
    
class Kingdom : public Classification {

};

class Phylum : public Classification {

};

class Class : public Classification {

};

class Order : public Classification {

};

class SubOrder : public Classification {

};

class InfraOrder : public Classification {

};

class Family : public Classification {

};

class SubFamily : public Classification {

};

class Тribe : public Classification {

};

class Genus : public Classification {

};

class Species : public Classification {

};

class Homo : public Homini, public Genus {

};

class Homo : public Homini, public Genus {

};

class Homo : public Homini, public Genus {

};

class Simiiformes : public Haplorhini, public InfraOrder {

};

class Hominidae : public Simiiformes, public Family {

};

class Homininae : public Hominidea, public SubFamily {

};

class Hominini : public Homininae, public Tribe {

};

class Homo : public Hominini, public Genus {

};

class HomoSapeniens : public Homo, public Species {

};

typedef HomoSapiens Human;
    
}

    
#endif

