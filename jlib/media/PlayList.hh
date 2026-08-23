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

#include <map>
#include <memory>
#include <vector>
#include <string>

#include <cmath>

#include <jlib/media/clip.hh>
#include <jlib/media/delayed.hh>
#include <jlib/media/instrument.hh>
#include <jlib/media/mixer.hh>
#include <jlib/media/sampler.hh>
#include <jlib/media/stream.hh>
#include <jlib/media/voice.hh>

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
            
            Roll(int id, stream* s, const std::string& name, const std::string& sample, const std::string& data);

            /**
             * A roll that sounds a synthesized note rather than a recording.
             *
             * note is a note string as notestream reads them -- "C@2", "A#@3:2",
             * "G@2/saw" -- so a pattern can name a pitch, a length and a
             * waveform the same way the rest of the library does, and an
             * unknown one throws here rather than at render time.
             *
             * The point of this is that the two kinds of roll are the same kind
             * of thing by the time they reach the mixer: one becomes a sampler
             * and the other a voice, both wrapped in a delayed to put them on
             * their beat, and nothing downstream can tell them apart.
             */
            Roll(int id, const std::string& note, const std::string& name, const std::string& data);

            Roll();
            
            int get_id() const;
            void set_id(int id);
            
            std::string get_name() const;
            void set_name(const std::string& name);
            
            std::string get_sample() const;
            void set_sample(const std::string& sample);
            
            reference operator[](int i);
            const_reference operator[](int i) const;
            
            stream* get_stream();
            const stream* get_stream() const;
            void set_stream(stream* s);

            /** Whether this sounds a note rather than a recording. */
            bool is_note() const;

            const instrument& get_instrument() const;
            void set_instrument(const instrument& i);

            /** Hz, and seconds, both taken from the note string. */
            double get_freq() const;
            double get_seconds() const;
            
        private:
            rep_type m_pattern;
            
            stream* m_stream;
            
            int m_id;
            std::string m_sample;
            std::string m_name;
            int m_beats;
            int m_bpu;

            bool m_is_note;
            instrument m_instrument;
            double m_freq;
            double m_seconds;
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
            
            Pattern(int id, const std::string& name);
            virtual ~Pattern();
            
            int get_id() const;
            void set_id(int id);
            
            std::string get_name() const;
            void set_name(const std::string& id);
            
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
            PlayList(int id, const std::string& name);
            virtual ~PlayList();

            int get_id() const;
            void set_id(int id);
            
            std::string get_name() const;
            void set_name(const std::string& id);

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

            const int ticks_per_minute = get_measure()*get_bpm();
            const int samples_per_sec = 44100;

            const int samples_per_tick = (int)(samples_per_sec / (ticks_per_minute / (double)60));

            const int n = (int)(((double)get_width() / (double)ticks_per_minute)*60*samples_per_sec);

            // Nothing to render, and every path below would be worse than
            // useless: the fold takes a remainder modulo n.
            if(n <= 0)
                return std::string();

            if(getenv("JLIB_MEDIA_PLAYLIST_DEBUG")) 
                std::cerr << "\tnumber of samples: " << n << std::endl
                          << "\tsamples per sec:   " << samples_per_sec << std::endl
                          << "\tsamples per tick:  " << samples_per_tick << std::endl
                          << "\tticks per minute:  " << ticks_per_minute << std::endl
                          << "\ttotal seconds:     " << (n / (double)samples_per_sec) << std::endl;

            // One decode per roll, however many times it is struck.
            //
            // This used to rewind the roll's stream and read it again for each
            // hit, which is the same samples decoded over and over -- and it
            // did not work at all for a wav-backed roll, because rewind() on a
            // wavstream set failbit and every read after it returned nothing.
            // That is fixed in wavstream.hh; this no longer depends on it.
            std::map<stream*, std::shared_ptr<clip> > decoded;

            // Sources, not a hand-rolled sum.  The gain staging and the limiter
            // replace what used to be here: the peak over the whole pattern,
            // with everything divided by it if it came out above one.  That is
            // the shape of mistake this library has made before -- one loud hit
            // anywhere quietly dropped the level of the entire bar, and nothing
            // put it back -- and the mixer holds the level steady instead.
            mixer mix;
            mix.set_staging(mixer::staging::automatic);
            mix.set_rate(samples_per_sec);

            unsigned long len = (unsigned long)n;

            slice_type::iterator i = slice.begin();
            for(;i!=slice.end();i++) {
                Pattern::iterator j = i->begin();
                for(;j!=i->end();j++) {
                    // A roll is either a recording or a note.  Both come out
                    // of here as sources wrapped in a delayed, and nothing
                    // below can tell which it has -- which is the whole reason
                    // the source interface exists.
                    std::shared_ptr<clip> c;
                    unsigned long length = 0;

                    if(j->is_note()) {
                        length = (unsigned long)(j->get_seconds() * samples_per_sec);

                        if(length == 0)
                            continue;
                    }
                    else {
                        stream* s = j->get_stream();

                        if(s == 0)
                            continue;

                        std::shared_ptr<clip>& cached = decoded[s];
                        if(!cached)
                            cached = std::make_shared<clip>(*s);

                        if(cached->empty())
                            continue;

                        c = cached;
                        length = c->frames();
                    }

                    for(u_int k=0;k<get_width();k++) {
                        if(!(*j)[k])
                            continue;

                        const unsigned long begin = (unsigned long)samples_per_tick * k;

                        std::shared_ptr<source> hit;

                        if(c) {
                            std::shared_ptr<sampler> play = std::make_shared<sampler>(c);
                            play->set_rate(samples_per_sec);
                            hit = play;
                        }
                        else {
                            hit = std::make_shared<voice>(j->get_instrument(),
                                                          j->get_freq(),
                                                          length,
                                                          samples_per_sec);
                        }

                        mix.add(std::make_shared<delayed>(hit, begin));

                        if(begin + length > len)
                            len = begin + length;

                        if(getenv("JLIB_MEDIA_PLAYLIST_DEBUG")) 
                            std::cerr << "\t\tbeat: " << k << std::endl
                                      << "\t\tbegin:  " << begin  << std::endl
                                      << "\t\tcount:  " << length  << std::endl;
                    }
                }
            }

            std::vector<Type::scaled> samples(len, 0);
            mix.render(samples.data(), len, 1);

            // A hit late in the pattern runs past the end of it, and has always
            // wrapped round to the start rather than being cut off, so that a
            // pattern loops seamlessly.  Kept -- but folded after the mix
            // rather than during it, so that the staging sees each hit once.
            for(unsigned long z = (unsigned long)n; z < len; z++)
                samples[z % n] += samples[z];

            std::vector<typename Type::sample<N>::buf> samples_out(n);

            // descale multiplies by 2^(bits-1), so exactly 1.0 lands one past
            // the top of a signed buffer and wraps -- which is a full-scale
            // sample of the wrong sign, and audible.  Clamp to what it can
            // actually represent.  The fold above can put a sample over the
            // ceiling the limiter held it under, so this is not theoretical.
            const double full = std::pow(2.0, 8.0*sizeof(typename Type::sample<N>::buf) - 1);
            const double top = (full - 1) / full;

            for(int z=0;z<n;z++) {
                double v = samples[z];

                if(v >  top) v =  top;
                if(v < -1.0) v = -1.0;

                samples_out[z] = Type::sample<N>::descale((Type::scaled)v);
            }

            std::string data;
            data.assign((const char*)samples_out.data(),
                        n*sizeof(typename Type::sample<N>::buf));
            
            if(getenv("JLIB_MEDIA_PLAYLIST_DEBUG"))
                std::cerr << "void jlib::media::PlayList::render(): leave" << std::endl;
            
            return data;
        }


    }

}    
#endif //JLIB_MEDIA_PLAYLIST_HH
    
