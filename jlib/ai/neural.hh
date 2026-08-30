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

#include <jlib/ai/backend.hh>

#include <jlib/math/matrix.hh>
#include <jlib/util/util.hh>
#include <jlib/util/json.hh>

#include <functional>
#include <string>
#include <random>
#include <fstream>
#include <tuple>

#include <cmath>

namespace jlib {
namespace ai {

// activation, LEAK, name_of, activation_from_name, activate_matrix and
// slope_matrix all moved to backend.hh, which is where the arithmetic they
// describe now happens.  They are still ai:: names, so nothing that used them
// has to change.

template<typename T>
class NeuralNetwork {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "jlib::ai::NeuralNetwork exception" +
                (msg.empty() ? std::string() : ": " + msg);
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    NeuralNetwork(double lrate, uint ninput, const std::vector<uint>& hidden, uint noutput);

    NeuralNetwork(util::json::object::ptr p);

    template<typename... Args>
    NeuralNetwork(double lrate, uint ninput, uint noutput, Args&&... args)
        : NeuralNetwork(lrate, ninput, std::vector<uint>({args...}), noutput)
    {
    }

    math::matrix<T> train(math::matrix<T> inputs, math::matrix<T> targets);
    math::matrix<T> query(math::matrix<T> inputs);

    util::json::object::ptr json();

    std::default_random_engine& get_generator();

    void set_rate(double rate);

    /**
     * Which nonlinearity the hidden layers use, and which the output uses.
     *
     * Both default to sigmoid, so a network that says nothing behaves exactly
     * as it did before these existed.
     *
     * **They are separate on purpose.**  ReLU is what lets a deep network
     * train -- the sigmoid's derivative peaks at 0.25, so the gradient is
     * attenuated at every hop and at three hidden layers nothing reaches the
     * input end (#131).  But ReLU on the *output* is wrong for a classifier
     * scored against targets of 0.01 and 0.99: its derivative is zero
     * wherever the output is, so an output unit that goes negative stops
     * learning and never comes back.
     *
     * Hidden relu with a sigmoid output is the ordinary arrangement, and it is
     * what the measurements in #131 were taken with.
     */
    activation get_hidden_activation() const { return m_hidden_activation; }
    void set_hidden_activation(activation a) { m_hidden_activation = a; }

    activation get_output_activation() const { return m_output_activation; }
    void set_output_activation(activation a) { m_output_activation = a; }

    /**
     * Where the numbers live and who does the arithmetic.
     *
     * Defaults to the host.  Setting a GPU backend moves the weights onto it
     * and keeps them there -- which is the point, since uploading them per
     * step would cost more than the multiply saves:
     *
     *     nn.set_backend(std::make_shared<metal::backend<float> >());
     *
     * This replaced a set_multiply() hook that took only the matrix product.
     * That hook worked, and left everything else on the CPU, so every layer
     * bounced the data back and forth and a step paid eight synchronisations.
     * Keeping the whole step on one device is worth up to 7.9x by itself.
     *
     * Safe at any time: the weights are read back and re-uploaded, so a
     * network can move between devices mid-training.
     */
    void set_backend(std::shared_ptr<backend<T> > b);

    backend<T>& get_backend() const { return *m_backend; }

    
protected:
    typedef typename backend<T>::tensor_ptr tensor_ptr;

    uint m_ninput;
    std::vector<uint> m_nhidden;
    uint m_noutput;
    double m_lrate;

    /**
     * Where the numbers live.  The host by default; a GPU if asked.
     *
     * The weights stay wherever this puts them, between calls as well as
     * within one.  That is the whole point: uploading them per step would
     * give back everything residency buys.
     */
    std::shared_ptr<backend<T> > m_backend;

    tensor_ptr m_wih;
    tensor_ptr m_who;
    std::vector<tensor_ptr> m_deep;

    /**
     * Scratch, sized for the batch and kept until the batch changes.
     *
     * A training step allocates nothing.  Everything here is shaped by the
     * batch size, so reserve() rebuilds when it moves and does nothing when it
     * does not -- which for a training loop is always.
     */
    uint m_batch = 0;

    tensor_ptr m_in, m_target, m_zo, m_out, m_err, m_slope_out, m_delta_out;

    std::vector<tensor_ptr> m_z;      // pre-activation, per hidden layer
    std::vector<tensor_ptr> m_h;      // activation
    std::vector<tensor_ptr> m_e;      // error arriving at that layer's output
    std::vector<tensor_ptr> m_slope;  // f'(h)

    void reserve(uint batch);
    void forward();
    // Was a std::function holding a sigmoid, with the matching derivative
    // written out by hand in train() as `out ^ (1 - out)`.  The function was
    // replaceable and the derivative was not, so replacing it silently trained
    // against the wrong slope.  A pair of enums keeps them together and
    // serialises, which a lambda could not.
    activation m_hidden_activation = activation::sigmoid;
    activation m_output_activation = activation::sigmoid;

    std::default_random_engine m_generator;
};

template<typename T>
NeuralNetwork<T>::NeuralNetwork(double lrate, uint ninput, const std::vector<uint>& hidden, uint noutput)
    : m_ninput(ninput),
      m_noutput(noutput),
      m_lrate(lrate),
      m_nhidden(hidden),
      m_backend(new host_backend<T>)
{
    if(m_nhidden.empty())
        throw std::runtime_error("Need at least one hidden layer");

    // Built on the host and then uploaded.  The random draw is the same
    // sequence whatever the backend is, so a network is identical on the CPU
    // and the GPU before a single step -- which is what lets the two be
    // compared at all.
    math::matrix<T> wih(m_nhidden.front(), m_ninput);
    math::matrix<T> who(m_noutput, m_nhidden.back());

    // Fan-in, not fan-out.  1/sqrt(fan) is the right heuristic and was always
    // the intent, but each of the three below passed the wrong dimension:
    // m_wih is (hidden x input), so the number of weights feeding each hidden
    // unit is m_ninput, not m_nhidden[0].  See #131 -- with 784 inputs and 200
    // hidden this was twice as wide as it should be, and who below was four
    // and a half times.
    //
    // Weights that are too large push the sigmoids toward their flat ends,
    // where the derivative approaches zero, which starves the backward pass on
    // top of the attenuation it already suffers per layer.
    T hbound = pow(m_ninput, -0.5);
    std::uniform_real_distribution<double> hdist(-double(hbound), double(hbound));

    wih.foreach([&](T& x) { x = T(hdist(m_generator)); });

    // who is (output x hidden): the fan-in is the hidden layer feeding it,
    // not the number of classes coming out.  This was the worst of the three
    // and the one the backward pass starts from.
    T obound = pow(m_nhidden.back(), -0.5);
    std::uniform_real_distribution<double> odist(-double(obound), double(obound));

    who.foreach([&](T& x) { x = T(odist(m_generator)); });

    std::vector<math::matrix<T> > deep;

    for(std::size_t i = 1; i < m_nhidden.size(); i++) {
        // m_deep[i-1] is (n[i] x n[i-1]), so the fan-in is the layer below.
        // Identical to the old expression when every hidden layer is the same
        // width, which is why the case people try first is the case this got
        // right.
        T dbound = pow(m_nhidden[i-1], -0.5);
        std::uniform_real_distribution<double> ddist(-double(dbound), double(dbound));

        deep.push_back(math::matrix<T>(m_nhidden[i], m_nhidden[i-1]));
        deep.back().foreach([&](T& x) { x = T(ddist(m_generator)); });
    }

    m_wih = m_backend->make(wih);
    m_who = m_backend->make(who);

    for(const math::matrix<T>& d : deep)
        m_deep.push_back(m_backend->make(d));
}

template<typename T>
NeuralNetwork<T>::NeuralNetwork(util::json::object::ptr p)
    : m_ninput(p->get("ninput")),
      m_noutput(p->get("noutput")),
      m_lrate(p->get("lrate")),
      m_hidden_activation(p->has("hidden_activation")
                          ? activation_from_name(p->get("hidden_activation"))
                          : activation::sigmoid),
      m_output_activation(p->has("output_activation")
                          ? activation_from_name(p->get("output_activation"))
                          : activation::sigmoid),
      m_backend(new host_backend<T>)
{
    util::json::object::ptr nh = p->obj("nhidden");

    if(nh->is(util::json::object::type_array)) {
        for(std::size_t i = 0; i < nh->size(); i++)
            m_nhidden.push_back(nh->get(i));
    }

    if(m_nhidden.empty())
        throw std::runtime_error("Need at least one hidden layer");

    math::matrix<T> wih(m_nhidden.front(), m_ninput);

    wih.foreach_index([&](uint r, uint c, T& x) {
            x = T(double(p->obj("wih")->get(r*m_ninput + c)));
        });

    m_wih = m_backend->make(wih);

    for(std::size_t i = 1; i < m_nhidden.size(); i++) {
        math::matrix<T> d(m_nhidden[i], m_nhidden[i-1]);

        d.foreach_index([&](uint r, uint c, T& x) {
                x = T(double(p->obj("deep")->obj(i-1)->get(r*m_nhidden[i-1] + c)));
            });

        m_deep.push_back(m_backend->make(d));
    }

    math::matrix<T> who(m_noutput, m_nhidden.back());

    who.foreach_index([&](uint r, uint c, T& x) {
            x = T(double(p->obj("who")->get(r*m_nhidden.back() + c)));
        });

    m_who = m_backend->make(who);
}

template<typename T>
void NeuralNetwork<T>::set_backend(std::shared_ptr<backend<T> > b) {
    if(!b || b == m_backend)
        return;

    // Read the weights off the old device before the new one takes over, then
    // upload.  A network can move mid-training this way, which is mostly
    // useful for testing one backend against another.
    const math::matrix<T> wih = m_wih->read();
    const math::matrix<T> who = m_who->read();

    std::vector<math::matrix<T> > deep;

    for(const tensor_ptr& d : m_deep)
        deep.push_back(d->read());

    m_backend = b;

    m_wih = b->make(wih);
    m_who = b->make(who);

    m_deep.clear();

    for(const math::matrix<T>& d : deep)
        m_deep.push_back(b->make(d));

    // The scratch belonged to the old backend, and a tensor handed to the
    // wrong one is refused rather than misread.  Dropped, so reserve() builds
    // it again on the next call.
    m_batch = 0;
}

template<typename T>
void NeuralNetwork<T>::reserve(uint batch) {
    if(batch == m_batch)
        return;

    backend<T>& b = *m_backend;

    m_in = b.make(m_ninput, batch);
    m_target = b.make(m_noutput, batch);
    m_zo = b.make(m_noutput, batch);
    m_out = b.make(m_noutput, batch);
    m_err = b.make(m_noutput, batch);
    m_slope_out = b.make(m_noutput, batch);
    m_delta_out = b.make(m_noutput, batch);

    m_z.clear(); m_h.clear(); m_e.clear(); m_slope.clear();

    for(std::size_t i = 0; i < m_nhidden.size(); i++) {
        m_z.push_back(b.make(m_nhidden[i], batch));
        m_h.push_back(b.make(m_nhidden[i], batch));
        m_e.push_back(b.make(m_nhidden[i], batch));
        m_slope.push_back(b.make(m_nhidden[i], batch));
    }

    m_batch = batch;
}

template<typename T>
void NeuralNetwork<T>::forward() {
    backend<T>& b = *m_backend;

    const std::size_t n = m_nhidden.size() - 1;   // the last hidden layer

    b.multiply(m_wih, m_in, m_z[0]);
    b.activate(m_hidden_activation, m_z[0], m_h[0]);

    for(std::size_t i = 1; i <= n; i++) {
        b.multiply(m_deep[i-1], m_h[i-1], m_z[i]);
        b.activate(m_hidden_activation, m_z[i], m_h[i]);
    }

    b.multiply(m_who, m_h[n], m_zo);
    b.activate(m_output_activation, m_zo, m_out);
}

template<typename T>
math::matrix<T> NeuralNetwork<T>::train(math::matrix<T> inputs, math::matrix<T> targets){
    // A column per sample.  Every step below is written in matrix terms that
    // carry a batch: wih * inputs is (H x I)(I x B) = H x B, the elementwise
    // terms stay elementwise, and multiplying by a transpose contracts the
    // batch away to leave a gradient of the weight's own shape.
    //
    // The gradient is a mean rather than a sum, which is what makes the
    // learning rate mean something independent of the batch size.  Summing was
    // measured at 1.9992 times the single-sample step for a batch of two; at
    // 64 that is a 64x step and divergence.  At B = 1, dividing by one changes
    // nothing.
    if(targets.N != inputs.N)
        throw exception("train: inputs and targets disagree about the batch size");

    const uint batch = (inputs.N > 0) ? inputs.N : 1;
    const T rate = T(m_lrate / double(batch));

    reserve(batch);

    backend<T>& b = *m_backend;

    m_in->write(inputs);
    m_target->write(targets);

    forward();

    const std::size_t n = m_nhidden.size() - 1;

    b.subtract(m_target, m_out, m_err);

    // Every error first, then every update.  The propagation reads the
    // weights, so it has to finish before any of them move -- which is #130.
    // That used to be handled by copying each weight matrix before updating
    // it; ordering the passes says the same thing structurally and copies
    // nothing.
    //
    // Note the propagation is W^T e with no slope in it: the activation
    // derivative enters only in the update.  That is this network's rule
    // rather than textbook backpropagation, it predates all of this, and
    // wiring a backend underneath is not the branch to change it in.
    b.multiply_tn(m_who, m_err, m_e[n]);

    for(std::size_t i = n; i >= 1; i--)
        b.multiply_tn(m_deep[i-1], m_e[i], m_e[i-1]);

    // Updates.  multiply_nt with beta = 1 accumulates straight into the
    // weight, so a layer's update is one operation and needs no gradient
    // tensor to hold it.
    b.slope(m_output_activation, m_out, m_slope_out);
    b.hadamard(m_err, m_slope_out, m_delta_out);
    b.multiply_nt(m_delta_out, m_h[n], m_who, rate, T(1));

    for(std::size_t i = n; i >= 1; i--) {
        b.slope(m_hidden_activation, m_h[i], m_slope[i]);
        b.hadamard(m_e[i], m_slope[i], m_e[i]);
        b.multiply_nt(m_e[i], m_h[i-1], m_deep[i-1], rate, T(1));
    }

    b.slope(m_hidden_activation, m_h[0], m_slope[0]);
    b.hadamard(m_e[0], m_slope[0], m_e[0]);
    b.multiply_nt(m_e[0], m_in, m_wih, rate, T(1));

    // The one synchronisation in the step.  Everything above was encoded
    // rather than run, on a backend that defers.
    b.wait();

    return m_err->read();
}

template<typename T>
math::matrix<T> NeuralNetwork<T>::query(math::matrix<T> inputs) {
    reserve(inputs.N > 0 ? inputs.N : 1);

    m_in->write(inputs);

    forward();

    m_backend->wait();

    return m_out->read();
}

template<typename T>    
util::json::object::ptr NeuralNetwork<T>::json() {
    util::json::object::ptr p = util::json::object::create();
    
    p->add("ninput", m_ninput);
    p->add("noutput", m_noutput);
    p->add("lrate", m_lrate);

    // By name rather than by number, so a saved network stays readable and a
    // reordering of the enum cannot silently change what a file means.
    p->add("hidden_activation", name_of(m_hidden_activation));
    p->add("output_activation", name_of(m_output_activation));

    util::json::array::ptr nhidden = util::json::array::create();
    //m_nhidden.foreach([&](T& x) {
    for(uint x : m_nhidden) {
	    nhidden->add(x);
    }
    p->add("nhidden", nhidden);
    
    util::json::array::ptr wih = util::json::array::create();
    m_wih->read().foreach([&](T& x) {
            wih->add(double(x));
        });
    p->add("wih", wih);
	
    util::json::array::ptr deep = util::json::array::create();
    for(auto& d : m_deep) {
        util::json::array::ptr jd = util::json::array::create();
        d->read().foreach([&](T& x) {
                jd->add(double(x));
            });
        deep->add(jd);
    }
    p->add("deep", deep);
    
    util::json::array::ptr who = util::json::array::create();
    m_who->read().foreach([&](T& x) {
            who->add(double(x));
        });
    p->add("who", who);
	
    return p;
}

template<typename T>    
std::default_random_engine& NeuralNetwork<T>::get_generator() {
    return m_generator;
}

template<typename T>
void NeuralNetwork<T>::set_rate(double rate) {
    m_lrate = rate;
}
    
}
}
