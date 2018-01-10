/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2017 Joey Yandle <xoloki@gmail.com>
 * 
 */

#include <jlib/math/matrix.hh>
#include <jlib/util/util.hh>
#include <jlib/util/json.hh>

#include <jlib/cuda/gemm.hh>

#include <functional>
#include <random>
#include <fstream>
#include <tuple>

#include <cmath>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <cuda_runtime.h>

namespace jlib {
namespace cuda {
    
template<typename T>
class NeuralNetwork {
public:
    NeuralNetwork(T lrate, uint ninput, const std::vector<uint>& hidden, uint noutput);

    NeuralNetwork(util::json::object::ptr p);
    ~NeuralNetwork();
    
    template<typename... Args>
    NeuralNetwork(T lrate, uint ninput, uint noutput, Args&&... args)
        : NeuralNetwork(lrate, ninput, std::vector<uint>({args...}), noutput)
    {
    }

    math::matrix<T> train(math::matrix<T> inputs, math::matrix<T> targets);
    math::matrix<T> query(math::matrix<T> inputs);

    util::json::object::ptr json();

    std::default_random_engine& get_generator();

    void set_rate(T rate);
    
protected:
    void gemm(math::matrix<T> a, T* ad, math::matrix<T> b, T* bd, math::matrix<T> c, T* cd);
    void gemm(math::matrix<T> a, math::matrix<T> b, math::matrix<T> c);
    void gemm(cublasOperation_t tra, cublasOperation_t trb, T alpha, math::matrix<T> a, math::matrix<T> b, T beta, math::matrix<T> c);
    
    void init_cublas();
    
    uint m_ninput;
    std::vector<uint> m_nhidden;
    uint m_noutput;
    T m_lrate;
    math::matrix<T> m_wih;
    math::matrix<T> m_who;
    std::vector<math::matrix<T>> m_deep;
    std::function<math::matrix<T>(math::matrix<T>)> m_activation_function;
    std::default_random_engine m_generator;
    cublasHandle_t m_handle;
    T* md_wih;
    T* md_inputs;
    T* md_hidden_inputs;
    T* md_who;
    T* md_deep_outputs;
    T* md_final_inputs;
    cublas::gemm<T> m_gemm;
};

template<typename T>
NeuralNetwork<T>::NeuralNetwork(T lrate, uint ninput, const std::vector<uint>& hidden, uint noutput)
    : m_ninput(ninput),
      m_noutput(noutput),
      m_lrate(lrate),
      m_nhidden(hidden),
      m_wih(m_nhidden.front(), m_ninput),
      m_who(m_noutput, m_nhidden.back())
{
    if(m_nhidden.empty())
        throw std::runtime_error("Need at least one hidden layer");
    
    T hbound = pow(m_nhidden[0], -0.5);
    std::uniform_real_distribution<T> hdist(-hbound, hbound);
        
    m_wih.foreach([&](T& x) {
            x = hdist(m_generator);
        });
        
    T obound = pow(m_noutput, -0.5);
    std::uniform_real_distribution<T> odist(-obound, obound);
        
    m_who.foreach([&](T& x) {
            x = odist(m_generator);
        });

    for(int i = 1; i < m_nhidden.size(); i++) {
        // add a deep matrix from [i-1] to [i]
	T dbound = pow(m_nhidden[i], -0.5);
	std::uniform_real_distribution<T> ddist(-dbound, dbound);
	
	m_deep.push_back(math::matrix<T>(m_nhidden[i], m_nhidden[i-1]));
	m_deep.back().foreach([&](T& x) {
                x = ddist(m_generator);
            });
    }

    // sigmoid function
    m_activation_function = [](math::matrix<T> input) {
        math::matrix<T> output(input.M, input.N);
        input.foreach_index([&](uint r, uint c, T& val) {
                output(r, c) = (1.0 / (1.0 + exp(-val))); //tanh(val);
            });
        return output;
    };

    init_cublas();
}
    
template<typename T>
NeuralNetwork<T>::NeuralNetwork(util::json::object::ptr p)
    : m_ninput(p->get("ninput")),
      m_noutput(p->get("noutput")),
      m_lrate(p->get("lrate")),
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
    
    // sigmoid function
    m_activation_function = [](math::matrix<T> input) {
        math::matrix<T> output(input.M, input.N);
        input.foreach_index([&](uint r, uint c, T& val) {
                output(r, c) = (1.0 / (1.0 + exp(-val))); //tanh(val);
            });
        return output;
    };

    init_cublas();
}

template<typename T>
NeuralNetwork<T>::~NeuralNetwork() {
    cublasDestroy(m_handle);
    cuda::free (md_wih);
    cuda::free (md_inputs);
    cuda::free (md_hidden_inputs);
    cuda::free (md_who);
    cuda::free (md_deep_outputs);
    cuda::free (md_final_inputs);
}

template<typename T>
void NeuralNetwork<T>::init_cublas() {
    cublasStatus_t stat = cublasCreate(&m_handle);
    if (stat != CUBLAS_STATUS_SUCCESS) {
	throw std::runtime_error ("CUBLAS initialization failed\n");
    }

    md_wih = (T*)cuda::malloc(m_wih.M * m_wih.N * sizeof(T));
    md_inputs = (T*)cuda::malloc(m_ninput * sizeof(T));
    md_hidden_inputs = (T*)cuda::malloc(m_wih.M * 1 * sizeof(T));
    md_who = (T*)cuda::malloc(m_who.M * m_who.N * sizeof(T));
    md_deep_outputs = (T*)cuda::malloc(m_wih.M * sizeof(T));
    md_final_inputs = (T*)cuda::malloc(m_who.M * 1 * sizeof(T));
}

template<typename T>
void NeuralNetwork<T>::gemm(math::matrix<T> a, T* ad, math::matrix<T> b, T* bd, math::matrix<T> c, T* cd) {
    const T alpha = 1, beta = 0;

    m_gemm.set_matrix(a, ad);
    m_gemm.set_matrix(b, bd);
    m_gemm.set_matrix(c, cd);

    m_gemm(m_handle, CUBLAS_OP_N, CUBLAS_OP_N, alpha, a, ad, b, bd, beta, c, cd);
}

template<typename T>
void NeuralNetwork<T>::gemm(cublasOperation_t tra, cublasOperation_t trb, T alpha, math::matrix<T> a, math::matrix<T> b, T beta, math::matrix<T> c) {
    cublasStatus_t stat;

    T* ad = m_gemm.set_matrix(a);
    T* bd = m_gemm.set_matrix(b);
    T* cd = m_gemm.set_matrix(c);

    m_gemm(m_handle, tra, trb, alpha, a, ad, b, bd, beta, c, cd);

    cuda::free (ad);
    cuda::free (bd);
    cuda::free (cd);
}
    
template<typename T>
void NeuralNetwork<T>::gemm(math::matrix<T> a, math::matrix<T> b, math::matrix<T> c) {
    gemm(CUBLAS_OP_N, CUBLAS_OP_N, 1, a, b, 0, c);
}
    
template<typename T>
math::matrix<T> NeuralNetwork<T>::train(math::matrix<T> inputs, math::matrix<T> targets){
    //std::cout << "wih[" << m_wih.M << "," << m_wih.N << "]" << " * " << "inputs[" << inputs.M << "," << inputs.N << "]" << std::endl;

    //math::matrix<T> hidden_inputs = m_wih * inputs;
    math::matrix<T> hidden_inputs(m_wih.M, inputs.N);
    gemm(m_wih, md_wih, inputs, md_inputs, hidden_inputs, md_hidden_inputs);
    //gemm(m_wih, inputs, hidden_inputs);
    
    math::matrix<T> hidden_outputs = m_activation_function(hidden_inputs);

    math::matrix<T> deep_inputs = hidden_inputs;
    math::matrix<T> deep_outputs = hidden_outputs;

    std::vector<math::matrix<T>> deep_inputs_cache;
    std::vector<math::matrix<T>> deep_outputs_cache;

    for(auto& deep : m_deep) {
        deep_inputs_cache.push_back(deep_outputs);

        //deep_inputs = deep * deep_outputs;
	deep_inputs = math::matrix<T>(deep.M, deep_outputs.N);
	gemm(deep, deep_outputs, deep_inputs);
	
        deep_outputs = m_activation_function(deep_inputs);

        deep_outputs_cache.push_back(deep_outputs);
    }

    //math::matrix<T> final_inputs = m_who * deep_outputs;
    math::matrix<T> final_inputs(m_who.M, deep_outputs.N);
    gemm(m_who, md_who, deep_outputs, md_deep_outputs, final_inputs, md_final_inputs);
    //gemm(m_who, deep_outputs, final_inputs);
    
    math::matrix<T> final_outputs = m_activation_function(final_inputs);
    //std::cout << "final_outputs[" << final_outputs.M << "," << final_outputs.N << "] \n" << final_outputs << std::endl;
    
    math::matrix<T> output_errors = targets - final_outputs;
    //std::cout << "output_errors[" << output_errors.M << "," << output_errors.N << "] \n" << output_errors << std::endl;

    //m_who += m_lrate * (((output_errors ^ final_outputs ^ (1.0 - final_outputs)) * deep_outputs.transpose()));
    math::matrix<T> tmp = (output_errors ^ final_outputs ^ ((T)1.0 - final_outputs));
    gemm(CUBLAS_OP_N, CUBLAS_OP_T, m_lrate, tmp, deep_outputs, 1, m_who);

    math::matrix<T> deep_errors = output_errors;
    math::matrix<T> deep = m_who;
    
    //for(auto x = m_deep.rbegin(); x != m_deep.rend(); x++) {
    for(int i = m_deep.size() - 1; i >= 0; i--) {
        //deep_errors = deep.transpose() * deep_errors;
	math::matrix<T> deep_errors_tmp(deep.transpose().M, deep_errors.N);
	gemm(CUBLAS_OP_T, CUBLAS_OP_N, 1, deep, deep_errors, 0, deep_errors_tmp);
	deep_errors = deep_errors_tmp;
	
        //m_deep[i] += m_lrate * (((deep_errors ^ deep_outputs_cache[i] ^ (1.0 - deep_outputs_cache[i])) * deep_inputs_cache[i].transpose()));
	tmp = (deep_errors ^ deep_outputs_cache[i] ^ ((T)1.0 - deep_outputs_cache[i]));
	gemm(CUBLAS_OP_N, CUBLAS_OP_T, m_lrate, tmp, deep_inputs_cache[i], 1, m_deep[i]);
        deep = m_deep[i];
    }

    //math::matrix<T> hidden_errors = deep.transpose() * deep_errors;
    math::matrix<T> hidden_errors(deep.transpose().M, deep_errors.N);
    gemm(CUBLAS_OP_T, CUBLAS_OP_N, 1, deep, deep_errors, 0, hidden_errors);
    
    //std::cout << "hidden_errors[" << hidden_errors.M << "," << hidden_errors.N << "] \n" << hidden_errors << std::endl;

    //m_wih += m_lrate * ((hidden_errors ^ hidden_outputs ^ (1.0 - hidden_outputs)) * inputs.transpose());
    tmp = (hidden_errors ^ hidden_outputs ^ ((T)1.0 - hidden_outputs));
    gemm(CUBLAS_OP_N, CUBLAS_OP_T, m_lrate, tmp, inputs, 1, m_wih);

    return output_errors;
}
    
template<typename T>
math::matrix<T> NeuralNetwork<T>::query(math::matrix<T> inputs) {
    //math::matrix<T> hidden_inputs = m_wih * inputs;
    math::matrix<T> hidden_inputs(m_wih.M, inputs.N);
    gemm(m_wih, md_wih, inputs, md_inputs, hidden_inputs, md_hidden_inputs);

    math::matrix<T> hidden_outputs = m_activation_function(hidden_inputs);

    for(auto& deep : m_deep) {
        //hidden_inputs = deep * hidden_outputs;
	hidden_inputs = math::matrix<T>(deep.M, hidden_outputs.N);
	gemm(deep, hidden_outputs, hidden_inputs);
        hidden_outputs = m_activation_function(hidden_inputs);
    }

    //math::matrix<T> final_inputs = m_who * hidden_outputs;
    math::matrix<T> final_inputs(m_who.M, hidden_outputs.N);
    gemm(m_who, md_who, hidden_outputs, md_deep_outputs, final_inputs, md_final_inputs);

    math::matrix<T> final_outputs = m_activation_function(final_inputs);
    
    return final_outputs;
}

template<typename T>    
util::json::object::ptr NeuralNetwork<T>::json() {
    util::json::object::ptr p = util::json::object::create();
    
    p->add("ninput", m_ninput);
    p->add("noutput", m_noutput);
    p->add("lrate", m_lrate);

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
void NeuralNetwork<T>::set_rate(T rate) {
    m_lrate = rate;
}
    
}
}
