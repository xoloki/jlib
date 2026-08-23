/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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
 * The equation of music is simple.  Take the natural base, raise 2 to the 
 * power of the octave, raise 2 to the power of the step as a fraction 
 * of the full scale, then multiply it all together.  This gives the frequency:
 * 
 *     55 * 2^octave * 2^(step/(2*2*3))
 *
 * Hail Eris!
 *
 */

#ifndef JLIB_MEDIA_NOTESTREAM_HH
#define JLIB_MEDIA_NOTESTREAM_HH

#include <iostream>
#include <exception>
#include <sstream>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>


#include <errno.h>
#include <sys/types.h>
#include <netinet/in.h>

#include <jlib/media/datastream.hh>
#include <jlib/media/instrument.hh>
#include <jlib/media/voice.hh>
#include <jlib/util/util.hh>

namespace jlib {
    namespace media {
        
        //typedef enum { A1=110, A2=220, A3=440, A4=880 } note;
        
        template< typename charT, typename traitT = std::char_traits<charT> >
        class basic_notebuf : public basic_databuf<charT,traitT> {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = std::string("jlib::media::basic_notebuf::exception")+(msg==""?"":": ")+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };
            
            typedef charT 					            char_type;
            typedef traitT 					            traits_type;
            typedef typename traits_type::int_type 		int_type;
            typedef typename traits_type::pos_type 		pos_type;
            typedef typename traits_type::off_type 		off_type;
            
            static const unsigned int BUF_SIZE = 1024;
            
            /**
             * default ctor, note will be A1 (110 Hz)
             */
            basic_notebuf(); 
            /**
             * parse note string like A#1
             */
            basic_notebuf(std::string note);
            /**
             * create note of the given freqency
             */
            basic_notebuf(double freq);
            /**
             * create note of the given step above the base freqency
             */
            basic_notebuf(int step, double base);
            
            //virtual int_type underflow();
            //virtual int_type overflow(int_type c=traits_type::eof());
            virtual int_type sync();

            std::string get_note() const; 
            double get_freq() const;
            double get_time() const; 

            void set_time(double time);

            /**
             * set duration, but frob time until you get an complete waveform
             */
            void set_nearest_time(double time);

            void set_note(std::string s);
            void set_note(double freq);
            void set_note(int step, double base);

            virtual void set_bits_per_sample(int s); 
            virtual void set_channels(int s); 
            virtual void set_samples_per_sec(int s); 
            virtual void set_format(int s); 

            static double get_freq(int step, double base);

            /**
             * How the note sounds, as against which note it is.
             *
             * Setting it regenerates, like the other setters here.  A note
             * string may override parts of it for itself; see set_note.
             */
            const instrument& get_instrument() const;
            void set_instrument(const instrument& i);

        protected:
            std::string create_data(double freq) const;

            instrument m_instrument;
            double m_freq;
            double m_time;
            std::string m_note;
        };
        
        template<typename charT, typename traitT=std::char_traits<charT> >
        class basic_notestream : public basic_datastream<charT,traitT> {
        public:
            basic_notestream();
            basic_notestream(std::string note);
            basic_notestream(double freq);
            basic_notestream(int step, double base);
            
            std::string get_note() const; 
            void set_note(std::string s);
            void set_note(double freq);
            void set_note(int step, double base);

            double get_time() const; 
            void set_time(double time);
            void set_nearest_time(double time);

            double get_freq() const;

            const instrument& get_instrument() const;
            void set_instrument(const instrument& i);

        protected:
            /** The buffer, or throw.  Every forwarder below wants this. */
            basic_notebuf<charT,traitT>* buf() const;
        };
        
        typedef basic_notestream<char> notestream;

        
        template< typename charT, typename traitT >
        inline
        basic_notebuf<charT,traitT>::basic_notebuf() 
            : basic_databuf<charT,traitT>(),
              m_freq(0),
              m_time(1)
        {
            // "A" and not "A1".  The grammar became STEP@OCTAVE:BEATS, so "A1"
            // reaches the two-character check and throws "two char notes must
            // be either 'Ab' or 'C#'" -- this constructor could not be called
            // at all.  It never was, in the tree or the tests, which is why
            // nobody noticed.
            set_note("A");
        }
        
        template< typename charT, typename traitT >
        inline
        basic_notebuf<charT,traitT>::basic_notebuf(std::string note) 
            : basic_databuf<charT,traitT>(),
              m_freq(0),
              m_time(1)
        {
            set_note(note);
        }
        
        template< typename charT, typename traitT >
        inline
        basic_notebuf<charT,traitT>::basic_notebuf(double freq) 
            : basic_databuf<charT,traitT>(),
              m_freq(0),
              m_time(1)
        {
            // m_time initialized before this, not after.  set_note(double)
            // generates immediately, and it used to read an m_time that nothing
            // had written -- so the sample count came from whatever was on the
            // stack, and could be negative (generating nothing) or enormous
            // (allocating a string to match).  Every notestream note(440.0) in
            // the tests went through it.
            set_note(freq);
        }
        
        template< typename charT, typename traitT >
        inline
        basic_notebuf<charT,traitT>::basic_notebuf(int step, double base) 
            : basic_databuf<charT,traitT>(),
              m_freq(0),
              m_time(1)
        {
            // as above: m_time before, not after
            set_note(step,base);
        }

        template< typename charT, typename traitT >
        inline
        basic_notebuf<charT,traitT>* basic_notestream<charT,traitT>::buf() const {
            typedef typename basic_notebuf<charT,traitT>::exception oops;

            if(!this->m_buf)
                throw oops("m_buf == null");

            basic_notebuf<charT,traitT>* b =
                dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf.get());

            if(!b)
                throw oops("buf == null");

            return b;
        }

        template< typename charT, typename traitT >
        inline
        double basic_notestream<charT,traitT>::get_freq() const {
            return buf()->get_freq();
        }

        template< typename charT, typename traitT >
        inline
        const instrument& basic_notestream<charT,traitT>::get_instrument() const {
            return buf()->get_instrument();
        }

        template< typename charT, typename traitT >
        inline
        void basic_notestream<charT,traitT>::set_instrument(const instrument& i) {
            buf()->set_instrument(i);
        }

        template< typename charT, typename traitT >
        inline
        double basic_notebuf<charT,traitT>::get_freq() const {
            // Declared since forever and never defined, so any call failed to
            // link -- which is why set_nearest_time() below has never been
            // usable and jnote.cc:72 has its call commented out.
            return m_freq;
        }

        template< typename charT, typename traitT >
        inline
        const instrument& basic_notebuf<charT,traitT>::get_instrument() const {
            return m_instrument;
        }

        template< typename charT, typename traitT >
        inline
        void basic_notebuf<charT,traitT>::set_instrument(const instrument& i) {
            m_instrument = i;
            this->set_data(create_data(m_freq));
        }

        template< typename charT, typename traitT >
        inline
        std::string basic_notebuf<charT,traitT>::get_note() const {
            return m_note;
        }
        
        template< typename charT, typename traitT >
        inline
        void basic_notebuf<charT,traitT>::set_note(std::string note) {
            // note format is STEP@OCTAVE:BEATS[/WAVE]

            // default to third octave and one beat
            int octave = 3;
            double beats = 1;

            // A waveform for this note only, overriding the instrument's:
            //
            //     A#@4:2/saw
            //
            // Taken off the front of the parse rather than the back of it, so
            // the delimiters below see the string they always did.  Only the
            // waveform is expressible here on purpose -- it is the one thing
            // worth saying per note, and an ADSR written out as four numbers in
            // a string is the point at which this wants escaping and a grammar,
            // rather than a suffix.  Everything else is a setter.
            std::size_t wpos = note.find("/");
            if(wpos != std::string::npos) {
                const std::string w = note.substr(wpos+1);

                note = note.substr(0, wpos);

                // throws by name, as the step parser below does
                m_instrument.set_wave(instrument::wave_from_name(w));
            }
            
            // first parse the beats
            std::size_t bpos = note.find(":");
            if(bpos != std::string::npos) {
                std::string b = note.substr(bpos+1);
                
                note = note.substr(0, bpos);
                
                std::stringstream ss(b);
                
                ss >> beats;
            }
            
            // next parse the octave 
            std::size_t opos = note.find("@");
            if(opos != std::string::npos) {
                std::string o = note.substr(opos+1);
                note = note.substr(0, opos);
                
                std::stringstream ss(o);
                
                ss >> octave;
            }
            
            if(note.empty()) {
                throw std::runtime_error("empty notes are always wrong");
            }
            
            // upcase so we can parse the step
            note[0] = std::toupper(note[0]);

            // true base is 55.  Hail Eris!
            double base = 55;
            int step = 0;
            
            // 12 steps per octave, but the stepping is irregular, so need lookup table
            if(note[0] == 'A') {
                step = 0;
            } else if(note[0] == 'B') {
                step = 2;
            } else if(note[0] == 'C') {
                step = 3;
            } else if(note[0] == 'D') {
                step = 5;
            } else if(note[0] == 'E') {
                step = 7;
            } else if(note[0] == 'F') {
                step = 8;
            } else if(note[0] == 'G') {
                step = 10;
            } else if(note[0] == 'R') {
                // resting base is zero
                base = 0;
            } else {
                throw std::runtime_error("unknown step '" + note + "' must be [ABCDEFGR]");
            }
            
            if(note.size() > 1) {
                // was "!note[1] == 'b'": the ! binds to note[1] alone, so this
                // compared a bool against 'b' and was always false, meaning
                // the whole condition was false and the throw never fired.
                //
                // And the test was == 2 rather than > 1, so anything longer
                // fell through with its accidental silently ignored: "C##" was
                // a C, not a D.  Reject the length as well as the character.
                if(note.size() > 2) {
                    throw std::runtime_error("note '" + note +
                                             "' has more than one accidental");
                }

                if(note[1] != '#' && note[1] != 'b') {
                    throw std::runtime_error("two char notes must be either 'Ab' or 'C#'");
                }
                
                if(note[1] == '#') {
                    step++;
                } else if(note[1] == 'b') {
                    step--;
                }
            }
            
            m_note = note;
            
            // start at the true base and get to the right octave
            base = base * std::pow(2, octave);
            
            m_freq = get_freq(step, base);
            
            this->set_time(beats);
        }
        
        template< typename charT, typename traitT >
        inline
        void basic_notebuf<charT,traitT>::set_note(double freq) {
            this->m_freq = freq;
            
            this->set_data(this->create_data(this->m_freq));
        }

        template< typename charT, typename traitT >
        inline
        void basic_notebuf<charT,traitT>::set_note(int step, double base) {
            set_note(get_freq(step,base));
        }

        template< typename charT, typename traitT >
        inline
        double basic_notebuf<charT,traitT>::get_freq(int step, double base) {
            return (base*pow(2,(step/12.)));
        }

        template< typename charT, typename traitT >
        inline
        double basic_notebuf<charT,traitT>::get_time() const {
            return m_time;
        }

        template< typename charT, typename traitT >
        inline
        void basic_notebuf<charT,traitT>::set_time(double time) {
            m_time = time;
            this->set_data(this->create_data(this->m_freq));
        }

        template< typename charT, typename traitT >
        inline
        void basic_notebuf<charT,traitT>::set_nearest_time(double time) {
	    double cycles = get_freq() * time;
            int cycle_round = static_cast<int>(std::ceil(cycles));
            time = cycle_round / get_freq();

            m_time = time;
            this->set_data(this->create_data(this->m_freq));
        }

        template< typename charT, typename traitT >
        inline
        std::string basic_notebuf<charT,traitT>::create_data(double freq) const {
            const int samples = (int)(this->get_samples_per_sec() * this->get_time());
            const int bytes_per_sample = (this->get_bits_per_sample()/8);
            const int size = samples * bytes_per_sample * this->get_channels();

            u_int64_t amp = (u_int64_t)pow(2,this->get_bits_per_sample()-1);

            if(size <= 0)
                return std::string();

            std::string data(size,'\0');
                
            if(getenv("JLIB_MEDIA_NOTESTREAM_DEBUG")) {
                std::cerr << "freq             " << freq << std::endl;
                std::cerr << "samples          " << samples << std::endl;
                std::cerr << "bits_per_sample  " << this->get_bits_per_sample() << std::endl;
                std::cerr << "bytes_per_sample " << bytes_per_sample << std::endl;
                std::cerr << "size             " << size << std::endl;
                std::cerr << "channels         " << this->get_channels() << std::endl;
            }

            // The instrument makes the sound; this function only writes it
            // down.  What used to be here was a sine and a fixed 5ms fade --
            // that fade being a degenerate envelope, instant attack, no decay,
            // full sustain, 5ms release, and it existed because a note lasts
            // get_time() seconds whatever its frequency, so freq*time is hardly
            // ever a whole number of cycles.  The waveform stopped wherever it
            // happened to be and dropped straight to silence: at 443.7Hz for
            // one second the last sample was -20287 out of a 21823 peak, a 93%
            // full-scale step, which is a click, and a melody is a train of
            // them.
            //
            // The ADSR subsumes it and inherits the requirement -- see
            // instrument::clamped(), which will not let attack or release go
            // below the 5ms that made this work.
            voice v(m_instrument, freq, static_cast<unsigned long>(samples),
                    static_cast<double>(this->get_samples_per_sec()));

            for(int i=0;i<samples;i++) {

                // The waveform in [-1,1], before it is committed to any
                // particular sample format.  PCM_FLOAT32 wants exactly this;
                // the integer formats scale and quantize it below.
                //
                // Clamped, because it is not guaranteed to arrive in range: a
                // truncated Fourier series overshoots at a discontinuity by
                // about 9%, and the waveforms are scaled to match each other in
                // RMS rather than in peak, so a sawtooth reaches about 1.43
                // before gain.  At the default gain that is 0.95 and nothing
                // happens, but at a gain of 1 it would wrap rather than clip --
                // llround of an out-of-range value straight into an int16.
                double raw = v.next();
                if(raw >  1.0) raw =  1.0;
                if(raw < -1.0) raw = -1.0;

                // Round rather than truncate.  A cast truncates toward zero,
                // which both doubles the worst-case quantization error and
                // makes it asymmetric: every value in (-1,1) collapses to 0,
                // so the waveform sits flat through each zero crossing
                // instead of passing through it.  That deadband is crossover
                // distortion, and at 8 bits -- where the whole signal is only
                // ~170 levels wide -- it is clearly audible.
                int64_t sample = std::llround(raw * amp);
                u_int64_t usample = sample+amp;
                
                
                for(int j=0;j<this->get_channels();j++) {
                    u_int32_t p0 = (bytes_per_sample*this->get_channels()*i)+j*bytes_per_sample;
                    u_int32_t p1 = (bytes_per_sample*this->get_channels()*i)+j*bytes_per_sample+1;

                    //char_type& c = data[(bytes_per_sample*this->get_channels()*i)+j*bytes_per_sample];
                    //char_type& d = data[(bytes_per_sample*this->get_channels()*i)+j*bytes_per_sample+1];

                    if(this->get_format() == Type::PCM_U8) {
                        u_char s = (usample) & 0x00ff;
                        jlib::util::byte_copy(data,&s,1,p0);
                    }
                    else if(this->get_format() == Type::PCM_S8) {
                        if(sample > (pow(2,8)-1) || sample < (-pow(2,8)))
                            throw exception("sample out of bounds at Type::PCM_S8");
                        char s = sample;
                        jlib::util::byte_copy(data,&s,1,p0);
                    }
                    else if(this->get_format() == Type::PCM_FLOAT32) {
                        // The shortest branch here, and the only one that
                        // quantizes nothing: no bias, no byte order, no
                        // clipping.  It is also what the hardware actually
                        // wants -- CoreAudio works in float32 internally, and
                        // PortAudio treats paFloat32 as its native format --
                        // so this is the one path with no conversion at all.
                        Type::scaled f = static_cast<Type::scaled>(raw);
                        jlib::util::byte_copy(data, &f, sizeof(f), p0);
                    }
                    else if(this->get_format() == Type::PCM_U16_LE) {
                        u_int16_t u = htons(usample);
                        char* v = reinterpret_cast<char*>(&u);

                        jlib::util::byte_copy(data,v+1,1,p0);
                        jlib::util::byte_copy(data,v+0,1,p1);
                        //c = (s & 0x00ff);
                        //d = (s & 0xff00) >> 8;
                    }
                    else if(this->get_format() == Type::PCM_U16_BE) {
                        u_int16_t u = htons(usample);
                        char* v = reinterpret_cast<char*>(&u);

                        jlib::util::byte_copy(data,v+0,2,p0);
                        //jlib::util::byte_copy(data,v+1,1,p1);
                        //c = (s & 0xff00) >> 8;
                        //d = (s & 0x00ff);
                    }
                    else if(this->get_format() == Type::PCM_S16_LE) {
                        int16_t s = sample;
                        u_int16_t u = htons(static_cast<u_int16_t>(s));
                        char* v = reinterpret_cast<char*>(&u);
                        
                        jlib::util::byte_copy(data,v+1,1,p0);
                        jlib::util::byte_copy(data,v+0,1,p1);

                        if(getenv("JLIB_MEDIA_NOTESTREAM_DEBUG") && this->get_channels() == 2) {
                            std::cerr << "sample = " << std::dec << i << std::endl;
                            std::cerr << "static_cast<u_int16_t>(s) = " << std::hex << static_cast<u_int16_t>(s) << " " << std::dec << static_cast<u_int16_t>(s) << std::endl;
                            std::cerr << "channel = " << j << std::endl;
                            std::cerr << "s = " << std::hex << s << " " << std::dec << s << std::endl;
                            std::cerr << "u = " << std::hex << u << " " << std::dec << u << std::endl;
                            std::cerr << "data["<<std::dec << p0<<"] = " << std::hex << (int(data[p0]) & 0xff) << std::endl;
                            std::cerr << "data["<<std::dec << p1<<"] = " << std::hex << (int(data[p1]) & 0xff) << std::endl << std::endl;
                        }                            

                        //c = (s & 0x00ff);
                        //d = (s & 0xff00) >> 8;
                    }
                    else if(this->get_format() == Type::PCM_S16_BE) {
                        int16_t s = sample;
                        u_int16_t u = htons(s);
                        char* v = reinterpret_cast<char*>(&u);

                        jlib::util::byte_copy(data,v+0,2,p0);
                        //jlib::util::byte_copy(data,v+1,1,p1);
                        //int16_t s = sample - amp;
                        //c = (s & 0xff00) >> 8;
                        //d = (s & 0x00ff);
                    }
                    else {
                        throw exception("bad format in create_data()");
                    }
                }
            }

            return data;
        }

        template< typename charT, typename traitT >
        inline
        typename basic_notebuf<charT,traitT>::int_type 
        basic_notebuf<charT,traitT>::sync() {
            return traits_type::eof();
        }



        template< typename charT, typename traitT >
        inline
        void basic_notebuf<charT,traitT>::set_bits_per_sample(int s) {
            basic_streambuf<charT,traitT>::set_bits_per_sample(s);
            this->set_data(this->create_data(this->m_freq));
        }

        template< typename charT, typename traitT >
        inline
        void basic_notebuf<charT,traitT>::set_channels(int s) {
            basic_streambuf<charT,traitT>::set_channels(s);
            this->set_data(this->create_data(this->m_freq));
        }

        template< typename charT, typename traitT >
        inline
        void basic_notebuf<charT,traitT>::set_samples_per_sec(int s) {
            basic_streambuf<charT,traitT>::set_samples_per_sec(s);
            this->set_data(this->create_data(this->m_freq));
        }

        template< typename charT, typename traitT >
        inline
        void basic_notebuf<charT,traitT>::set_format(int s) {
            basic_streambuf<charT,traitT>::set_format(s);
            this->set_data(this->create_data(this->m_freq));
        }





        template< typename charT, typename traitT >
        inline
        basic_notestream<charT,traitT>::basic_notestream() 
            : basic_datastream<charT,traitT>()
        {
            this->m_buf.reset(new basic_notebuf<charT,traitT>());
            this->init(this->m_buf.get());
        }
        
        template< typename charT, typename traitT >
        inline
        basic_notestream<charT,traitT>::basic_notestream(std::string note) 
            : basic_datastream<charT,traitT>()
        {
            this->m_buf.reset(new basic_notebuf<charT,traitT>(note));
            this->init(this->m_buf.get());
        }
        
        template< typename charT, typename traitT >
        inline
        basic_notestream<charT,traitT>::basic_notestream(double freq) 
            : basic_datastream<charT,traitT>()
        {
            this->m_buf.reset(new basic_notebuf<charT,traitT>(freq));
            this->init(this->m_buf.get());
        }

        template< typename charT, typename traitT >
        inline
        basic_notestream<charT,traitT>::basic_notestream(int step, double base) 
            : basic_datastream<charT,traitT>()
        {
            this->m_buf.reset(new basic_notebuf<charT,traitT>(step, base));
            this->init(this->m_buf.get());
        }

        template< typename charT, typename traitT >
        inline
        std::string
        basic_notestream<charT,traitT>::get_note() const
        {
            if(!this->m_buf)
                throw typename basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf.get());
            if(buf)
                return buf->get_note();
            else
                throw typename basic_notebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_notestream<charT,traitT>::set_note(std::string note) 
        {
            if(!this->m_buf)
                throw typename basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf.get());
            if(buf)
                buf->set_note(note);
            else
                throw typename basic_notebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_notestream<charT,traitT>::set_note(double freq) 
        {
            if(!this->m_buf)
                throw typename basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf.get());
            if(buf)
                buf->set_note(freq);
            else
                throw typename basic_notebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_notestream<charT,traitT>::set_note(int step, double base) 
        {
            if(!this->m_buf)
                throw typename basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf.get());
            if(buf)
                buf->set_note(step,base);
            else
                throw typename basic_notebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        double
        basic_notestream<charT,traitT>::get_time() const
        {
            if(!this->m_buf)
                throw typename basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf.get());
            if(buf)
                return buf->get_time();
            else
                throw typename basic_notebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_notestream<charT,traitT>::set_time(double time) 
        {
            if(!this->m_buf)
                throw typename basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf.get());
            if(buf)
                buf->set_time(time);
            else
                throw typename basic_notebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_notestream<charT,traitT>::set_nearest_time(double time) 
        {
            if(!this->m_buf)
                throw typename basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf.get());
            if(buf)
                buf->set_nearest_time(time);
            else
                throw typename basic_notebuf<charT,traitT>::exception("buf == null");
        }

    }
}


#endif // JLIB_MEDIA_STREAM_HH
