/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <jwy@divisionbyzero.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 * 
 */

#include <jlib/x/Window.hh>
#include <jlib/util/util.hh>
#include <jlib/sys/sys.hh>
#include <jlib/sys/Directory.hh>
#include <glibmm/main.h>
#include <glibmm/thread.h>
#include <sigc++/sigc++.h>

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <exception>
#include <sstream>
#include <set>

#include <cstdlib>

std::string get_incoming() {
    return std::string(std::getenv("HOME")) + "/PoisonDownloads/.incoming/";
}

namespace jlib {
namespace poisoned {


class Download : public sigc::trackable {
public:
    class Chunk {
    public:
	std::pair<long, long> points;
	bool changed;

	bool operator<(const Chunk& chunk) const {
	    return points < chunk.points;
	}

	bool operator==(const Chunk& chunk) const {
	    return points == chunk.points;
	}
    };

    Download(std::string file) 
	: state(get_incoming() + file)
    {
	load();
    }

    void load() {
	std::ifstream ifs(state.c_str());
	std::string data;
	sys::read(ifs, data);
	
	const std::string TRANSMIT = "transmit = ";
	const std::string TOTAL = "total = ";
	const std::string CHUNKS = "chunks = ";
	const std::string MAX_SEEK = "max_seek = ";
	const std::string FILENAME = "filename = ";

	std::vector<std::string> tokens = util::tokenize(data, "\n", false);
	for (unsigned int i = 0; i < tokens.size(); i++) {
	    if (util::begins(tokens[i], TRANSMIT)) {
		transmit = util::int_value(tokens[i].substr(TRANSMIT.length()));
	    } else if (util::begins(tokens[i], TOTAL)) {
		total = util::int_value(tokens[i].substr(TOTAL.length()));
	    } else if (util::begins(tokens[i], FILENAME)) {
		filename = tokens[i].substr(FILENAME.length());
	    } else if(util::begins(tokens[i], MAX_SEEK)) {
		max_seek = util::int_value(tokens[i].substr(MAX_SEEK.length()));
	    } else if (util::begins(tokens[i], CHUNKS)) {
		std::string c = tokens[i].substr(CHUNKS.length());
		std::vector<std::string> ct = util::tokenize(c, " ", false);

		std::set<Chunk> oldchunks;
		for(std::list<Chunk>::iterator ci = chunks.begin(); 
		    ci != chunks.end(); ci++) {
		    oldchunks.insert(*ci);
		}

		chunks.clear();
		for (unsigned int j = 0; j < ct.size(); j++) {
		    std::vector<std::string> ctc = 
			util::tokenize(ct[j], "-", false);

		    if (ctc.size() == 2) {
			Chunk chunk;
			chunk.points.first = util::int_value(ctc[0]);
			chunk.points.second = util::int_value(ctc[1]);
			chunk.changed = 
			    (oldchunks.size() && 
			     (oldchunks.find(chunk) == oldchunks.end()));
			
			chunks.push_back(chunk);
		    }
		}
		
	    }
	}

	mtime = util::file::mtime(state);
    }

    bool changed() const {
	return mtime != util::file::mtime(state);
    }

    double get_percent() const {
	return (double)get_transmit() / (double)get_total();
    }

    bool operator<(const poisoned::Download& y) const {
	return get_percent() < y.get_percent();
    }

    std::string get_filename() const { return filename; }
    long get_total() const { return total; }
    long get_transmit() const { return transmit; }
    std::list<Chunk> get_chunks() const { return chunks; }

private:
    std::string state;
    std::string filename;
    long transmit;
    long total;
    long max_seek;
    long mtime;

    std::list<Chunk> chunks;
};


class giftd {
public:
    giftd() {
	load();
    }

    void load() {
	sys::Directory dir(get_incoming());
	std::vector<std::string> files = dir.list(sys::REGULAR);

	downloads.clear();
	for(unsigned int i = 0; i < files.size(); i++) {
	    std::string file = files[i];
	    if(file[0] == '.' && util::ends(file, ".state")) {
		try {
		    std::string data = file.substr(1);
		    data = data.substr(0, data.rfind(".state"));
		    util::file::getstat(get_incoming() + "/" + data);

		    downloads.insert(poisoned::Download(file));
		} catch(std::exception& e) {}
	    }
	}
    }

    std::set<Download>& get_downloads() { return downloads; }
    //const std::set<Download>& get_downloads() const { return downloads; }

private:
    std::set<Download> downloads;
};

class Show : public sigc::trackable {
public:
    void on_key_press(std::string key, int x, int y) {
	if (key == "q" || key == "Q") {
	    std::exit(0);
	} else {
	    draw();
	}
    }

    bool on_timeout() {
	window.iterate();
	return true;
    }

    void draw(const Download& download, int row) {
	const int N = n;
	const int B = 1;
	const int W = 800;
	const int H = 600;
	const int RW = W;
	const int DW = W - 2*B;
	const int RH = 600 / N;
	const int DH = RH - 2*B;
	const int TX = 10;
	const int TY = DH - 3;

	window.set_foreground(0, 0, 0);
	window.draw_rectangle(B, row*RH + B, DW, DH);

	std::list<Download::Chunk> chunks = download.get_chunks();
	std::list<Download::Chunk>::iterator i = chunks.begin();
	for(; i != chunks.end(); i++) {
	    int x1 = static_cast<int>((i->points.first / 
				       (double)download.get_total()) * DW);
	    int x2 = static_cast<int>((i->points.second / 
				       (double)download.get_total()) * DW);

	    if (i->changed) {
		window.set_foreground(0xff, 0, 0);
	    } else {
		window.set_foreground(0xff, 0xff, 0xff);
	    }
	    
	    window.set_mode(x::Window::DRAW);
	    window.fill_rectangle(B + x1, row*RH + B, B + x2 - x1, DH);
	}

	window.set_foreground(0xff, 0xff, 0xff);
	window.set_mode(x::Window::DRAW);
	window.draw_rectangle(B, row*RH + B, DW, DH);
	window.move(TX, row*RH + TY);
	//window.set_mode(x::Window::DRAW);
	window.set_foreground(0, 0, 0xff);
	window.draw_string(download.get_filename());
    }

    bool draw() {
	window.clear();

	std::set<poisoned::Download>& downloads = daemon.get_downloads();
	std::set<poisoned::Download>::iterator di = downloads.begin();
	for(int i = 0; i < n && di != downloads.end(); i++,di++) {
	    Download download(*di);
	    draw(download, i);
	}
	
	return true;
    }

    bool xdraw() {
	std::set<poisoned::Download>& downloads = daemon.get_downloads();
	std::set<poisoned::Download>::iterator di = downloads.begin();
	for(int i = 0; i < n && di != downloads.end(); i++,di++) {
	    Download download(*di);

	    if (download.changed()) {
		download.load();
		draw(download, i);
	    }
	}
	
	return true;
    }

    Show(int num) 
	: n(num),
	  window("Top " + util::string_value(n) + " Poisoned Downloads", 
		 800, 600)
    {
	window.center();
	window.key_press.connect(sigc::mem_fun(this, &Show::on_key_press));

	daemon.load();
	if (n > daemon.get_downloads().size()) {
	    n = daemon.get_downloads().size();
	}

	xdraw();

	Glib::signal_timeout().
	    connect(sigc::mem_fun(this, &Show::on_timeout), 66);
	Glib::signal_timeout().
	    connect(sigc::mem_fun(this, &Show::xdraw), 1000);
    }

private:
    int n;
    giftd daemon;
    x::Window window;
};

}
}

using namespace jlib;

int main(int argc, char** argv) {
    try {
	Glib::RefPtr<Glib::MainLoop> loop = Glib::MainLoop::create();
	poisoned::Show show(23);

	loop->run();
    }
    catch(std::exception& e) {
        std::cerr << "caught exception: "<<e.what() << std::endl;
    }
}
