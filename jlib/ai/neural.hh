/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2017 Joey Yandle <xoloki@gmail.com>
 * 
 */

#include <jlib/math/matrix.hh>
#include <jlib/util/util.hh>
#include <jlib/util/json.hh>

#include <functional>
#include <random>
#include <fstream>
#include <tuple>

#include <cmath>

namespace jlib {
namespace ai {

template<typename T>
class NeuralNetwork {
public:
    NeuralNetwork(util::json::object::ptr p);
    NeuralNetwork(double lrate, uint ninput, uint noutput, uint nhidden);

    void train(math::matrix<T> inputs, math::matrix<T> targets);
    math::matrix<T> query(math::matrix<T> inputs);

    util::json::object::ptr json();

    std::default_random_engine& get_generator();
    
protected:
    int m_ninput;
    int m_nhidden;
    int m_noutput;
    double m_lrate;
    math::matrix<T> m_wih;
    math::matrix<T> m_who;
    std::vector<math::matrix<T>> m_deep;
    std::function<math::matrix<T>(math::matrix<T>)> m_activation_function;
    std::default_random_engine m_generator;
};

template<typename T>
NeuralNetwork<T>::NeuralNetwork(util::json::object::ptr p)
    : NeuralNetwork(p->get("lrate"), p->get("ninput"), p->get("noutput"), p->get("nhidden"))
{
    m_wih.foreach_index([&](uint r, uint c, T& x) {
            x = p->obj("wih")->get(r*m_ninput + c);
        });
    
    m_who.foreach_index([&](uint r, uint c, T& x) {
            x = p->obj("who")->get(r*m_nhidden + c);
        });
}
    
template<typename T>
NeuralNetwork<T>::NeuralNetwork(double lrate, uint ninput, uint noutput, uint nhidden)
    : m_ninput(ninput),
      m_nhidden(nhidden),
      m_noutput(noutput),
      m_lrate(lrate),
      m_wih(m_nhidden, m_ninput),
      m_who(m_noutput, m_nhidden)
{
    T hbound = pow(m_nhidden, -0.5);
    std::uniform_real_distribution<T> hdist(-hbound, hbound);
    
    m_wih.foreach([&](T& x) {
            x = hdist(m_generator);
        });
    
    T obound = pow(m_noutput, -0.5);
    std::uniform_real_distribution<T> odist(-obound, obound);
    
    m_who.foreach([&](T& x) {
            x = odist(m_generator);
        });
    
    // sigmoid function
    m_activation_function = [](math::matrix<T> input) {
        math::matrix<T> output(input.M, input.N);
        input.foreach_index([&](uint r, uint c, T& val) {
                output(r, c) = (1.0 / (1.0 + exp(-val))); //tanh(val);
            });
        return output;
    }; 
}

template<typename T>
void NeuralNetwork<T>::train(math::matrix<T> inputs, math::matrix<T> targets){
    math::matrix<T> hidden_inputs = m_wih * inputs;
    math::matrix<T> hidden_outputs = m_activation_function(hidden_inputs);
    math::matrix<T> final_inputs = m_who * hidden_outputs;
    math::matrix<T> final_outputs = m_activation_function(final_inputs);
    //std::cout << "final_outputs[" << final_outputs.M << "," << final_outputs.N << "] \n" << final_outputs << std::endl;
    
    math::matrix<T> output_errors = targets - final_outputs;
    //std::cout << "output_errors[" << output_errors.M << "," << output_errors.N << "] \n" << output_errors << std::endl;
    
    math::matrix<T> hidden_errors = m_who.transpose() * output_errors;
    //std::cout << "hidden_errors[" << hidden_errors.M << "," << hidden_errors.N << "] \n" << hidden_errors << std::endl;
    
    m_who += m_lrate * (((output_errors ^ final_outputs ^ (1.0 - final_outputs)) * hidden_outputs.transpose()));
    m_wih += m_lrate * ((hidden_errors ^ hidden_outputs ^ (1.0 - hidden_outputs)) * inputs.transpose());
}
    
template<typename T>
math::matrix<T> NeuralNetwork<T>::query(math::matrix<T> inputs) {
    math::matrix<T> hidden_inputs = m_wih * inputs;
    math::matrix<T> hidden_outputs = m_activation_function(hidden_inputs);
    math::matrix<T> final_inputs = m_who * hidden_outputs;
    math::matrix<T> final_outputs = m_activation_function(final_inputs);
    
    return final_outputs;
}

template<typename T>    
util::json::object::ptr NeuralNetwork<T>::json() {
    util::json::object::ptr p = util::json::object::create();
    
    p->add("ninput", m_ninput);
    p->add("nhidden", m_nhidden);
    p->add("noutput", m_noutput);
    p->add("lrate", m_lrate);
    
    util::json::array::ptr wih = util::json::array::create();
    m_wih.foreach([&](T& x) {
            wih->add(x);
        });
    p->add("wih", wih);
	
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

}
}
