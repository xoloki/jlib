/*
  To convert a jhyper into music, model each vertex as an emmitter of sound at a certain frequency (depending on the color assigned to the vertex).  Do so in N dimensions.  On render, project down to 3 dimensions.  For each frame, consider not only the new position of the vertex, but also its relative position in regards to the past position.  That will give us a velocity towards the viewer, which will be used for doppler effect corrections to the 3d sound.  
I never implemented a full analog digital instrument.  I will do that in order to make this music full featured, not just crappy sounds sequenced.

Attack, decay, sustain, release.  Each vertex gets a full ASDR instrument.  Pitch is random and is reflective of color; purple is high pitch and red is low pitch.  Volume and pitch will be determined positionally.

Twiddle the knobs.  See what happens.
 
 */
