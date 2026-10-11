#ifndef _BS_SF2000_AUDIO_H_
#define _BS_SF2000_AUDIO_H_

#include "audio_system.h"
#include "data_win.h"

typedef struct Sf2000AudioSystem Sf2000AudioSystem;

Sf2000AudioSystem* Sf2000AudioSystem_create(DataWin* dataWin);
void Sf2000AudioSystem_renderFrame(void);

#endif /* _BS_SF2000_AUDIO_H_ */
