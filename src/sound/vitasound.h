#ifndef __VITASOUND_H
#define __VITASOUND_H

// PS Vita software sound renderer (SDL audio + own mixer), used instead of
// FMOD Ex. See vitasound.cpp.
class SoundRenderer;
SoundRenderer *Vita_CreateSoundRenderer();

#endif
