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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <cuda_runtime.h>
#include "cublas_v2.h"


namespace jlib {
namespace ai {

template<typename T>
struct gemm {
    void operator()(cublasHandle_t m_handle, cublasOperation_t tra, cublasOperation_t trb, T alpha, math::matrix<T> a, const T* da, math::matrix<T> b, const T* db, T beta, math::matrix<T> c, T* dc) {}
};

template<>
struct gemm<float> {
    void operator()(cublasHandle_t m_handle, cublasOperation_t tra, cublasOperation_t trb, float alpha, math::matrix<float> a, const float* ad, math::matrix<float> b, const float* bd, float beta, math::matrix<float> c, float* cd) {
	cublasStatus_t stat;
	uint k = (tra == CUBLAS_OP_N ? a.N : a.M);
	
	stat = cublasSgemm (m_handle, tra, trb, c.M, c.N, k, &alpha, ad, a.M, bd, b.M, &beta, cd, c.M);
	if (stat != CUBLAS_STATUS_SUCCESS) {
	    throw std::runtime_error ("CUBLAS multiplication failed\n");
	}
	
	math::buffer<float> cb = c;
	stat = cublasGetMatrix (c.M, c.N, sizeof(float), cd, c.M, cb.data(), c.M);
	if (stat != CUBLAS_STATUS_SUCCESS) {
	    throw std::runtime_error  ("data upload failed");
	}
    }
};

template<>
struct gemm<double> {
    void operator()(cublasHandle_t m_handle, cublasOperation_t tra, cublasOperation_t trb, double alpha, math::matrix<double> a, const double* ad, math::matrix<double> b, const double* bd, double beta, math::matrix<double> c, double* cd) {
	cublasStatus_t stat;
	uint k = (tra == CUBLAS_OP_N ? a.N : a.M);
	
	stat = cublasDgemm (m_handle, tra, trb, c.M, c.N, k, &alpha, ad, a.M, bd, b.M, &beta, cd, c.M);
	if (stat != CUBLAS_STATUS_SUCCESS) {
	    throw std::runtime_error ("CUBLAS multiplication failed\n");
	}
	
	math::buffer<double> cb = c;
	stat = cublasGetMatrix (c.M, c.N, sizeof(double), cd, c.M, cb.data(), c.M);
	if (stat != CUBLAS_STATUS_SUCCESS) {
	    std::ostringstream os;
	    os << "data upload failed: " << (int)stat;
	    throw std::runtime_error(os.str());
	}
    }
};

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

    void train(math::matrix<T> inputs, math::matrix<T> targets);
    math::matrix<T> query(math::matrix<T> inputs);

    util::json::object::ptr json();

    std::default_random_engine& get_generator();

    void set_rate(T rate);
    
protected:
    static void* cuda_malloc(std::size_t n);
    static T* set_matrix(math::matrix<T> m);
    static void set_matrix(math::matrix<T> m, T* ret);
    
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
    jlib::ai::gemm<T> m_gemm;
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
    cudaFree (md_wih);
    cudaFree (md_inputs);
    cudaFree (md_hidden_inputs);
    cudaFree (md_who);
    cudaFree (md_deep_outputs);
    cudaFree (md_final_inputs);
}

template<typename T>
void NeuralNetwork<T>::init_cublas() {
    cublasStatus_t stat = cublasCreate(&m_handle);
    if (stat != CUBLAS_STATUS_SUCCESS) {
	throw std::runtime_error ("CUBLAS initialization failed\n");
    }

    md_wih = (T*)cuda_malloc(m_wih.M * m_wih.N * sizeof(T));
    md_inputs = (T*)cuda_malloc(m_ninput * sizeof(T));
    md_hidden_inputs = (T*)cuda_malloc(m_wih.M * 1 * sizeof(T));
    md_who = (T*)cuda_malloc(m_who.M * m_who.N * sizeof(T));
    md_deep_outputs = (T*)cuda_malloc(m_wih.M * sizeof(T));
    md_final_inputs = (T*)cuda_malloc(m_who.M * 1 * sizeof(T));
}

template<typename T>
void* NeuralNetwork<T>::cuda_malloc(std::size_t n) {
    void* ret = nullptr;
    cudaError_t err = cudaMalloc (&ret, n);
    if (err != cudaSuccess) {
	throw std::runtime_error ("device memory allocation failed");
    }
    return ret;
}

template<typename T>
T* NeuralNetwork<T>::set_matrix(math::matrix<T> m) {
    T* ret = (T*)cuda_malloc(m.M * m.N * sizeof(T));
    set_matrix(m, ret);
    return ret;
}
    
template<typename T>
void NeuralNetwork<T>::set_matrix(math::matrix<T> m, T* ret) {
    math::buffer<T> buffer = m;
    
    cublasStatus_t stat = cublasSetMatrix (m.M, m.N, sizeof(T), buffer.data(), m.M, ret, m.M);

    if (stat != CUBLAS_STATUS_SUCCESS) {
	throw std::runtime_error ("data download failed");
    }
}
    
template<typename T>
void NeuralNetwork<T>::gemm(math::matrix<T> a, T* ad, math::matrix<T> b, T* bd, math::matrix<T> c, T* cd) {
    const T alpha = 1, beta = 0;

    set_matrix(a, ad);
    set_matrix(b, bd);
    set_matrix(c, cd);

    m_gemm(m_handle, CUBLAS_OP_N, CUBLAS_OP_N, alpha, a, ad, b, bd, beta, c, cd);
}

template<typename T>
void NeuralNetwork<T>::gemm(cublasOperation_t tra, cublasOperation_t trb, T alpha, math::matrix<T> a, math::matrix<T> b, T beta, math::matrix<T> c) {
    cublasStatus_t stat;

    T* ad = set_matrix(a);
    T* bd = set_matrix(b);
    T* cd = set_matrix(c);

    m_gemm(m_handle, tra, trb, alpha, a, ad, b, bd, beta, c, cd);

    cudaFree (ad);
    cudaFree (bd);
    cudaFree (cd);
}
    
template<typename T>
void NeuralNetwork<T>::gemm(math::matrix<T> a, math::matrix<T> b, math::matrix<T> c) {
    gemm(CUBLAS_OP_N, CUBLAS_OP_N, 1, a, b, 0, c);
}
    
template<typename T>
void NeuralNetwork<T>::train(math::matrix<T> inputs, math::matrix<T> targets){
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
	math::matrix<T> deep_errors(deep.transpose().M, deep_errors.N);
	gemm(CUBLAS_OP_T, CUBLAS_OP_N, 1, deep, deep_errors, 0, deep_errors);
	
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


#include <jlib/math/matrix.hh>
#include <jlib/util/util.hh>
#include <jlib/util/json.hh>
#include <jlib/sys/Directory.hh>
#include <jlib/sys/sys.hh>

#include <functional>
#include <random>
#include <mutex>
#include <fstream>
#include <tuple>

#include <cmath>

#include <Magick++.h>

std::default_random_engine generator;

using namespace jlib;
using namespace jlib::util;

typedef float T;

math::matrix<T> load(std::string path, uint r, uint c, bool greyscale = true);
char convert(int n);
int convert(char c);
char capitalize(char c);

std::tuple<uint,T> getmax(math::matrix<T> m);

int main(int argc, char** argv) {
    uint R = 90;
    uint C = 120;
    std::vector<uint> HNODES;
    int ONODES = 62;
    const std::string S = "Sample";
    const std::string I = "img";
    uint epochs = 1, train_multi = 1;
    std::string train_path, test_train_path, test_my_path, load_file, output_file, train_mnist_path, test_mnist_path;
    T train_rate = 0.1;
    int train_decay = -1;
    
    for(int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if(arg == "--train-path") {
            train_path = argv[++i];
        } else if(arg == "--train-epochs") {
            epochs = util::int_value(argv[++i]);
        } else if(arg == "--train-multi") {
            train_multi = util::int_value(argv[++i]);
        } else if(arg == "--train-rate") {
	    std::istringstream is(argv[++i]);
            is >> train_rate;
        } else if(arg == "--train-decay") {
            train_decay = std::stoi(argv[++i]);
        } else if(arg == "--test-train-path") {
            test_train_path = argv[++i];
        } else if(arg == "--train-mnist-path") {
            train_mnist_path = argv[++i];
        } else if(arg == "--test-mnist-path") {
            test_mnist_path = argv[++i];
        } else if(arg == "--test-my-path") {
            test_my_path = argv[++i];
        } else if(arg == "--load-file") {
            load_file = argv[++i];
        } else if(arg == "--output-file") {
            output_file = argv[++i];
        } else if(arg == "--hidden-nodes") {
            HNODES.push_back(std::stoi(argv[++i]));
        } else if(arg == "--output-nodes") {
            ONODES = util::int_value(argv[++i]);
        } else if(arg == "--image-rows" || arg == "-r") {
            R = util::int_value(argv[++i]);
        } else if(arg == "--image-cols" || arg == "-c") {
            C = util::int_value(argv[++i]);
        } else {
            std::cerr << "WARNING: unknown arg '" << arg << "'" << std::endl;
        }
    }

    int INODES = R*C;

    std::unique_ptr<ai::NeuralNetwork<T>> nn;
    
    if(load_file.empty()) {
        nn.reset(new ai::NeuralNetwork<T>(train_rate, INODES, HNODES, ONODES));
    } else {
        std::cout << "Loading json output from " << load_file << std::endl;

        std::string cache;
        std::ifstream ifs(load_file);
        sys::read(ifs, cache);
	
        json::object::ptr o = json::object::create(cache);
	
        nn.reset(new ai::NeuralNetwork<T>(o));
    }

    std::vector<std::tuple<int,math::matrix<T>>> inputs;
    std::mutex mutex;

    if(!train_path.empty()) {
        std::cout << "Loading handwriting from " << train_path << std::endl;
	
        std::ifstream ifs(train_path + "/all.txt~");
        while(ifs) {
            std::string img;
            ifs >> img;
            if(ifs && !img.empty()) {
                std::string path = train_path + "/" + img;
                std::string num = util::slice(path, I, "-");

                while(!num.empty() && num[0] == '0')
                    num.erase(0, 1);
                
                int n = 0;
                if(!num.empty())
                    n = util::int_value(num) - 1;

                if(n >= ONODES)
                    continue;

                math::matrix<T> input = load(path, R, C);

                for(int i = 0; i < train_multi; i++)
                    inputs.push_back(std::make_tuple(n, input));
            }
        }
    }

    if(!train_mnist_path.empty()) {
        std::cout << "Loading mnist data from " << train_mnist_path << std::endl;
        std::ifstream ifs(train_mnist_path);
        while(ifs) {
            std::string line;
            std::getline(ifs, line);
            if(ifs) {
                std::vector<std::string> inlist = util::tokenize(line, ",");
                int size = inlist.size() - 1;
		
                //std::cout << "Got " << size << " elements" << std::endl;
		
                int label = util::int_value(inlist.front());
                math::matrix<T> input(size, 1);
		
                for(std::size_t i = 0; i < size; i++) {
                    input(i, 0) = ((util::int_value(inlist[i+1]) / 255.0) * 0.99) + 0.01;
                }

                inputs.push_back(std::make_tuple(label, input));
            }
        }
    }

    if(!inputs.empty()) {
        math::matrix<T> target(ONODES, 1);
        target.foreach([](T& x) {
                x = 0.01;
            });
        
        std::uniform_int_distribution<int> idist(0, inputs.size()-1);
        
        for(uint e = 0; e < epochs; e++) {
            std::cout << "Training epoch " << e << ", " << inputs.size() << " inputs" << std::endl;
            if(train_decay > 0 && ((e % train_decay) == (train_decay - 1))) {
                std::cout << "Decay training rate from " << train_rate << " to " << (train_rate / 10.0) << std::endl;
                train_rate /= 10.0;
                nn->set_rate(train_rate);
            }
            
            std::cout << "Shuffling inputs... " << std::flush;
            for(int i = 0; i < inputs.size(); i++) {
                int x = idist(generator);
                auto tmp = inputs[i];
                inputs[i] = inputs[x];
                inputs[x] = tmp;
            }
            std::cout << "done" << std::endl;
            
            for(auto i : inputs) {
                int n = std::get<0>(i);
                math::matrix<T> input = std::get<1>(i);
                
                target(n, 0) = 0.99;
		
                nn->train(input, target);
		
                target(n, 0) = 0.01;
            }
        }
    }

    if(!output_file.empty()) {
        json::object::ptr o = nn->json();
        std::string str = o->str(true);
        
        std::cout << "Writing json output to " << output_file << std::endl;
        
        std::ofstream ofs(output_file);
        ofs << str;
    }        
    
    if(!test_mnist_path.empty()) {
        std::cout << "Opening " << test_mnist_path << std::endl;

        uint count = 0, correct = 0;
  
        std::ifstream tfs(test_mnist_path);
        while(tfs) {
            std::string line;
            std::getline(tfs, line);
            if(tfs) {
                std::vector<std::string> inlist = util::tokenize(line, ",");
                int size = inlist.size() - 1;
  
                //std::cout << "Got " << size << " elements" << std::endl;
      
                int label = util::int_value(inlist.front());
                math::matrix<T> input(size, 1);
      
                for(std::size_t i = 0; i < size; i++) {
                    input(i, 0) = ((util::int_value(inlist[i+1]) / 255.0) * 0.99) + 0.01;
                }

                math::matrix<T> output = nn->query(input);

                T max = output(0, 0);
                uint x = 0;
                for(uint i = 1; i < output.M; i++) {
                    if(output(i, 0) > max) {
                        max = output(i, 0);
                        x = i;
                    }
                }
      
                //std::cout << "Expected " << label << " got " << x << std::endl;
                count++;
                if(label == x)
                    correct++;
            }

        }

        double ratio = correct / double(count);
        std::cout << "Got " << ratio * 100 << "% success rate" << std::endl;

    }

    if(!test_train_path.empty()) {
        uint count = 0, correct = 0;

        sys::Directory root(test_train_path);
        auto samples = root.list_dirs(true);
        for(auto sample : samples) {
            uint scount = 0, scorrect = 0;
            uint ocount = 0, ocorrect = 0;
            //std::cout << "Opening sample " << sample << std::endl;
            std::string::size_type x = sample.find(S);
            std::string number = sample.substr(x + S.size());
            while(!number.empty() && number[0] == '0')
                number.erase(0, 1);
            int n = 0;
            if(!number.empty())
                n = util::int_value(number) - 1;

            if(n >= ONODES)
                continue;
                
            char c = convert(n);
            char oc = capitalize(c);
            int o = convert(oc);
            
            //std::cout << "Parsed label " << n << std::endl;
	    
            sys::Directory sdir(sample);
            auto images = sdir.list_files(true);
	    
            for(auto image : images) {
                //std::cout << "Opening image " << image << std::endl;
		
                math::matrix<T> input = load(image, R, C);
                math::matrix<T> output = nn->query(input);
		
                T max = output(0, 0);
                uint x = 0;
                for(uint i = 1; i < output.M; i++) {
                    if(output(i, 0) > max) {
                        max = output(i, 0);
                        x = i;
                    }
                }
		
                //std::cout << "Expected " << convert(n) << " got " << convert(x) << " (" << (100*max) << "%)" << std::endl;
                count++;
                scount++;
                if(n != x) {
                    if(o == x) {
                        ocorrect++;
                    }
                } else {
                    correct++;
                    scorrect++;
                }
            }
            
            double ratio = scorrect / double(scount);
            double oratio = (scorrect + ocorrect) / double(scount);
            if(o != n)
                std::cout << "Got " << ratio * 100 << "% success rate for " << convert(n) << ", " << 100 * oratio << " including " << oc << std::endl;
            else
                std::cout << "Got " << ratio * 100 << "% success rate for " << convert(n) << std::endl;

        }

        double ratio = correct / double(count);
        std::cout << "Got " << ratio * 100 << "% success rate" << std::endl;
    }
	
    if(!test_my_path.empty()) {
        uint count = 0, correct = 0;

        sys::Directory root(test_my_path);
        auto files = root.list_files(true);
        for(auto file : files) {
            if(file.find("-") != std::string::npos) {
                std::cout << "Opening file " << file << std::endl;
	    
                int n = util::int_value(util::slice(file, "-", "-"));

                if(n >= ONODES)
                    continue;
		
                std::cout << "Parsed label " << n << std::endl;
	    
                math::matrix<T> input = load(file, R, C);
                math::matrix<T> output = nn->query(input);
                auto rmax = getmax(output);
                int x = std::get<0>(rmax);
                T max = std::get<1>(rmax);
	    
                std::cout << "Expected " << convert(n) << " got " << convert(x) << " (" << (100*max) << "%)" << std::endl;
                count++;
                if(n != x) {
                    //std::cout << output << std::endl;
                } else {
                    correct++;
                }
            }
        }

        double ratio = correct / double(count);
        std::cout << "Got " << ratio * 100 << "% success rate" << std::endl;
    }
	
    return 0;
}

math::matrix<T> load(std::string path, uint r, uint c, bool greyscale) {
    using MagickCore::Quantum;
    const uint QMAX = QuantumRange;
    Magick::Image image(path);
    
    uint rrem = image.rows() % r;
    uint crem = image.columns() % c;
    
    uint radd = (rrem == 0 ? 0 : r - rrem);
    uint cadd = (crem == 0 ? 0 : c - crem);
    
    uint nrow = image.rows() + radd;
    uint ncol = image.columns() + cadd;

    uint rdiv = nrow / r;
    uint cdiv = ncol / c;
    
    if(rdiv != cdiv) {
        int diff = rdiv - cdiv;
        if(diff > 0) {
            cadd += (diff * c);
        } else {
            radd -= (diff * r);
        }
    }
    
    if(radd != 0 || cadd != 0) {
        Magick::Image icopy(image);
        Magick::Image base(Magick::Geometry(image.columns() + cadd, image.rows() + radd), Magick::Color("white"));
        base.composite(icopy, cadd/2, radd/2, Magick::OverCompositeOp);

        image = base;
    }

    if(image.rows() != r || image.columns() != c) {
        //std::cout << "Scaling image from " << image.rows() << "x" << image.columns() << " to " << r << "x" <<  c << std::endl;
	
        uint rscale = image.rows() / r;
        uint cscale = image.columns() / c;
        //std::cout << "Scale down 1/" << rscale << ", 1/" << cscale << std::endl;
	
        image.zoom(Magick::Geometry(r, c));
    }
    
    math::matrix<T> input(r*c, 1);
    for(uint y = 0; y < image.rows(); y++) {
        for(uint x = 0; x < image.columns(); x++) {
            Magick::Color color = image.pixelColor(x, y);
            input(y*image.columns() + x, 0) = ((QMAX - color.intensity()) / double(QMAX)) * 0.99 + 0.01;
            //std::cout << "Setting color intensity to " << input(y*image.columns() + x, 0) << std::endl;
        }
    }

    return input;
}

char convert(int n) {
    if(n < 10) {
        return '0' + n;
    } else if(n < 36) {
        return 'A' + (n - 10);
    } else {
        return 'a' + (n - 36);
    }
}

int convert(char c) {
    if(c >= '0' && c <= '9')
        return c - '0';
    else if(c >= 'A' && c <= 'Z')
        return 10 + (c - 'A');
    else
        return 36 + (c - 'a');
}

char capitalize(char c) {
    if(std::isalpha(c)) {
        if(std::isupper(c))
            return std::tolower(c);
        else
            return std::toupper(c);
    } else {
        return c;
    }
}

std::tuple<uint,T> getmax(math::matrix<T> output) {
    T max = output(0, 0);
    uint x = 0;
    for(uint i = 1; i < output.M; i++) {
        if(output(i, 0) > max) {
            max = output(i, 0);
            x = i;
        }
    }

    return std::make_tuple(x, max);
}
