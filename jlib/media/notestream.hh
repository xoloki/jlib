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
#include <string>
#include <cstring>
#include <cmath>

#include <bits/char_traits.h>

#include <errno.h>
#include <sys/soundcard.h>
#include <sys/types.h>
#include <netinet/in.h>

#include <jlib/media/datastream.hh>
#include <jlib/util/util.hh>

namespace jlib {
    namespace media {
        
        //typedef enum { A1=110, A2=220, A3=440, A4=880 } note;
        
        template< typename charT, typename traitT = std::char_traits<charT> >
        class basic_notebuf : public basic_databuf<charT,traitT> {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = std::string("jlib::media::basic_notebuf::exception")+(msg==""?"":": ")+msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
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
        protected:
            std::string create_data(double freq) const;

            double m_freq;
            double m_time;
            std::string m_note;
            std::string m_data;
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

        protected:
        };
        
        typedef basic_notestream<char> notestream;

        
        template< typename charT, typename traitT >
        inline
        basic_notebuf<charT,traitT>::basic_notebuf() 
            : basic_databuf<charT,traitT>()
        {
            set_note("A1");
            set_time(1);
        }
        
        template< typename charT, typename traitT >
        inline
        basic_notebuf<charT,traitT>::basic_notebuf(std::string note) 
            : basic_databuf<charT,traitT>()
        {
            set_note(note);
        }
        
        template< typename charT, typename traitT >
        inline
        basic_notebuf<charT,traitT>::basic_notebuf(double freq) 
            : basic_databuf<charT,traitT>()
        {
            set_note(freq);
            set_time(1);
        }
        
        template< typename charT, typename traitT >
        inline
        basic_notebuf<charT,traitT>::basic_notebuf(int step, double base) 
            : basic_databuf<charT,traitT>()
        {
            set_note(step,base);
            set_time(1);
        }

        template< typename charT, typename traitT >
        inline
        std::string basic_notebuf<charT,traitT>::get_note() const {
            return m_note;
        }
        
        template< typename charT, typename traitT >
        inline
        void basic_notebuf<charT,traitT>::set_note(std::string note) {
            // note format is STEP@OCTAVE:BEATS

            // default to third octave and one beat
            int octave = 3;
            double beats = 1;
            
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
            
            double base = 110;
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
                base = 0;
            } else {
                throw std::runtime_error("unknown step '" + note + "' must be [ABCDEFGR]");
            }
            
            if(note.size() == 2) {
                if(note[1] != '#' && !note[1] == 'b') {
                    throw std::runtime_error("two char notes must be either 'Ab' or 'C#'");
                }
                
                if(note[1] == '#') {
                    step++;
                } else if(note[1] == 'b') {
                    step--;
                }
            }
            
            m_note = note;
            
            // unless we're resting, start at the first base and get to the right octave
            if(note[0] != 'R') {
                base = 55 * std::pow(2, octave);
            }
            
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
            double vol = 0.666;
            static const long double pi = 3.14159265358979323846264338;

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

            for(int i=0;i<samples;i++) {
                
                int64_t sample = (int64_t)((vol*amp*sin(freq*this->get_time()*(2*pi)*(i/double(samples)))));
                u_int64_t usample = sample+amp;
                
                
                for(int j=0;j<this->get_channels();j++) {
                    u_int32_t p0 = (bytes_per_sample*this->get_channels()*i)+j*bytes_per_sample;
                    u_int32_t p1 = (bytes_per_sample*this->get_channels()*i)+j*bytes_per_sample+1;

                    //char_type& c = data[(bytes_per_sample*this->get_channels()*i)+j*bytes_per_sample];
                    //char_type& d = data[(bytes_per_sample*this->get_channels()*i)+j*bytes_per_sample+1];

                    if(this->get_format() == AFMT_U8) {
                        u_char s = (usample) & 0x00ff;
                        jlib::util::byte_copy(data,&s,1,p0);
                    }
                    else if(this->get_format() == AFMT_S8) {
                        if(sample > (pow(2,8)-1) || sample < (-pow(2,8)))
                            throw exception("sample out of bounds at AFMT_S8");
                        char s = sample;
                        jlib::util::byte_copy(data,&s,1,p0);
                    }
                    else if(this->get_format() == AFMT_U16_LE) {
                        u_int16_t u = htons(usample);
                        char* v = reinterpret_cast<char*>(&u);

                        jlib::util::byte_copy(data,&v+1,1,p0);
                        jlib::util::byte_copy(data,&v+0,1,p1);
                        //c = (s & 0x00ff);
                        //d = (s & 0xff00) >> 8;
                    }
                    else if(this->get_format() == AFMT_U16_BE) {
                        u_int16_t u = htons(usample);
                        char* v = reinterpret_cast<char*>(&u);

                        jlib::util::byte_copy(data,&v+0,2,p0);
                        //jlib::util::byte_copy(data,&v+1,1,p1);
                        //c = (s & 0xff00) >> 8;
                        //d = (s & 0x00ff);
                    }
                    else if(this->get_format() == AFMT_S16_LE) {
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
                    else if(this->get_format() == AFMT_S16_BE) {
                        int16_t s = sample;
                        u_int16_t u = htons(s);
                        char* v = reinterpret_cast<char*>(&u);

                        jlib::util::byte_copy(data,&v+0,2,p0);
                        //jlib::util::byte_copy(data,&v+1,1,p1);
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
            this->m_buf=new basic_notebuf<charT,traitT>();
            this->init(this->m_buf);
        }
        
        template< typename charT, typename traitT >
        inline
        basic_notestream<charT,traitT>::basic_notestream(std::string note) 
            : basic_datastream<charT,traitT>()
        {
            this->m_buf=new basic_notebuf<charT,traitT>(note);
            this->init(this->m_buf);
        }
        
        template< typename charT, typename traitT >
        inline
        basic_notestream<charT,traitT>::basic_notestream(double freq) 
            : basic_datastream<charT,traitT>()
        {
            this->m_buf=new basic_notebuf<charT,traitT>(freq);
            this->init(this->m_buf);
        }

        template< typename charT, typename traitT >
        inline
        basic_notestream<charT,traitT>::basic_notestream(int step, double base) 
            : basic_datastream<charT,traitT>()
        {
            this->m_buf=new basic_notebuf<charT,traitT>(step, base);
            this->init(this->m_buf);
        }

        template< typename charT, typename traitT >
        inline
        std::string
        basic_notestream<charT,traitT>::get_note() const
        {
            if(!this->m_buf)
                throw basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf);
            if(buf)
                return buf->get_note();
            else
                throw basic_notebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_notestream<charT,traitT>::set_note(std::string note) 
        {
            if(!this->m_buf)
                throw basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf);
            if(buf)
                buf->set_note(note);
            else
                throw basic_notebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_notestream<charT,traitT>::set_note(double freq) 
        {
            if(!this->m_buf)
                throw basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf);
            if(buf)
                buf->set_note(freq);
            else
                throw basic_notebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_notestream<charT,traitT>::set_note(int step, double base) 
        {
            if(!this->m_buf)
                throw basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf);
            if(buf)
                buf->set_note(step,base);
            else
                throw basic_notebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        double
        basic_notestream<charT,traitT>::get_time() const
        {
            if(!this->m_buf)
                throw basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf);
            if(buf)
                return buf->get_time();
            else
                throw basic_notebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_notestream<charT,traitT>::set_time(double time) 
        {
            if(!this->m_buf)
                throw typename basic_notebuf<charT,traitT>::exception("this->m_buf == null");
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf);
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
            basic_notebuf<charT,traitT>* buf = dynamic_cast< basic_notebuf<charT,traitT>* >(this->m_buf);
            if(buf)
                buf->set_nearest_time(time);
            else
                throw typename basic_notebuf<charT,traitT>::exception("buf == null");
        }

    }
}


#endif // JLIB_MEDIA_STREAM_HH
