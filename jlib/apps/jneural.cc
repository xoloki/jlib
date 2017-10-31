#include <jlib/math/matrix.hh>
#include <jlib/util/util.hh>
#include <jlib/sys/Directory.hh>

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
	//std::cout << "m_wih[" << m_wih.M << "," << m_wih.N << "] \n" << m_wih << std::endl;
	//std::cout << "inputs[" << inputs.M << "," << inputs.N << "] \n" << inputs << std::endl;
	//std::cout << "targets[" << targets.M << "," << targets.N << "] \n" << targets << std::endl;

	math::matrix<T> hidden_inputs = m_wih * inputs;
	//std::cout << "hidden_inputs[" << hidden_inputs.M << "," << hidden_inputs.N << "] \n" << hidden_inputs << std::endl;

	math::matrix<T> hidden_outputs = m_activation_function(hidden_inputs);
	//std::cout << "hidden_outputs[" << hidden_outputs.M << "," << hidden_outputs.N << "] \n" << hidden_outputs << std::endl;

	math::matrix<T> final_inputs = m_who * hidden_outputs;
	math::matrix<T> final_outputs = m_activation_function(final_inputs);
	//std::cout << "final_outputs[" << final_outputs.M << "," << final_outputs.N << "] \n" << final_outputs << std::endl;
    
	// output layer error is the (target - actual)
	math::matrix<T> output_errors = targets - final_outputs;
	//std::cout << "output_errors[" << output_errors.M << "," << output_errors.N << "] \n" << output_errors << std::endl;

	// hidden layer error is the output_errors, split by weights, recombined at hidden nodes
	math::matrix<T> whoT = m_who.transpose();
	math::matrix<T> hidden_errors = whoT * output_errors;
	//std::cout << "hidden_errors[" << hidden_errors.M << "," << hidden_errors.N << "] \n" << hidden_errors << std::endl;

	// update the weights for the links between the hidden and output layers
	math::matrix<T> hoT = hidden_outputs.transpose();
	m_who += m_lrate * (((output_errors ^ final_outputs ^ (1.0 - final_outputs)) * hoT));
	/*
	  m_who.foreach_index([&](uint k, uint j, T& v) {
	  v += (m_lrate * ( output_errors(k, 0) * final_outputs(k, 0) * (1.0 - final_outputs(k, 0)) * hoT(0, j) ));
	  });
	*/
	// update the weights for the links between the input and hidden layers
	math::matrix<T> iT = inputs.transpose();
	m_wih += m_lrate * ((hidden_errors ^ hidden_outputs ^ (1.0 - hidden_outputs)) * iT);
	/*
	  m_wih.foreach_index([&](uint k, uint j, T& v) {
	  v += (m_lrate * ( hidden_errors(k, 0) * hidden_outputs(k, 0) * (1.0 - hidden_outputs(k, 0)) * iT(0, j) ));
	  });
	*/
    }

    math::matrix<T> query(math::matrix<T> inputs) {
	math::matrix<T> hidden_inputs = m_wih * inputs;
	math::matrix<T> hidden_outputs = m_activation_function(hidden_inputs);
	math::matrix<T> final_inputs = m_who * hidden_outputs;
	math::matrix<T> final_outputs = m_activation_function(final_inputs);

	return final_outputs;
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

using jlib::math::matrix;

int main(int argc, char** argv) {
    const int HNODES = 200;
    const int ONODES = 10;
    const int INODES = 784;
    std::string training_file = "mnist_dataset/mnist_train_full.csv";
    std::string testing_file = "mnist_dataset/mnist_test_full.csv";

    jlib::ml::NeuralNetwork<double> nn(INODES, HNODES, ONODES, 0.1);

    if(argc > 1) {
	training_file = argv[1];
    }

    if(argc > 2) {
	testing_file = argv[2];
    }
  
    std::cout << "Opening " << training_file << std::endl;
    uint epochs = 5;
    for(uint e = 0; e < epochs; e++) {
	std::cout << "Training epoch " << e << std::endl;
    std::ifstream ifs(training_file);
    while(ifs) {
	std::string line;
	std::getline(ifs, line);
	if(ifs) {
	    std::vector<std::string> inlist = jlib::util::tokenize(line, ",");
	    int size = inlist.size() - 1;
  
	    //std::cout << "Got " << size << " elements" << std::endl;
      
	    int label = jlib::util::int_value(inlist.front());
	    matrix<double> input(size, 1);
      
	    for(std::size_t i = 0; i < size; i++) {
		input(i, 0) = ((jlib::util::int_value(inlist[i+1]) / 255.0) * 0.99) + 0.01;
	    }
      
	    matrix<double> target(ONODES, 1);
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
  
    std::cout << "Opening " << testing_file << std::endl;

    uint count = 0, correct = 0;
  
    std::ifstream tfs(testing_file);
    while(tfs) {
	std::string line;
	std::getline(tfs, line);
	if(tfs) {
	    std::vector<std::string> inlist = jlib::util::tokenize(line, ",");
	    int size = inlist.size() - 1;
  
	    //std::cout << "Got " << size << " elements" << std::endl;
      
	    int label = jlib::util::int_value(inlist.front());
	    matrix<double> input(size, 1);
      
	    for(std::size_t i = 0; i < size; i++) {
		input(i, 0) = ((jlib::util::int_value(inlist[i+1]) / 255.0) * 0.99) + 0.01;
	    }

	    matrix<double> output = nn.query(input);

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

    jlib::sys::Directory dir("my_own_images");
    std::vector<std::string> files = dir.list_files();

    for(std::string file : files) {
	if(jlib::util::begins(file, "joey-")) {
	    try {
	    std::cout << "Opening " << file << std::endl;
	    std::string nstr = file.substr(5, 1);
	    int n = jlib::util::int_value(nstr);

	    Magick::Image image(dir.get_path() + "/" + file);
	    matrix<double> input(image.rows()*image.columns(), 1);
	    for(uint y = 0; y < image.rows(); y++) {
		for(uint x = 0; x < image.columns(); x++) {
		    Magick::Color color = image.pixelColor(x, y);
		    input(y*image.columns() + x, 0) = ((65535 - color.intensity()) / 65535.0) * 0.99 + 0.01;
		    //std::cout << "Setting color intensity to " << input(y*image.columns() + x, 0) << std::endl;
		}
	    }

	    matrix<double> output = nn.query(input);

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

	    
	    } catch(...) {
		std::cerr << "Caught exception" << std::endl;
	    }
	}
    }

    double ratio = correct / double(count);
    std::cout << "Got " << ratio * 100 << "% success rate" << std::endl;
  
    return 0;
}
