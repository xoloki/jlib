#include <iostream>
#include <sstream>

#include <jlib/util/xml.hh>

#include <cstdlib>

const std::string DEFAULT = "<?xml version=\"1.0\"?>\n";
const std::string DEFAULT_1_1 = "<?xml version=\"1.1\"?>\n";
const std::string FOO = "<?xml version=\"1.0\"?>\n<foo>\n</foo>\n";
const std::string FOO_BAR_BAZ = "<?xml version=\"1.0\"?>\n<foo bar=\"baz\">\n</foo>\n";
const std::string FOO_BAR_BAT = "<?xml version=\"1.0\"?>\n<foo bar=\"bat\">\n</foo>\n";

using namespace jlib::util::xml;

void check_document(document& doc,std::string x);

int main(int argc, char** argv) {

    //try {
        document doc;
        check_document(doc,DEFAULT);
        
        node::ptr n = node::create("foo");
        doc.add(n);
        check_document(doc,FOO);
        
        n->set_attribute("bar","baz");
        check_document(doc,FOO_BAR_BAZ);
        
        n->set_attribute("bar","bat");
        check_document(doc,FOO_BAR_BAT);
        
        return 0;
        /*
    } catch(std::exception e) {
        std::cerr << "error: " << e.what() << std::endl;

        return 1;
    } catch(...) {
        return 1;
    }
        */
}

void check_document(document& doc,std::string x) {
    std::ostringstream o;
    doc.save(o);

    if(o.str() != x) {
        std::cerr << "error: document is:\n"
                  << o.str() << std::endl
                  << "expected:\n"
                  << x << std::endl;
        exit(1);
    }

}
