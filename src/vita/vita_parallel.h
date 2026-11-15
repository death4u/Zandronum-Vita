// [Vita] Game logic on a second CPU core while the 3D scene is drawn.
//
// A frame used to be: run the tics, then build the scene (BSP walk), then
// draw it, all on one core. Now, once the scene has been built (everything
// the draw step needs is copied into the draw lists by then), the tics for
// the next frame run on the logic thread (core 1) while the main thread
// sends the opaque geometry to the GPU. The two meet again before decals,
// portals, translucent things and the HUD are drawn.
//
// While the two overlap, the logic thread must not change anything the
// draw step reads; the few such things are handled here:
//   - the garbage collector doesn't run (nothing gets freed),
//   - dynamic lights aren't linked/unlinked (deferred to the join),
//   - archive reads are serialized (FileReader shares FILE handles),
//   - console output is serialized,
//   - no level changes, menus' video changes etc.: input is processed on
//     the main thread before the overlap, and the logic thread stops at a
//     tic that has a game action pending.
#ifndef VITA_PARALLEL_H
#define VITA_PARALLEL_H

#ifdef __vita__
class ADynamicLight;

extern volatile bool Vita_Overlap;	// logic thread running alongside drawing
extern bool Vita_LogicDone;			// the tics for this frame already ran

bool Vita_OnLogicThread();
void Vita_TraceTic(bool begin);
void Vita_GCCatchUp();				// main thread, after the join
bool Vita_ParallelAllowed();		// called by the renderer at the overlap point
void Vita_LogicBegin();
void Vita_LogicEnd();
void Vita_SetFrameAllowsOverlap(bool allow);

// Dynamic light (un)linking requested by the logic thread during the overlap.
bool Vita_DeferLight(ADynamicLight *light, bool unlink);
// Console commands from the logic thread during the overlap.
bool Vita_DeferCommand(const char *cmd, int keynum);

void Vita_ArchiveLock();
void Vita_ArchiveUnlock();
void Vita_PrintLock();
void Vita_PrintUnlock();

struct FVitaArchiveLock
{
	FVitaArchiveLock() { Vita_ArchiveLock(); }
	~FVitaArchiveLock() { Vita_ArchiveUnlock(); }
};
#endif
#endif
