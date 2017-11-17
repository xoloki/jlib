/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2017 Joey Yandle <xoloki@gmail.com>
 * 
 */

#include <jlib/math/matrix.hh>
#include <jlib/util/util.hh>
#include <jlib/util/json.hh>
#include <jlib/sys/Directory.hh>
#include <jlib/sys/sys.hh>

#include <functional>
#include <random>
#include <fstream>
#include <tuple>

#include <cmath>

#include <Magick++.h>

std::default_random_engine generator;

namespace jlib {
namespace ml {

template<typename T>
class NeuralNetwork {
public:
    NeuralNetwork(util::json::object::ptr p)
        : NeuralNetwork(p->get("ninput"), p->get("nhidden"), p->get("noutput"), p->get("lrate"))
    {
        m_wih.foreach_index([&](uint r, uint c, T& x) {
                x = p->obj("wih")->get(r*m_ninput + c);
            });

        m_who.foreach_index([&](uint r, uint c, T& x) {
                x = p->obj("who")->get(r*m_nhidden + c);
            });
    }
    
    NeuralNetwork(uint ninput, uint nhidden, uint noutput, double lrate)
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
                x = hdist(generator);
            });

        T obound = pow(m_noutput, -0.5);
        std::uniform_real_distribution<T> odist(-obound, obound);
    
        m_who.foreach([&](T& x) {
                x = odist(generator);
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

    void train(math::matrix<T> inputs, math::matrix<T> targets) {
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

    math::matrix<T> query(math::matrix<T> inputs) {
        math::matrix<T> hidden_inputs = m_wih * inputs;
        math::matrix<T> hidden_outputs = m_activation_function(hidden_inputs);
        math::matrix<T> final_inputs = m_who * hidden_outputs;
        math::matrix<T> final_outputs = m_activation_function(final_inputs);

        return final_outputs;
    }

    //(p->get("ninput"), p->get("nhidden"), p->get("noutput"), p->get("lrate"))
    
    util::json::object::ptr json() {
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
    
protected:
    int m_ninput;
    int m_nhidden;
    int m_noutput;
    double m_lrate;
    math::matrix<T> m_wih;
    math::matrix<T> m_who;
    std::function<math::matrix<T>(math::matrix<T>)> m_activation_function;
};

}
}

using namespace jlib;
using namespace jlib::util;

typedef double T;

math::matrix<T> load(std::string path, uint r, uint c, bool greyscale = true);
char convert(int n);
int convert(char c);
char capitalize(char c);

std::tuple<uint,double> getmax(math::matrix<T> m);

int main(int argc, char** argv) {
    uint R = 90;
    uint C = 120;
    int HNODES = 256;
    int ONODES = 62;
    const std::string S = "Sample";
    const std::string I = "img";
    uint epochs = 1, train_multi = 1;
    std::string train_path, test_train_path, test_my_path, load_file, output_file, train_mnist_path, test_mnist_path;
    double train_rate = 0.1;
    
    for(int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if(arg == "--train-path") {
            train_path = argv[++i];
        } else if(arg == "--train-epochs") {
            epochs = util::int_value(argv[++i]);
        } else if(arg == "--train-multi") {
            train_multi = util::int_value(argv[++i]);
        } else if(arg == "--train-rate") {
            train_rate = util::double_value(argv[++i]);
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
            HNODES = util::int_value(argv[++i]);
        } else if(arg == "--output-nodes") {
            ONODES = util::int_value(argv[++i]);
        } else if(arg == "--image-rows") {
            R = util::int_value(argv[++i]);
        } else if(arg == "--image-cols") {
            C = util::int_value(argv[++i]);
        }
    }

    int INODES = R*C;
    ml::NeuralNetwork<double> nn(INODES, HNODES, ONODES, train_rate);
    
    if(!load_file.empty()) {
        std::cout << "Loading json output from " << load_file << std::endl;

        std::string cache;
        std::ifstream ifs(load_file);
        sys::read(ifs, cache);
	
        json::object::ptr o = json::object::create(cache);
	
        nn = ml::NeuralNetwork<double>(o);
    }

    std::vector<std::tuple<int,math::matrix<T>>> inputs;
    
    if(!train_path.empty()) {
	std::cout << "Training handwriting from " << train_path << std::endl;
	
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
	std::cout << "Training mnist data from " << train_mnist_path << std::endl;
	std::ifstream ifs(train_mnist_path);
	while(ifs) {
	    std::string line;
	    std::getline(ifs, line);
	    if(ifs) {
		std::vector<std::string> inlist = util::tokenize(line, ",");
		int size = inlist.size() - 1;
		
		//std::cout << "Got " << size << " elements" << std::endl;
		
		int label = util::int_value(inlist.front());
		math::matrix<double> input(size, 1);
		
		for(std::size_t i = 0; i < size; i++) {
		    input(i, 0) = ((util::int_value(inlist[i+1]) / 255.0) * 0.99) + 0.01;
		}

		inputs.push_back(std::make_tuple(label, input));
	    }
	}
    }

    math::matrix<T> target(ONODES, 1);
    target.foreach([](T& x) {
	    x = 0.01;
	});

    std::uniform_int_distribution<int> idist(0, inputs.size()-1);
    for(int i = 0; i < inputs.size(); i++) {
	int x = idist(generator);
	auto tmp = inputs[i];
	inputs[i] = inputs[x];
	inputs[x] = tmp;
    }

    if(!inputs.empty()) {
	for(uint e = 0; e < epochs; e++) {
	    std::cout << "Training epoch " << e << ", " << inputs.size() << " inputs" << std::endl;
	    
	    for(auto i : inputs) {
		int n = std::get<0>(i);
		math::matrix<T> input = std::get<1>(i);
		
		target(n, 0) = 0.99;
		
		nn.train(input, target);
		
		target(n, 0) = 0.01;
	    }
	}
    }

    if(!output_file.empty()) {
	json::object::ptr o = nn.json();
	std::string str = o->str();
        
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
		math::matrix<double> input(size, 1);
      
		for(std::size_t i = 0; i < size; i++) {
		    input(i, 0) = ((util::int_value(inlist[i+1]) / 255.0) * 0.99) + 0.01;
		}

		math::matrix<double> output = nn.query(input);

		double max = output(0, 0);
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
                math::matrix<double> output = nn.query(input);
		
                double max = output(0, 0);
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
                math::matrix<double> output = nn.query(input);
                auto rmax = getmax(output);
                int x = std::get<0>(rmax);
                double max = std::get<1>(rmax);
	    
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
    
    math::matrix<double> input(r*c, 1);
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

std::tuple<uint,double> getmax(math::matrix<T> output) {
    double max = output(0, 0);
    uint x = 0;
    for(uint i = 1; i < output.M; i++) {
        if(output(i, 0) > max) {
            max = output(i, 0);
            x = i;
        }
    }

    return std::make_tuple(x, max);
}
