/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2020 Joey Yandle <xoloki@gmail.com>
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

#include <jlib/crypt/crypt.hh>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <unistd.h>

namespace gpg = jlib::crypt::gpg;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::string home_a, home_b;

/**
 * Registered with atexit, so it runs on the skip paths below as well as on the
 * ordinary one.  Generating a key spawns a gpg-agent bound to that homedir; it
 * outlives the process that asked for it, so leaving without the kill leaves a
 * daemon behind holding a directory under /tmp.
 */
static void cleanup() {
    for(const std::string& h : { home_a, home_b }) {
        if(h.empty())
            continue;

        std::system(("gpgconf --homedir '" + h + "' --kill all >/dev/null 2>&1").c_str());
        std::system(("rm -rf '" + h + "'").c_str());
    }
}

/** 77 is automake's exit code for a test that could not run. */
static void skip(const std::string& why) {
    std::cout << "SKIP: " << why << "\n";
    std::exit(77);
}

/**
 * A fresh gnupg home.  Deliberately short and directly under /tmp rather than
 * under TMPDIR: gpg puts its agent sockets in the homedir, and a unix socket
 * path is capped near 104 bytes, which macOS's per-user TMPDIR alone can eat.
 */
static std::string temp_home(const char* tag) {
    std::string tmpl = std::string("/tmp/jlib_gpg_") + tag + "_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());

    buf.push_back('\0');

    if(::mkdtemp(buf.data()) == 0)
        skip(std::string("could not create a temporary gnupg home: ") + std::strerror(errno));

    return std::string(buf.data());
}

/** Generate a signing and encrypting key in c's homedir. */
static void create_key(gpg::ctx& c, const std::string& uid) {
    // "default" rather than a named algorithm: gpg's default is a signing
    // primary with an encryption subkey, and this test needs both.
    gpgme_error_t err = gpgme_op_createkey(c.gpgme(), uid.c_str(), "default", 0, 0, 0,
                                           GPGME_CREATE_NOPASSWD);

    if(gpgme_err_code(err) != GPG_ERR_NO_ERROR)
        skip("could not generate a key: " + std::string(gpgme_strerror(err)));
}

int main(int argc, char** argv) {
    std::atexit(cleanup);

    home_a = temp_home("a");

    // Before gpg::init, which reads it.
    if(::setenv("GNUPGHOME", home_a.c_str(), 1) != 0)
        skip("could not set GNUPGHOME");

    try {
        gpg::init(GPGME_PROTOCOL_OpenPGP);
    } catch(const std::exception& e) {
        skip(std::string("no OpenPGP engine: ") + e.what());
    }

    const std::string uid = "jlib test <test@example.invalid>";

    gpg::ctx c;
    create_key(c, uid);

    gpg::key::list keys = gpg::list_keys("test@example.invalid", true);
    if(keys.empty())
        skip("the generated key is not in the keyring");

    const gpg::key::ptr k = keys.front();

    std::cout << "#416 a verify result is evidence, not a verdict\n";
    {
        ok("#416 a null result is not verified", !gpg::verified(0));

        const std::string message = "the quick brown fox";

        gpg::data::ptr plain = gpg::data::create(message);
        gpg::data::ptr sig = gpg::data::create();

        c.signers_clear();
        c.signers_add(k);
        c.op_sign(plain, sig, GPGME_SIG_MODE_DETACH);

        plain->rewind();
        ok("a good detached signature is verified",
           gpg::verified(c.op_verify_detached(sig, plain)));

        // The trap, demonstrated rather than argued.  op_verify is the inline
        // form; handed this detached signature it has nothing to check the
        // signature against, and gpgme reports that as a *successful*
        // operation carrying no signatures.  A caller who verified by seeing
        // that op_verify did not throw read this as a good signature.
        sig->rewind();
        gpg::data::ptr out = gpg::data::create();
        gpgme_verify_result_t empty = 0;
        bool empty_threw = false;
        try {
            empty = c.op_verify(sig, out);
        } catch(const std::exception& e) {
            empty_threw = true;
        }
        ok("#416 verifying a detached signature as an inline one does not throw", !empty_threw);
        ok("#416 and verified() rejects it, where the absence of a throw did not",
           !gpg::verified(empty));

        // Not asserted: that the result carries an empty signature list.  That
        // is how gpgme 2.1.2 reports this, and asserting it failed against an
        // older gpgme in a container, which returns a signature that does not
        // verify instead.  Either way op_verify does not throw and verified()
        // says no, which are the two things a caller depends on -- the outcome
        // rather than the mechanism that produces it.
        std::cout << "       (signatures reported: "
                  << ((empty != 0 && empty->signatures != 0) ? "one or more" : "none")
                  << ")\n";

        // The case the old interface made easy to get wrong.  op_verify
        // returns an error only for operational failure, so a signature over
        // different data is a *successful* verify operation -- it must not
        // throw, and the caller who stopped there saw success.
        bool threw = false;
        bool said_verified = true;
        try {
            sig->rewind();
            gpg::data::ptr tampered = gpg::data::create(std::string("the quick brown dog"));
            said_verified = gpg::verified(c.op_verify_detached(sig, tampered));
        } catch(const std::exception& e) {
            threw = true;
        }
        ok("#416 a signature over tampered data does not throw", !threw,
           "the operation succeeded; only the signature is bad");
        ok("#416 and it is not verified", !said_verified);

        // Truncation, same shape.
        bool trunc_threw = false;
        bool trunc_verified = true;
        try {
            sig->rewind();
            gpg::data::ptr cut = gpg::data::create(message.substr(0, message.size() - 1));
            trunc_verified = gpg::verified(c.op_verify_detached(sig, cut));
        } catch(const std::exception& e) {
            trunc_threw = true;
        }
        ok("#416 a signature over truncated data does not throw", !trunc_threw);
        ok("#416 and it is not verified", !trunc_verified);
    }

    std::cout << "\n#420 key::to_array\n";
    {
        const std::vector<gpgme_key_t> arr = gpg::key::to_array(keys);

        ok("#420 to_array is one longer than the list", arr.size() == keys.size() + 1,
           std::to_string(arr.size()) + " for " + std::to_string(keys.size()) + " keys");
        ok("#420 and NULL-terminated, which is what gpgme reads to", arr.back() == 0);
        ok("#420 and it owns its storage, so no caller has to delete it",
           arr.front() == k->gpgme());

        const std::vector<gpgme_key_t> empty = gpg::key::to_array(gpg::key::list());
        ok("#420 an empty list is still a terminated array",
           empty.size() == 1 && empty.front() == 0);
    }

    std::cout << "\n#420 encryption no longer always trusts\n";
    {
        // Our own secret key is ultimately trusted by gpg, so it is the case
        // that must work without any trust override.
        gpg::key::list to;
        to.push_back(k);

        gpg::data::ptr plain = gpg::data::create(std::string("secret"));
        gpg::data::ptr cipher = gpg::data::create();

        bool threw = false;
        try {
            c.op_encrypt(to, plain, cipher);
        } catch(const std::exception& e) {
            threw = true;
            std::cout << "       (" << e.what() << ")\n";
        }
        ok("#420 encrypting to our own trusted key needs no trust override", !threw);

        if(!threw) {
            gpg::data::ptr back = gpg::data::create();
            c.op_decrypt(cipher, back);
            ok("and the ciphertext decrypts to the plaintext", back->read() == "secret");
        }

        // Now a key the trust model has no reason to accept: generated in a
        // second homedir and imported here as a bare public key.  This is the
        // case GPGME_ENCRYPT_ALWAYS_TRUST used to wave through on every single
        // encryption, which is what made a substituted key indistinguishable
        // from the right one.
        home_b = temp_home("b");

        gpg::ctx other;
        if(gpgme_err_code(gpgme_ctx_set_engine_info(other.gpgme(), GPGME_PROTOCOL_OpenPGP,
                                                    0, home_b.c_str())) != GPG_ERR_NO_ERROR) {
            std::cout << "  SKIP  the untrusted-key checks: could not point a context at a second homedir\n";
        } else {
            create_key(other, "jlib other <other@example.invalid>");

            gpgme_data_t pub = 0;
            gpgme_data_new(&pub);

            if(gpgme_err_code(gpgme_op_export(other.gpgme(), "other@example.invalid", 0, pub))
               != GPG_ERR_NO_ERROR) {
                std::cout << "  SKIP  the untrusted-key checks: could not export the second key\n";
            } else {
                gpgme_data_seek(pub, 0, SEEK_SET);
                gpgme_op_import(c.gpgme(), pub);

                gpg::key::list untrusted = gpg::list_keys("other@example.invalid");
                if(untrusted.empty()) {
                    std::cout << "  SKIP  the untrusted-key checks: the imported key is not in the keyring\n";
                } else {
                    gpg::data::ptr p2 = gpg::data::create(std::string("secret"));
                    gpg::data::ptr c2 = gpg::data::create();

                    bool refused = false;
                    try {
                        c.op_encrypt(untrusted, p2, c2);
                    } catch(const std::exception& e) {
                        refused = true;
                    }
                    ok("#420 encrypting to an untrusted key is refused by default", refused);

                    gpg::data::ptr p3 = gpg::data::create(std::string("secret"));
                    gpg::data::ptr c3 = gpg::data::create();

                    bool allowed = true;
                    try {
                        c.op_encrypt(untrusted, p3, c3, gpg::trust::always);
                    } catch(const std::exception& e) {
                        allowed = false;
                        std::cout << "       (" << e.what() << ")\n";
                    }
                    ok("#420 and allowed when the caller asks for trust::always", allowed,
                       "which is what every encryption used to do unconditionally");
                }
            }
            gpgme_data_release(pub);
        }
    }

    // Deliberately not tested here:
    //
    // That init() honours its protocol argument.  It does now -- it used to
    // check for an OpenPGP engine whatever it was asked for -- but asserting
    // it means asking for a protocol this host has no engine for, and whether
    // gpgsm is installed is a property of the host rather than of this code.
    //
    // Nor revoked, expired or missing-key signatures.  verified() rejects each
    // by its summary flag, and producing them needs a key whose state this
    // test would have to manufacture rather than observe.
    //
    // Nor the to_array leak itself, which is now structural -- the function
    // returns a vector -- and not something a run can observe.

    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
