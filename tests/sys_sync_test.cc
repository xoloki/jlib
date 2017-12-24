#include <jlib/sys/sync.hh>

#include <iostream>

#include <cstdlib>

int global_foo = 0;

class Foo {
public:
    virtual std::string foo() { return "foo"; }
    std::string echo(std::string s) { return s; }
    void set_global_foo(int s) { global_foo = s; }
};

class Bar : public Foo {
public:
    virtual std::string foo() { return "bar"; }
};

int main(int argc, char** argv) {
    using jlib::sys::sync;

    sync<int> i(0);

    if(i != 0) {
        std::cerr << "error: incorrect value for sync: expected 0 got " << (int)i << std::endl;
        exit(1);
    }

    i = 1;

    if(i != 1) {
        std::cerr << "error: incorrect value for sync: expected 1 got " << (int)i << std::endl;
        exit(1);
    }

    exit(0);
}
