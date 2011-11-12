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
    i.lock();
    if(i.trylock()) {
        std::cerr << "error: able to lock sync twice" << std::endl;
        exit(1);
    }
    else {
        i.unlock();
    }

    if(i != 0) {
        std::cerr << "error: incorrect value for sync: expected 0 got " << (int)i << std::endl;
        exit(1);
    }

    i = 1;

    if(i != 1) {
        std::cerr << "error: incorrect value for sync: expected 1 got " << (int)i << std::endl;
        exit(1);
    }

    sync<Foo> foo;
    sync<Bar> bar;

    if(foo.call(&Foo::foo) != "foo") {
        std::cerr << "error: in jlib::sys::sync::call<class F, class rettype>"<< std::endl;
        exit(1);
    }

    if(foo.call(&Foo::echo, std::string("baz")) != "baz") {
        std::cerr << "error: in jlib::sys::sync::call<class F, class rettype, class Arg>"<< std::endl;
        exit(1);
    }

    foo.call<Foo,int>(&Foo::set_global_foo, 666);
    if(global_foo != 666) {
        std::cerr << "error: in jlib::sys::sync::call<class F, class Arg>"<< std::endl;
        exit(1);
    }

    exit(0);
}
