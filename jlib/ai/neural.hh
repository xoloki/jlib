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

/**
 * Which nonlinearity a layer applies.
 *
 * Every one of these has a derivative expressible in terms of its own
 * *output*, which is the fact that makes them interchangeable here: train()
 * already caches each layer's output to build `out * (1 - out)`, so nothing
 * needs the pre-activation kept as well.
 *
 *   sigmoid      s (1 - s)
 *   tanh         1 - s^2
 *   relu         s > 0 ? 1 : 0          -- output > 0 exactly when input was
 *   leaky_relu   s > 0 ? 1 : LEAK       -- likewise, since LEAK is positive
 */
enum class activation { sigmoid, tanh, relu, leaky_relu };

/** The slope a leaky ReLU keeps below zero. */
inline constexpr double LEAK = 0.01;

inline std::string name_of(activation a) {
    switch(a) {
    case activation::sigmoid:    return "sigmoid";
    case activation::tanh:       return "tanh";
    case activation::relu:       return "relu";
    case activation::leaky_relu: return "leaky_relu";
    }

    return "sigmoid";
}

inline activation activation_from_name(const std::string& s) {
    if(s == "sigmoid")    return activation::sigmoid;
    if(s == "tanh")       return activation::tanh;
    if(s == "relu")       return activation::relu;
    if(s == "leaky_relu") return activation::leaky_relu;

    throw std::runtime_error("unknown activation '" + s + "'");
}

template<typename T>
math::matrix<T> activate(activation a, const math::matrix<T>& in) {
    math::matrix<T> out(in.M, in.N);

    for(uint r = 0; r < in.M; r++) {
        for(uint c = 0; c < in.N; c++) {
            const T x = in(r, c);

            switch(a) {
            case activation::sigmoid:    out(r,c) = T(1) / (T(1) + std::exp(-x)); break;
            case activation::tanh:       out(r,c) = std::tanh(x);                 break;
            case activation::relu:       out(r,c) = (x > 0) ? x : T(0);           break;
            case activation::leaky_relu: out(r,c) = (x > 0) ? x : T(LEAK) * x;    break;
            }
        }
    }

    return out;
}

/** The derivative, from the output rather than the input.  See activation. */
template<typename T>
math::matrix<T> activate_slope(activation a, const math::matrix<T>& out) {
    math::matrix<T> d(out.M, out.N);

    for(uint r = 0; r < out.M; r++) {
        for(uint c = 0; c < out.N; c++) {
            const T s = out(r, c);

            switch(a) {
            case activation::sigmoid:    d(r,c) = s * (T(1) - s);        break;
            case activation::tanh:       d(r,c) = T(1) - s * s;          break;
            case activation::relu:       d(r,c) = (s > 0) ? T(1) : T(0); break;
            case activation::leaky_relu: d(r,c) = (s > 0) ? T(1) : T(LEAK); break;
            }
        }
    }

    return d;
}

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

    /** How two matrices are multiplied.  See set_multiply(). */
    typedef std::function<math::matrix<T>(const math::matrix<T>&,
                                          const math::matrix<T>&)> multiply_type;

    /**
     * Replace the matrix multiply with somebody else's.
     *
     * Everything expensive here is a GEMM, so this is the whole of what a GPU
     * would want to take over.  Injected rather than depended on: jlib/ai must
     * not need jlib/metal -- it builds before it, and on a machine with no
     * Metal it does not build at all -- so the *caller* supplies one.
     *
     *     metal::matrix_multiply mm;
     *     nn.set_multiply([&mm](const auto& a, const auto& b) { return mm(a, b); });
     *
     * Only the matrix-by-matrix products go through this.  The elementwise
     * work -- the Hadamard terms, the scalar rate -- stays where it is: it is
     * a fraction of the arithmetic and all of the awkwardness.
     *
     * **Batch first.**  A GPU loses on this network at a batch of one: the
     * dispatch alone is about 0.35ms against 0.11ms to do the whole multiply
     * on the CPU.  At a batch of 64 it wins by 9.7x and at 256 by 27x.
     * Setting this without batching makes training slower.
     */
    void set_multiply(multiply_type m) {
        m_multiply = m ? m : default_multiply();
    }

    static multiply_type default_multiply() {
        return [](const math::matrix<T>& a, const math::matrix<T>& b) {
            return a * b;
        };
    }
    
protected:
    uint m_ninput;
    std::vector<uint> m_nhidden;
    uint m_noutput;
    double m_lrate;
    math::matrix<T> m_wih;
    math::matrix<T> m_who;
    std::vector<math::matrix<T>> m_deep;
    // Was a std::function holding a sigmoid, with the matching derivative
    // written out by hand in train() as `out ^ (1 - out)`.  The function was
    // replaceable and the derivative was not, so replacing it silently trained
    // against the wrong slope.  A pair of enums keeps them together and
    // serialises, which a lambda could not.
    activation m_hidden_activation = activation::sigmoid;
    activation m_output_activation = activation::sigmoid;

    multiply_type m_multiply = default_multiply();
    std::default_random_engine m_generator;
};

template<typename T>
NeuralNetwork<T>::NeuralNetwork(double lrate, uint ninput, const std::vector<uint>& hidden, uint noutput)
    : m_ninput(ninput),
      m_noutput(noutput),
      m_lrate(lrate),
      m_nhidden(hidden),
      m_wih(m_nhidden.front(), m_ninput),
      m_who(m_noutput, m_nhidden.back())
{
    if(m_nhidden.empty())
        throw std::runtime_error("Need at least one hidden layer");
    
    // Fan-in, not fan-out.  1/sqrt(fan) is the right heuristic and was always
    // the intent, but each of the three below passed the wrong dimension:
    // m_wih is (hidden x input), so the number of weights feeding each hidden
    // unit is m_ninput, not m_nhidden[0].  See #131 -- with 784 inputs and 200
    // hidden this was twice as wide as it should be, and m_who below was four
    // and a half times.
    //
    // Weights that are too large push the sigmoids toward their flat ends,
    // where the derivative approaches zero, which starves the backward pass on
    // top of the attenuation it already suffers per layer.
    T hbound = pow(m_ninput, -0.5);
    std::uniform_real_distribution<T> hdist(-hbound, hbound);
        
    m_wih.foreach([&](T& x) {
            x = hdist(m_generator);
        });
        
    // m_who is (output x hidden): the fan-in is the hidden layer feeding it,
    // not the number of classes coming out.  This was the worst of the three
    // and the one the backward pass starts from.
    T obound = pow(m_nhidden.back(), -0.5);
    std::uniform_real_distribution<T> odist(-obound, obound);
        
    m_who.foreach([&](T& x) {
            x = odist(m_generator);
        });

    for(int i = 1; i < m_nhidden.size(); i++) {
        // add a deep matrix from [i-1] to [i]
	// m_deep[i-1] is (n[i] x n[i-1]), so the fan-in is the layer below.
	// Identical to the old expression when every hidden layer is the same
	// width, which is why the case people try first is the case this got
	// right.
	T dbound = pow(m_nhidden[i-1], -0.5);
	std::uniform_real_distribution<T> ddist(-dbound, dbound);
	
	m_deep.push_back(math::matrix<T>(m_nhidden[i], m_nhidden[i-1]));
	m_deep.back().foreach([&](T& x) {
                x = ddist(m_generator);
            });
    }

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
      m_wih(1, 1),
      m_who(1, 1)
{
    util::json::object::ptr nh = p->obj("nhidden");

    if(nh->is(util::json::object::type_array)) {
        std::vector<uint> nhidden;
        for(int i = 0; i < nh->size(); i++) {
            m_nhidden.push_back(nh->get(i));
        }
    } else if(nh->is(util::json::object::type_int)) {
        m_nhidden.push_back(p->get("nhidden"));
    } else {
        throw std::runtime_error("Unknown type of nhidden field: " + nh->str());
    }
          
    if(m_nhidden.empty())
        throw std::runtime_error("Need at least one hidden layer");

    m_wih = math::matrix<T>(m_nhidden.front(), m_ninput);
    m_wih.foreach_index([&](uint r, uint c, T& x) {
            x = p->obj("wih")->get(r*m_ninput + c);
        });

    for(int i = 1; i < m_nhidden.size(); i++) {
        // add a deep matrix from [i-1] to [i]
	m_deep.push_back(math::matrix<T>(m_nhidden[i], m_nhidden[i-1]));
	m_deep.back().foreach_index([&](uint r, uint c, T& x) {
		x = p->obj("deep")->obj(i-1)->get(r*m_nhidden[i-1] + c);
            });
    }

    m_who = math::matrix<T>(m_noutput, m_nhidden.back());
    m_who.foreach_index([&](uint r, uint c, T& x) {
            x = p->obj("who")->get(r*m_nhidden.back() + c);
        });
    
}
    
template<typename T>
math::matrix<T> NeuralNetwork<T>::train(math::matrix<T> inputs, math::matrix<T> targets){
    // A column per sample.  Every step below is already written in matrix
    // terms that carry a batch: m_wih * inputs is (H x I)(I x B) = H x B, the
    // Hadamard terms stay elementwise, and multiplying by a transpose
    // contracts the batch away to leave a gradient of the weight's own shape.
    // So this took a batch before anything here was changed -- it just summed
    // the gradient over one.
    //
    // Summing makes the step size scale with the batch, which was measured:
    // a batch of two identical samples moved the weights 1.9992 times as far
    // as one sample, and as far as two sequential steps. At a batch of 64 that
    // is a 64x step and divergence.
    //
    // So the gradient is a mean, which is what makes the learning rate mean
    // something independent of the batch size. At B = 1 -- every caller in the
    // tree today -- dividing by one changes nothing, so this is exactly
    // backward compatible.
    const std::size_t batch = (inputs.N > 0) ? inputs.N : 1;

    // In T, not double: the scalar-times-matrix operator takes both in the
    // matrix's own type, so a double rate against a matrix<float> does not
    // compile.  The division stays in double so a small rate over a large
    // batch does not lose digits before it is narrowed.
    const T rate = static_cast<T>(m_lrate / double(batch));

    if(targets.N != inputs.N)
        throw exception("train: inputs and targets disagree about the batch size");

    //std::cout << "wih[" << m_wih.M << "," << m_wih.N << "]" << " * " << "inputs[" << inputs.M << "," << inputs.N << "]" << std::endl;

    math::matrix<T> hidden_inputs = m_multiply(m_wih, inputs);
    math::matrix<T> hidden_outputs = activate(m_hidden_activation, hidden_inputs);

    math::matrix<T> deep_inputs = hidden_inputs;
    math::matrix<T> deep_outputs = hidden_outputs;

    std::vector<math::matrix<T>> deep_inputs_cache;
    std::vector<math::matrix<T>> deep_outputs_cache;

    for(auto& deep : m_deep) {
        deep_inputs_cache.push_back(deep_outputs);

        deep_inputs = m_multiply(deep, deep_outputs);
        deep_outputs = activate(m_hidden_activation, deep_inputs);

        deep_outputs_cache.push_back(deep_outputs);
    }

    math::matrix<T> final_inputs = m_multiply(m_who, deep_outputs);
    math::matrix<T> final_outputs = activate(m_output_activation, final_inputs);
    //std::cout << "final_outputs[" << final_outputs.M << "," << final_outputs.N << "] \n" << final_outputs << std::endl;
    
    math::matrix<T> output_errors = targets - final_outputs;
    //std::cout << "output_errors[" << output_errors.M << "," << output_errors.N << "] \n" << output_errors << std::endl;

    // Captured before the update, not after.  Closes #130.
    //
    // This read m_who *after* adding its own step, so the error arriving at
    // every layer below had been propagated through weights that already
    // moved -- and the deep loop repeated it, once per layer.
    // Backpropagation evaluates every gradient at the weights the step
    // started from and applies them afterwards.
    //
    // It was first measured on a one-hidden-layer network, where the effect
    // is about 0.1% of a step, and dismissed on that basis.  That was the
    // wrong measurement: an error that compounds once per layer compounds
    // least at one layer.  At three and four it is the difference between
    // learning and not.
    math::matrix<T> deep = m_who;

    m_who += rate * m_multiply(output_errors ^ activate_slope(m_output_activation, final_outputs),
                               deep_outputs.transpose());

    math::matrix<T> deep_errors = output_errors;

    for(int i = m_deep.size() - 1; i >= 0; i--) {
        deep_errors = m_multiply(deep.transpose(), deep_errors);

        math::matrix<T> before = m_deep[i];

        m_deep[i] += rate * m_multiply(deep_errors ^ activate_slope(m_hidden_activation, deep_outputs_cache[i]),
                                       deep_inputs_cache[i].transpose());
        deep = before;
    }

    math::matrix<T> hidden_errors = m_multiply(deep.transpose(), deep_errors);
    //std::cout << "hidden_errors[" << hidden_errors.M << "," << hidden_errors.N << "] \n" << hidden_errors << std::endl;
    
    m_wih += rate * m_multiply(hidden_errors ^ activate_slope(m_hidden_activation, hidden_outputs),
                               inputs.transpose());

    return output_errors;
}
    
template<typename T>
math::matrix<T> NeuralNetwork<T>::query(math::matrix<T> inputs) {
    math::matrix<T> hidden_inputs = m_multiply(m_wih, inputs);
    math::matrix<T> hidden_outputs = activate(m_hidden_activation, hidden_inputs);

    for(auto& deep : m_deep) {
        hidden_inputs = m_multiply(deep, hidden_outputs);
        hidden_outputs = activate(m_hidden_activation, hidden_inputs);
    }

    math::matrix<T> final_inputs = m_multiply(m_who, hidden_outputs);
    math::matrix<T> final_outputs = activate(m_output_activation, final_inputs);
    
    return final_outputs;
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
    m_wih.foreach([&](T& x) {
            wih->add(x);
        });
    p->add("wih", wih);
	
    util::json::array::ptr deep = util::json::array::create();
    for(auto& d : m_deep) {
        util::json::array::ptr jd = util::json::array::create();
        d.foreach([&](T& x) {
                jd->add(x);
            });
        deep->add(jd);
    }
    p->add("deep", deep);
    
    util::json::array::ptr who = util::json::array::create();
    m_who.foreach([&](T& x) {
            who->add(x);
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
