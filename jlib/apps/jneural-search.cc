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
#include <jlib/ai/neural.hh>

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

typedef double T;

math::matrix<T> load(std::string path, uint r, uint c, bool greyscale = true);
std::vector<std::tuple<int,math::matrix<T>>> load_mnist(std::string path);

char convert(int n);
int convert(char c);
char capitalize(char c);

std::tuple<uint,double> getmax(math::matrix<T> m);

std::string make_output(std::vector<uint> hnodes);

int main(int argc, char** argv) {
    uint R = 90;
    uint C = 120;
    std::vector<uint> hnodes;
    uint hmin = 10;
    uint hmax = 1000;
    uint hdiff = 10;
    uint hlayers = 1;
    int ONODES = 62;
    const std::string S = "Sample";
    const std::string I = "img";
    uint epochs = 1, train_multi = 1;
    std::string train_path, test_train_path, test_my_path, load_file, output_file, train_mnist_path, test_mnist_path;
    double train_rate = 0.1;
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
            train_rate = util::double_value(argv[++i]);
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
            hnodes.push_back(std::stoi(argv[++i]));
        } else if(arg == "--hidden-min") {
            hmin = std::stoi(argv[++i]);
        } else if(arg == "--hidden-max") {
            hmax = std::stoi(argv[++i]);
        } else if(arg == "--hidden-layers") {
            hlayers = std::stoi(argv[++i]);
        } else if(arg == "--hidden-diff") {
            hdiff = std::stoi(argv[++i]);
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

    std::vector<uint> hidden;
    for(int i = 0; i < hlayers; i++) {
        hidden.push_back(hmin);
    }

    std::vector<std::tuple<int,math::matrix<T>>> inputs = load_mnist(train_mnist_path);
    std::uniform_int_distribution<int> idist(0, inputs.size()-1);
    bool finished = false, first = true;
    sys::Directory pwd("./");
    
    while(!finished) {
	if(!first) {
	    // go backwards finding a layer to increment
	    bool found = false;
	    for(int i = hlayers - 1; i >= 0; i--) {
		if(hidden[i] < hmax) {
		    hidden[i] += hdiff;
		    
		    // now go back forwards and reset to hmin
		    for(int j = i+1; j < hlayers; j++) {
			hidden[j] = hmin;
		    }
		    
		    found = true;
		    break;
		}
	    }
	    
	    if(!found) {
		finished = true;
		continue;
	    }
	} else {
	    first = false;
	}

	std::ostringstream os;
        os << "deep-search";
        for(auto h : hidden) {
            os << "-" << h << "h";
        }
        os << "-" << train_rate << "r";
        os << "-" << epochs << "e";
        os << ".json";
        output_file = os.str();
            
	bool exists = false;
	try {
	    if(pwd.is(output_file, sys::REGULAR)) {
		exists = true;
	    }
	} catch(...) {
	}

        std::unique_ptr<ai::NeuralNetwork<double>> nn;
	if(exists) {
	    std::cout << "Loading " << output_file << std::endl;
	    
	    std::string cache;
	    std::ifstream ifs(output_file);
	    sys::read(ifs, cache);
	    
	    json::object::ptr o = json::object::create(cache);
	    
	    nn.reset(new ai::NeuralNetwork<double>(o));
	} else {
	    std::cout << "Training " << output_file << std::endl;
	    nn.reset(new ai::NeuralNetwork<double>(train_rate, INODES, hidden, ONODES));

	    math::matrix<T> target(ONODES, 1);
	    target.foreach([](T& x) {
		    x = 0.01;
		});
        
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

	    json::object::ptr o = nn->json();
	    std::string str = o->str(true);
            
	    std::cout << "Writing " << output_file << std::endl;
            
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

                    math::matrix<double> output = nn->query(input);

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
    }
    
    return 0;
}

std::vector<std::tuple<int,math::matrix<T>>> load_mnist(std::string path) {
    std::cout << "Loading mnist data from " << path << std::endl;
    std::vector<std::tuple<int,math::matrix<T>>> inputs;
    std::ifstream ifs(path);
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

    return inputs;
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
