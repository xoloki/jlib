// -*- C++ -*-

/* pattern.cc
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

#include <jlib/media/PlayList.hh>

#include <iostream>

#include <sys/soundcard.h>

namespace jlib {
    namespace media {
        
        Roll::Roll(int id, stream* s, std::string name, std::string sample, std::string data) 
            : m_pattern(data.length())
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
        {
            set_id(0);
        }
        
        int Roll::get_id() const { return m_id; }
        void Roll::set_id(int id) { m_id = id; }
        
        std::string Roll::get_sample() const { return m_sample; }
        void Roll::set_sample(std::string sample) { m_sample = sample; }
        
        std::string Roll::get_name() const { return m_name; }
        void Roll::set_name(std::string name) { m_name = name; }
        
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
        
        
        Pattern::Pattern(int id, std::string name) {
            m_id = id;
            m_name = name;
            m_roll_id_max = 0;
        }
        
        Pattern::~Pattern() {
            
        }
        
        int Pattern::get_id() const { return m_id; }
        void Pattern::set_id(int id) { m_id = id; }
        
        std::string Pattern::get_name() const { return m_name; }
        void Pattern::set_name(std::string name) { m_name = name; }
        
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

        PlayList::PlayList(int id, std::string name) { 
            set_id(id);
            set_name(name);
            m_pattern_id_max = 0; 
        }

        PlayList::~PlayList() {}
        
        int PlayList::get_id() const { return m_id; }
        void PlayList::set_id(int id) { m_id = id; }
        
        std::string PlayList::get_name() const { return m_name; }
        void PlayList::set_name(std::string name) { m_name = name; }

        int PlayList::get_bpm() const { return m_bpm; }
        void PlayList::set_bpm(int bpm) { m_bpm = bpm; }
        
        int PlayList::get_width() const { return m_width; }
        void PlayList::set_width(int width) { m_width = width; }
        
        int PlayList::get_measure() const { return m_measure; }
        void PlayList::set_measure(int measure) { m_measure = measure; }
        
        int PlayList::get_next_pattern_id() {
            ++m_pattern_id_max;
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
