/*
  To convert a jhyper into music, model each vertex as an emmitter of sound at a certain frequency (depending on the color assigned to the vertex).  Do so in N dimensions.  On render, project down to 3 dimensions.  For each frame, consider not only the new position of the vertex, but also its relative position in regards to the past position.  That will give us a velocity towards the viewer, which will be used for doppler effect corrections to the 3d sound.  
I never implemented a full analog digital instrument.  I will do that in order to make this music full featured, not just crappy sounds sequenced.

Attack, decay, sustain, release.  Each vertex gets a full ASDR instrument.  Pitch is random and is reflective of color; purple is high pitch and red is low pitch.  Volume and pitch will be determined positionally with Doppler corrections (I'll skip the relatavistic corrections for now, though since it's just special relativity I should do it eventually).

The music video consists of animation code which creates objects in the N dimensional space and moves them in time and space.  The observer moves too, both in position and orientation.  It is a dance of an observer moving among moving objects.

It should be sufficient for the video portion to grab the window manager frame buffer, then write to a video stream in some container/codec.  Audio will run a parallel rendering pipeline, running the animation and rendering the audio at each frame.  The audio and video pipelines should fill output queues, each of which is read sequentially by a controller which muxes them and writes the next 

Twiddle the knobs.  See what happens.
 
It will eventually be possible to build a live music system, where a dj can twiddle the knobs of time; and allow skewing certain parameters of the animation, especially relating to color and intensity of edges.  all of these twiddles can be mapped to user input devices, like a playstation controller, or dj mixer type thing, or a spacemouse.

 */
