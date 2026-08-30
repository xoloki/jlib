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
#include <jlib/sys/Directory.hh>
#include <jlib/sys/sys.hh>
#include <jlib/ai/neural.hh>

#ifdef HAVE_METAL
#include <jlib/metal/backend.hh>
#endif
#include <jlib/apps/magick.hh>

#include <algorithm>

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

// float, not double.  Metal Shading Language has no double at all -- the
// compiler refuses it in as many words -- so a GPU multiply can only be
// offered in float, and there is no point in the network being wider than the
// thing that is supposed to accelerate it.  Nothing trains in fp64 anyway.
typedef float T;

math::matrix<T> load(const std::string& path, uint r, uint c, bool greyscale = true);
char convert(int n);
int convert(char c);
/**
 * The same letter in the other case, or the character unchanged.
 *
 * Was called capitalize(), which reads as "make this uppercase" and is not
 * what it does or what it is for.  It has to work both ways: this is a 62-way
 * classifier -- ten digits, then A-Z, then a-z -- and the scorer uses the case
 * twin to count "right letter, wrong case" apart from "wrong letter", which
 * are different kinds of failure.
 *
 * A digit has no twin, so it comes back unchanged and scores no bonus.
 */
char swap_case(char c);

std::tuple<uint,double> getmax(math::matrix<T> m);

int main(int argc, char** argv) {
    uint R = 90;
    uint C = 120;
    std::vector<uint> HNODES;
    int ONODES = 62;
    const std::string S = "Sample";
    const std::string I = "img";
    uint epochs = 1, train_multi = 1;
    std::string train_path, test_train_path, test_my_path, load_file, output_file, train_mnist_path, test_mnist_path;

    // Hidden and output are named separately because they want different
    // answers: relu is what lets depth train, and relu on the output of a
    // classifier scored against 0.01/0.99 targets kills any unit that goes
    // negative.  See ai::activation.
    std::string hidden_activation = "sigmoid", output_activation = "sigmoid";

    // One sample at a time by default, which is what this always did.  The GPU
    // needs a batch to be worth using at all: at a batch of one its dispatch
    // costs more than the multiply.
    int batch_size = 1;
    bool use_metal = false;
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
        } else if(arg == "--batch-size") {
            batch_size = util::int_value(argv[++i]);
        } else if(arg == "--metal") {
            use_metal = true;
        } else if(arg == "--hidden-activation") {
            hidden_activation = argv[++i];
        } else if(arg == "--output-activation") {
            output_activation = argv[++i];
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

        nn->set_hidden_activation(ai::activation_from_name(hidden_activation));
        nn->set_output_activation(ai::activation_from_name(output_activation));
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

#ifdef HAVE_METAL
    // Held here rather than in the lambda: the kernel and the queue are worth
    // keeping across calls, and a training loop makes the same shapes over and
    // over.
    if(use_metal) {
        // The whole network moves, not just the multiply.  A hook that took
        // only the matrix product left everything else on the CPU, so each
        // layer bounced the data across and a step paid eight
        // synchronisations rather than one.
        std::shared_ptr<jlib::metal::backend<T> > gpu(new jlib::metal::backend<T>);

        nn->set_backend(gpu);

        std::cout << "Training on " << gpu->name()
                  << ", batch " << batch_size << std::endl;
    }
#else
    if(use_metal) {
        std::cerr << "WARNING: --metal, but this build has no Metal" << std::endl;
    }
#endif

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
            
            // A column per sample.  train() has taken a batch since the
            // gradient became a mean, and one column is the old behaviour
            // exactly, so batch_size = 1 changes nothing.
            for(std::size_t s = 0; s < inputs.size(); s += batch_size) {
                const std::size_t b =
                    std::min<std::size_t>(batch_size, inputs.size() - s);

                math::matrix<T> x(INODES, b), y(ONODES, b);

                for(std::size_t c = 0; c < b; c++) {
                    const auto& item = inputs[s + c];
                    const math::matrix<T>& in = std::get<1>(item);

                    for(int r = 0; r < INODES; r++)
                        x(r, c) = in(r, 0);

                    for(int r = 0; r < ONODES; r++)
                        y(r, c) = 0.01;

                    y(std::get<0>(item), c) = 0.99;
                }

                nn->train(x, y);
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
                
            // The class this sample would be if its case were the other one,
            // so a prediction of "a" for an "A" can be counted separately from
            // a prediction of "4".  For a digit the twin is the digit itself,
            // so twin == n and the bonus below can never fire.
            const char expect = convert(n);
            const char twin_char = swap_case(expect);
            const int twin = convert(twin_char);
            
            //std::cout << "Parsed label " << n << std::endl;
	    
            sys::Directory sdir(sample);
            auto images = sdir.list_files(true);
	    
            for(auto image : images) {
                //std::cout << "Opening image " << image << std::endl;
		
                math::matrix<T> input = load(image, R, C);
                math::matrix<T> output = nn->query(input);
		
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
                    if(twin == x) {
                        ocorrect++;
                    }
                } else {
                    correct++;
                    scorrect++;
                }
            }
            
            double ratio = scorrect / double(scount);
            double oratio = (scorrect + ocorrect) / double(scount);
            if(twin != n)
                std::cout << "Got " << ratio * 100 << "% success rate for " << convert(n) << ", " << 100 * oratio << " including " << twin_char << std::endl;
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

math::matrix<T> load(const std::string& path, uint r, uint c, bool greyscale) {
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
            input(y*image.columns() + x, 0) = ((QMAX - jlib::apps::luma(color)) / double(QMAX)) * 0.99 + 0.01;
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

char swap_case(char c) {
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
