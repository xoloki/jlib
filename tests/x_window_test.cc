#include <jlib/x/Window.hh>

#include <iostream>

#include <cstdlib>

#include <unistd.h>

void key_pressed(std::string key,int x,int y);
void button_pressed(int button,int x,int y);
void timeout();

int main(int argc, char** argv) {
    try {
        jlib::x::Window window("x_window_test", 200, 200);
        int w = window.get_width();
        int h = window.get_height();

        window.fill_rectangle(0,0,w/2,h/2);
        window.fill_rectangle(w/2,h/2,w/2,h/2);
        
        window.center();
        window.seek(-10,-10);
        window.set_mode(jlib::x::Window::INVERT);

        window.fill_oval(20,20);

        window.center();
        window.seek(-40,5);
        window << "HELLO, WORLD!";

        window.key_press.connect(sigc::ptr_fun(&key_pressed));
        window.button_press.connect(sigc::ptr_fun(&button_pressed));
        window.timeout.connect(sigc::ptr_fun(&timeout));

        window.run();
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::exit(1);
    }

    std::exit(0);
}

void key_pressed(std::string key,int x,int y) {
    std::cout << "key pressed: " << key << ": ("<<x<<","<<y<<")" <<std::endl;
    if(key == "q")
        std::exit(0);
}

void button_pressed(int button,int x,int y) {
    std::cout << "button pressed: " << button << ": ("<<x<<","<<y<<")" <<std::endl;
}

void timeout() {
    std::exit(0);
}
