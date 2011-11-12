/* -*- mode: C++ c-basic-offset: 4 -*-
 * AudioFile.h
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
 */

#include <jlib/media/Dsp.hh>

#include <jlib/util/util.hh>

#include <jlib/sys/sys.hh>

#include <cmath>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

const int JLIB_MEDIA_DSP_FRAGSIZE = 2048;

namespace jlib {
    namespace media {
        
        Dsp::Dsp(std::string node,bool open)
            : m_bits_per_sample(-1),
              m_samples_per_sec(-1),
              m_channels(-1),
              m_format(-1),
              m_fragsize(-1),
              m_configured(false),
              m_dsp(-1),
              m_node(node)
        {
            if(open)
                this->open(node);
        }
        
        
        Dsp::~Dsp() {
            close();
        }
        
        void Dsp::open(std::string node) {
            if(node != "")
                m_node = node;
            
            if((m_dsp=::open(m_node.c_str(),O_WRONLY))==-1) {
                exception::throw_errno("Error opening device "+m_node);
            }
            
            if(ioctl(m_dsp, SNDCTL_DSP_SYNC, 0)==-1) {
                close();
                exception::throw_errno("Error running ioctl SNDCTL_DSP_SYNC");
            }

            set_fragsize(JLIB_MEDIA_DSP_FRAGSIZE);
        }
        
        void Dsp::config(stream& s) {
            config(s.get_bits_per_sample(),s.get_samples_per_sec(),
                   s.get_channels(),s.get_format());
        }
        
        void Dsp::config(int bits_per_sample, int samples_per_sec, int channels, int format) {
            m_configured = false;

            if(getenv("JLIB_MEDIA_DSP_DEBUG")) {
                std::cerr << "enter jlib::media::Dsp::config_dsp()" << std::endl;
            }
            if(getenv("JLIB_MEDIA_DSP_DEBUG")) 
                std::cout << "\tSNDCTL_DSP_BITS = " << bits_per_sample << std::endl
                          << "\tSNDCTL_DSP_SPEED = " << samples_per_sec << std::endl
                          << "\tSNDCTL_DSP_CHANNELS = " << channels << std::endl
                          << "\tSNDCTL_DSP_SETFMT = " << format << std::endl;
            int s;
            if(m_format != format) {
                m_format = format;
                s = format;
                
                if(ioctl(m_dsp, SNDCTL_DSP_SETFMT, &s) == -1) {
                    close();
                    exception::throw_errno("Error running ioctl SNDCTL_DSP_SETFMT = "+
                                           jlib::util::string_value(format));
                }
                else if(s != format) {
                    exception::throw_errno("device doesn't support format "
                                           +jlib::util::string_value(format)+
                                           ", but it likes " + jlib::util::string_value(s));
                }
            }
            
            if(m_channels != channels) {
                m_channels = channels;
                s = channels;
                if(ioctl(m_dsp, SNDCTL_DSP_CHANNELS, &s) == -1) {
                    close();
                    exception::throw_errno("Error running ioctl SNDCTL_DSP_CHANNELS = "+
                                           jlib::util::string_value(channels));
                }
                else if(s != channels) {
                    exception::throw_errno("device doesn't support channels "+
                                           jlib::util::string_value(channels)+
                                           ", but it likes " + jlib::util::string_value(s));
                }
            }
            
            if(m_samples_per_sec != samples_per_sec) {
                m_samples_per_sec = samples_per_sec;
                s = samples_per_sec;
                if(ioctl(m_dsp, SNDCTL_DSP_SPEED, &s) == -1) {
                    close();
                    exception::throw_errno("Error running ioctl SNDCTL_DSP_SPEED = "
                                           +jlib::util::string_value(samples_per_sec));
                }
                else if(s != samples_per_sec) {
                    exception::throw_errno("device doesn't support speed "+
                                           jlib::util::string_value(samples_per_sec)+
                                           ", but it likes " + jlib::util::string_value(s));
                }
            }

            m_configured = true;
            
            if(getenv("JLIB_MEDIA_DSP_DEBUG")) {
                std::cerr << "leave jlib::media::Dsp::config_dsp()" << std::endl;
            }
        }
        
        void Dsp::play(stream& s) {
            if(getenv("JLIB_MEDIA_DSP_DEBUG")) {
                std::cerr << "enter jlib::media::Dsp::play()" << std::endl;
            }

            config(s);
            
            while(s) {
                play_frag(s);
            }

            if(getenv("JLIB_MEDIA_DSP_DEBUG")) {
                std::cerr << "leave jlib::media::Dsp::play()" << std::endl;
            }
        }
        
        void Dsp::play_frag(stream& s, int n) {
            jlib::sys::getstring(s,m_buf,n*get_fragsize());
            write(m_buf);
        }

        void Dsp::reset() {
            if(ioctl(m_dsp, SNDCTL_DSP_RESET, 0)==-1) {
                exception::throw_errno("error: ioctl(SNDCTL_DSP_RESET)");
            }
            
        }

        bool Dsp::is_configured() const { return m_configured; }
        
        int Dsp::get_fragsize() const {
            return m_fragsize;
        }

        void Dsp::set_fragsize(int f) {
            m_fragsize = f;
            int lg2 = (int)(log((double)m_fragsize)/log(2));
            int arg = (lg2 & 0x0000ffff) | 0x7fff0000;
            if(ioctl(m_dsp, SNDCTL_DSP_SETFRAGMENT, &arg)) 
                exception::throw_errno("error in ioctl(SNDCTL_DSP_SETFRAGMENT)");
        }
        
        int Dsp::get_fragments() const {
            audio_buf_info info;
            if(ioctl(m_dsp, SNDCTL_DSP_GETOSPACE, &info)==-1)
                exception::throw_errno("error in ioctl(SNDCTL_DSP_GETOSPACE)");

            return info.fragments;
        }

        int Dsp::get_frags_total() const {
            audio_buf_info info;
            if(ioctl(m_dsp, SNDCTL_DSP_GETOSPACE, &info)==-1)
                exception::throw_errno("error in ioctl(SNDCTL_DSP_GETOSPACE)");
            
            return info.fragstotal;
        }
        
        int Dsp::get_frags_used() const {
            audio_buf_info info;
            if(ioctl(m_dsp, SNDCTL_DSP_GETOSPACE, &info)==-1)
                exception::throw_errno("error in ioctl(SNDCTL_DSP_GETOSPACE)");
            
            return info.fragstotal-info.fragments;
        }
        
        void Dsp::close() {
            if(m_dsp != -1) ::close(m_dsp);
        }
        
        void Dsp::write(std::string data) {
            if(m_dsp != -1 && data.length() > 0) {
                int e = ::write(m_dsp,data.data(),data.length());
                if(e == -1)
                    exception::throw_errno("error wrote 0/"+
                                           jlib::util::string_value(static_cast<unsigned int>(data.length()))+
                                           " bytes to dsp");
                else if(e != data.length())
                    exception::throw_errno("error: wrote "+
                                           jlib::util::string_value(e)+
                                           "/" +
                                           jlib::util::string_value(static_cast<unsigned int>(data.length()))+
                                           " bytes to dsp");
           }
        }
        
        /*
          int m_bits_per_sample, m_samples_per_sec, m_channels, m_format;
          bool m_is_configured;
          int m_dsp;
          std::string m_node;
        */
        
        
    }
}

