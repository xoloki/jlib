// -*- C++ -*-

/* pattern.cc
 *
 * Copyright (C) 2002 Joey Yandle <xoloki@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include <jlib/media/PlayList.hh>
#include <jlib/media/notestream.hh>

#include <utility>
#include <iostream>


namespace jlib {
    namespace media {
        
        Roll::Roll(int id, const std::string& note, const std::string& name, const std::string& data)
            : m_pattern(data.length()),
              m_stream(0),
              m_beats(0),
              m_bpu(0),
              m_is_note(true)
        {
            set_id(id);
            set_sample(note);
            set_name(name);

            // notestream owns the note grammar, so a pattern names pitches the
            // same way jnote and jmelody do rather than inventing a second
            // spelling.  Parsed once, here, so a bad note is a construction
            // error and not a surprise at render time.
            notestream parse(note);

            m_instrument = parse.get_instrument();
            m_freq = parse.get_freq();
            m_seconds = parse.get_time();

            for(u_int i=0;i<data.length();i++) {
                if(data[i] != '0' && data[i] != '1')
                    std::cerr << "invalid data["<<i<<"]: " << data[i] << std::endl;
                m_pattern[i] = (data[i] == '1');
            }
        }

        Roll::Roll(int id, stream* s, const std::string& name, const std::string& sample, const std::string& data) 
            : m_pattern(data.length()),
              m_beats(0),
              m_bpu(0),
              m_is_note(false),
              m_freq(0),
              m_seconds(0)
        {
            set_id(id);
            set_sample(sample);
            set_name(name);
            set_stream(s);
            
            for(u_int i=0;i<data.length();i++) {
                if(data[i] != '0' && data[i] != '1')
                    std::cerr << "invalid data["<<i<<"]: " << data[i] << std::endl;
                m_pattern[i] = (data[i] == '1');
            }
        }
        
        Roll::Roll() 
            : m_stream(0),
              m_id(0),
              m_beats(0),
              m_bpu(0),
              m_is_note(false),
              m_freq(0),
              m_seconds(0)
        {
            // m_stream was left uninitialized here, so get_stream() on a
            // default-constructed Roll returned whatever was on the stack --
            // which a null check does not catch, and which render() would then
            // read audio from.  The other constructor sets all of these.
        }
        
        int Roll::get_id() const { return m_id; }
        void Roll::set_id(int id) { m_id = id; }
        
        std::string Roll::get_sample() const { return m_sample; }
        void Roll::set_sample(const std::string& sample) { m_sample = std::move(sample); }
        
        std::string Roll::get_name() const { return m_name; }
        void Roll::set_name(const std::string& name) { m_name = std::move(name); }
        
        Roll::reference Roll::operator[](int i) {
            return m_pattern[i];
        }
        
        Roll::const_reference Roll::operator[](int i) const {
            return m_pattern[i];
        }
        
        stream* Roll::get_stream() {
            return m_stream;
        }
        
        const stream* Roll::get_stream() const {
            return m_stream;
        }
        
        void Roll::set_stream(stream* s) {
            m_stream = s;
        }
        
        bool Roll::is_note() const { return m_is_note; }
        
        const instrument& Roll::get_instrument() const { return m_instrument; }
        void Roll::set_instrument(const instrument& i) { m_instrument = i; }
        
        double Roll::get_freq() const { return m_freq; }
        double Roll::get_seconds() const { return m_seconds; }
        
        
        Pattern::Pattern(int id, const std::string& name) {
            m_id = id;
            m_name = name;
            m_roll_id_max = 0;
        }
        
        Pattern::~Pattern() {
            
        }
        
        int Pattern::get_id() const { return m_id; }
        void Pattern::set_id(int id) { m_id = id; }
        
        std::string Pattern::get_name() const { return m_name; }
        void Pattern::set_name(const std::string& name) { m_name = std::move(name); }
        
        Pattern::reference Pattern::operator[](int i) {
            return m_rolls[i];
        }
        
        Pattern::const_reference Pattern::operator[](int i) const {
            return m_rolls[i];
        }
        
        void Pattern::push_back(const_reference r) { 
            m_rolls.push_back(r); 
            if(r.get_id() > m_roll_id_max)
                m_roll_id_max = r.get_id();
        }
        
        int Pattern::get_next_roll_id() {
            return ++m_roll_id_max;
        }

        PlayList::PlayList() { m_pattern_id_max = 0; }

        PlayList::PlayList(int id, const std::string& name) { 
            set_id(id);
            set_name(name);
            m_pattern_id_max = 0; 
        }

        PlayList::~PlayList() {}
        
        int PlayList::get_id() const { return m_id; }
        void PlayList::set_id(int id) { m_id = id; }
        
        std::string PlayList::get_name() const { return m_name; }
        void PlayList::set_name(const std::string& name) { m_name = std::move(name); }

        int PlayList::get_bpm() const { return m_bpm; }
        void PlayList::set_bpm(int bpm) { m_bpm = bpm; }
        
        int PlayList::get_width() const { return m_width; }
        void PlayList::set_width(int width) { m_width = width; }
        
        int PlayList::get_measure() const { return m_measure; }
        void PlayList::set_measure(int measure) { m_measure = measure; }
        
        int PlayList::get_next_pattern_id() {
            // Incremented but never returned.  m_pattern_id_max starts at 0,
            // so the first id handed out is 1.  Caught by -Werror=return-type;
            // it has no callers, which is why it went unnoticed.
            return ++m_pattern_id_max;
        }
        
        PlayList::reference PlayList::operator[](int i) {
            return m_slices[i];
        }

        PlayList::const_reference PlayList::operator[](int i) const {
            return m_slices[i];
        }
        
        void PlayList::push_back(const_reference r) {
            m_slices.push_back(r);
        }
        
        std::string PlayList::render(int fmt, slice_type slice) {
            switch(fmt) {
            case Type::PCM_U8:
                return render<Type::PCM_U8>(slice);
            case Type::PCM_S8:
                return render<Type::PCM_S8>(slice);
            case Type::PCM_S16_LE:
                return render<Type::PCM_S16_LE>(slice);
            case Type::PCM_S16_BE:
                return render<Type::PCM_S16_BE>(slice);
            case Type::PCM_U16_LE:
                return render<Type::PCM_U16_LE>(slice);
            case Type::PCM_U16_BE:
                return render<Type::PCM_U16_BE>(slice);
            case Type::PCM_MPEG:
                return render<Type::PCM_MPEG>(slice);
            case Type::PCM_AC3:
                return render<Type::PCM_AC3>(slice);
            case Type::PCM_FLOAT32:
                return render<Type::PCM_FLOAT32>(slice);
            default:
                return std::string();
            }
        }
        
        
    }
}
