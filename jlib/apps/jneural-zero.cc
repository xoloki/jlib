/* -*- mode:C++ c-basic-offset:4  tab-width:4 -*-
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

#include <cmath>

#include <Magick++.h>

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
        std::default_random_engine generator;

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

int main(int argc, char** argv) {
    const int HNODES = 200;
    const int ONODES = 10;
    const int INODES = 784;
    std::string training_file = "mnist_dataset/mnist_train_full.csv";
    std::string testing_file = "mnist_dataset/mnist_test_full.csv";
    uint epochs = 5;

    ml::NeuralNetwork<double> nn(INODES, HNODES, ONODES, 0.1);

    if(argc > 1) {
        training_file = argv[1];
    }

    if(argc > 2) {
        testing_file = argv[2];
    }

    if(argc > 3) {
        epochs = util::int_value(argv[3]);
    }
  
    std::cout << "Opening " << training_file << std::endl;
    if(ends(training_file, ".csv")) {
        for(uint e = 0; e < epochs; e++) {
            std::cout << "Training epoch " << e << std::endl;
            std::ifstream ifs(training_file);
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
		    
                    math::matrix<double> target(ONODES, 1);
                    for(int i = 0; i < ONODES; i++) {
                        if(i == label)
                            target(i, 0) = 0.99;
                        else
                            target(i, 0) = 0.01;
                    }
		    
                    nn.train(input, target);
                }
            }
            ifs.close();
        }

        if(argc > 4) {
            std::string output_file = argv[4];
            json::object::ptr o = nn.json();
        }
	
    } else {
        std::string cache;
        std::ifstream ifs(training_file);
        sys::read(ifs, cache);

        json::object::ptr o = json::object::create(cache);
	
    }
  
    std::cout << "Opening " << testing_file << std::endl;

    uint count = 0, correct = 0;
  
    std::ifstream tfs(testing_file);
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

    sys::Directory dir("my_own_images");
    std::vector<std::string> files = dir.list_files();
    
    
    for(std::string file : files) {
        if(util::begins(file, "joey-")) {
            try {
                std::cout << "Opening " << file << std::endl;
                std::string nstr = file.substr(5, 1);
                int n = util::int_value(nstr);

                using MagickCore::Quantum;
                const uint QMAX = QuantumRange;
                const uint SIZE = 28;
                Magick::Image image(dir.get_path() + "/" + file);

                uint rrem = image.rows() % SIZE;
                uint crem = image.columns() % SIZE;

                uint radd = (rrem == 0 ? 0 : SIZE - rrem);
                uint cadd = (crem == 0 ? 0 : SIZE - crem);

                uint nrow = image.rows() + radd;
                uint ncol = image.columns() + cadd;

                if(nrow != ncol) {
                    int diff = nrow - ncol;
                    if(diff > 0) {
                        cadd += diff;
                    } else {
                        radd -= diff;
                    }
                }
	    
                if(radd != 0 || cadd != 0) {
                    Magick::Image icopy(image);
                    Magick::Image base(Magick::Geometry(image.columns() + cadd, image.rows() + radd), Magick::Color("white"));
                    base.composite(icopy, cadd/2, radd/2, Magick::OverCompositeOp);

                    std::cout << "Scaling image from " << image.rows() << "x" << image.columns() << " to " << base.rows() << "x" <<  base.columns() << std::endl;
		
                    if(base.rows() != base.columns())
                        std::cerr << "Not square!" << std::endl;

                    if(base.rows() % SIZE != 0)
                        std::cerr << "Not SIZE aligned" << std::endl;
		
                    uint scale = base.rows() / SIZE;
                    std::cout << "Scale down 1/" << scale << std::endl;

                    base.zoom(Magick::Geometry(SIZE, SIZE));

                    image = base;
                }
	    
                math::matrix<double> input(image.rows()*image.columns(), 1);
                for(uint y = 0; y < image.rows(); y++) {
                    for(uint x = 0; x < image.columns(); x++) {
                        Magick::Color color = image.pixelColor(x, y);
                        input(y*image.columns() + x, 0) = ((QMAX - color.intensity()) / double(QMAX)) * 0.99 + 0.01;
                        //std::cout << "Setting color intensity to " << input(y*image.columns() + x, 0) << std::endl;
                    }
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
      
                std::cout << "Expected " << n << " got " << x << " (" << (100*max) << "%)" << std::endl;
                if(n != x)
                    std::cout << output << std::endl;
	    
            } catch(std::exception& e) {
                std::cerr << "Caught exception: " << e.what() << std::endl;
            } catch(...) {
                std::cerr << "Caught exception" << std::endl;
            }
        }
    }

    double ratio = correct / double(count);
    std::cout << "Got " << ratio * 100 << "% success rate" << std::endl;
  
    return 0;
}
