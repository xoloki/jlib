// -*- C++ -*-

/* pattern.hh
 * 
 * Copyright (C) 2002 Joey Yandle
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

#ifndef JLIB_MEDIA_PLAYLIST_HH
#define JLIB_MEDIA_PLAYLIST_HH

#include <vector>
#include <string>

#include <cmath>

#include <jlib/media/stream.hh>

namespace jlib {
    namespace media {
        
        class Roll {
        public:
            typedef std::vector<bool> rep_type;
            
            typedef rep_type::pointer pointer;
            typedef rep_type::const_pointer const_pointer;
            typedef rep_type::reference reference;
            typedef rep_type::const_reference const_reference;
            typedef rep_type::iterator iterator;
            typedef rep_type::const_iterator const_iterator; 
            typedef rep_type::reverse_iterator reverse_iterator;
            typedef rep_type::const_reverse_iterator const_reverse_iterator;
            typedef rep_type::size_type size_type;
            typedef rep_type::difference_type difference_type;
            typedef rep_type::allocator_type allocator_type;            
            
            Roll(int id, stream* s, std::string name, std::string sample, std::string data);
            Roll();
            
            int get_id() const;
            void set_id(int id);
            
            std::string get_name() const;
            void set_name(std::string name);
            
            std::string get_sample() const;
            void set_sample(std::string sample);
            
            reference operator[](int i);
            const_reference operator[](int i) const;
            
            stream* get_stream();
            const stream* get_stream() const;
            void set_stream(stream* s);
            
        private:
            rep_type m_pattern;
            
            stream* m_stream;
            
            int m_id;
            std::string m_sample;
            std::string m_name;
            int m_beats;
            int m_bpu;
        };
        
        class Pattern {
        public:
            typedef std::vector<Roll> rep_type;
            
            typedef rep_type::pointer pointer;
            typedef rep_type::const_pointer const_pointer;
            typedef rep_type::reference reference;
            typedef rep_type::const_reference const_reference;
            typedef rep_type::iterator iterator;
            typedef rep_type::const_iterator const_iterator; 
            typedef rep_type::reverse_iterator reverse_iterator;
            typedef rep_type::const_reverse_iterator const_reverse_iterator;
            typedef rep_type::size_type size_type;
            typedef rep_type::difference_type difference_type;
            typedef rep_type::allocator_type allocator_type;            
            
            Pattern(int id, std::string name);
            virtual ~Pattern();
            
            int get_id() const;
            void set_id(int id);
            
            std::string get_name() const;
            void set_name(std::string id);
            
            int get_next_roll_id();
            
            reference operator[](int i);
            const_reference operator[](int i) const;
            
            void push_back(const_reference r);
            
            iterator begin() { return m_rolls.begin(); }
            const_iterator begin() const { return m_rolls.begin(); }
            iterator end() { return m_rolls.end(); }
            const_iterator end() const { return m_rolls.end(); }
            reverse_iterator rbegin() { return m_rolls.rbegin(); }
            const_reverse_iterator rbegin() const { return m_rolls.rbegin(); }
            reverse_iterator rend() { return m_rolls.rend(); }
            const_reverse_iterator rend() const { return m_rolls.rend(); }
            bool empty() const { return m_rolls.empty(); }
            size_type size() const { return m_rolls.size(); }
            
        protected:
            rep_type m_rolls;
            int m_id;
            std::string m_name;
            int m_roll_id_max;
        };

        class PlayList {
        public:
            typedef std::vector<Pattern> slice_type;
            typedef std::vector<slice_type> rep_type;
            
            typedef rep_type::pointer pointer;
            typedef rep_type::const_pointer const_pointer;
            typedef rep_type::reference reference;
            typedef rep_type::const_reference const_reference;
            typedef rep_type::iterator iterator;
            typedef rep_type::const_iterator const_iterator; 
            typedef rep_type::reverse_iterator reverse_iterator;
            typedef rep_type::const_reverse_iterator const_reverse_iterator;
            typedef rep_type::size_type size_type;
            typedef rep_type::difference_type difference_type;
            typedef rep_type::allocator_type allocator_type;            

            PlayList();
            PlayList(int id, std::string name);
            virtual ~PlayList();

            int get_id() const;
            void set_id(int id);
            
            std::string get_name() const;
            void set_name(std::string id);

            int get_bpm() const;
            void set_bpm(int bpm);
            
            int get_width() const;
            void set_width(int width);
            
            int get_measure() const;
            void set_measure(int measure);

            int get_next_pattern_id();

            reference operator[](int i);
            const_reference operator[](int i) const;
            
            void push_back(const_reference r);
            
            std::string render(int fmt, slice_type slice);

            template<int N>
            std::string render(slice_type slice);

            iterator begin() { return m_slices.begin(); }
            const_iterator begin() const { return m_slices.begin(); }
            iterator end() { return m_slices.end(); }
            const_iterator end() const { return m_slices.end(); }
            reverse_iterator rbegin() { return m_slices.rbegin(); }
            const_reverse_iterator rbegin() const { return m_slices.rbegin(); }
            reverse_iterator rend() { return m_slices.rend(); }
            const_reverse_iterator rend() const { return m_slices.rend(); }
            bool empty() const { return m_slices.empty(); }
            size_type size() const { return m_slices.size(); }

        protected:
            rep_type m_slices;

            int m_id;
            std::string m_name;
            int m_bpm;
            int m_width;
            int m_measure;
            int m_pattern_id_max;
            
        };
        
        template<int N>
        inline
        std::string PlayList::render(slice_type slice) {
            if(getenv("JLIB_MEDIA_PLAYLIST_DEBUG"))
                std::cerr << "void jlib::media::PlayList::render(): enter" << std::endl;

            int ticks_per_minute = get_measure()*get_bpm();
            int m_samples_per_sec = 44100;

            int samples_per_tick = (int)(m_samples_per_sec / (ticks_per_minute / (double)60));

            const int n = (int)(((double)get_width() / (double)ticks_per_minute)*60*m_samples_per_sec);

            Type::scaled samples[n];// = new Type::sample<N>::buf[n];
            typename Type::sample<N>::buf samples_out[n];// = new Type::sample<N>::buf[n];
            memset(samples,0,n*sizeof(Type::scaled));
            memset(samples_out,0,n*sizeof(typename Type::sample<N>::buf));

            std::string data;
            
            if(getenv("JLIB_MEDIA_PLAYLIST_DEBUG")) 
                std::cerr << "\tnumber of samples: " << n << std::endl
                          << "\tsamples per sec:   " << m_samples_per_sec << std::endl
                          << "\tsamples per tick:  " << samples_per_tick << std::endl
                          << "\tticks per minute:  " << ticks_per_minute << std::endl
                          << "\tticks per second:  " << (ticks_per_minute * 60) << std::endl
                          << "\ttotal seconds:     " << (n / (double)m_samples_per_sec) << std::endl;
            
            slice_type::iterator i = slice.begin();
            for(;i!=slice.end();i++) {
                int r = 0;
                Pattern::iterator j = i->begin();
                for(;j!=i->end();j++) {
                    Pattern::reference roll = *j;
                    stream* s = j->get_stream();

                    for(u_int k=0;k<get_width();k++) {
                        if(roll[k]) {
                            s->rewind();

                            int sample_begin = samples_per_tick * k;
                            int sample_count = (s->get_length() / (s->get_bits_per_sample() / 8));
                            
                            if(getenv("JLIB_MEDIA_PLAYLIST_DEBUG")) 
                                std::cerr << "\t\tbeat: " << k << std::endl
                                          << "\t\tbegin:  " << sample_begin  << std::endl
                                          << "\t\tcount:  " << sample_count  << std::endl;
                            
                            for(int x=0;x<(sample_count) && (*s);x++) {
                                samples[(x+sample_begin)%n] += s->get_scaled();
                            }
                        }
                    }
                    
                    r++;
                }
            }

            Type::scaled max = 1.0;
            
            for(int z=0;z<n;z++)
                if(std::abs(samples[z]) > max)
                    max = std::abs(samples[z]);
            
            // scale to 
            if(max > 1.0) {
                if(getenv("JLIB_MEDIA_PLAYLIST_DEBUG")) 
                    std::cerr << "\tmax > 1.0: " << max << std::endl;
                for(int z=0;z<n;z++)
                    samples[z] /= max;
            }
            
            for(int z=0;z<n;z++)
                samples_out[z] = Type::sample<N>::descale(samples[z]);

            data.assign((char*)samples_out, n*sizeof(typename Type::sample<N>::buf));
            
            if(getenv("JLIB_MEDIA_PLAYLIST_DEBUG"))
                std::cerr << "void jlib::media::PlayList::render(): leave" << std::endl;
            
            return data;
        }


    }

}    
#endif //JLIB_MEDIA_PLAYLIST_HH
    
