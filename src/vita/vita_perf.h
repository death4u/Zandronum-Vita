// [Vita] Light-weight frame profiler. Accumulates microseconds per section
// and, every few seconds, appends a summary line to console.txt so slow
// spots on real hardware can be diagnosed from a play-test.
#ifndef VITA_PERF_H
#define VITA_PERF_H
#ifdef __vita__

#include <psp2/kernel/processmgr.h>

enum
{
	VPERF_TIC,			// game logic (G_Ticker)
	VPERF_SCENE,		// 3D view: BSP traversal + submitting geometry
	VPERF_MODELS,		// part of SCENE: drawing 3D models
	VPERF_UPDATE,		// 2D/HUD, menus and the buffer swap
	VPERF_SWAP,			// part of UPDATE: waiting for the GPU / vsync
	VPERF_UPLOAD,		// texture uploads (incl. DXT compression)
	VPERF_MODELLOAD,	// loading models on demand
	VPERF_THINKERS,		// part of TIC: all thinkers (actors, ACS scripts...)
	VPERF_GC,			// part of THINKERS: garbage collector steps
	VPERF_DB,			// SQLite (ACS database functions)
	VPERF_ACSNEW,		// starting ACS scripts (P_GetScriptGoing)
	VPERF_FINDCVAR,		// cvar lookups by name
	VPERF_COUNT
};

extern unsigned long long Vita_PerfAcc[VPERF_COUNT];
// Detailed profiling (per thinker class, per ACS script, opcode counts):
// vita_perfdetail. Off by default, since its timers cost time themselves.
extern bool Vita_PerfDetail;
extern int Vita_PerfCalls[VPERF_COUNT];

static inline unsigned long long Vita_PerfNow() { return sceKernelGetProcessTimeWide(); }

struct FVitaPerfScope
{
	int Slot;
	unsigned long long Start;
	FVitaPerfScope(int slot) : Slot(slot), Start(Vita_PerfNow()) {}
	~FVitaPerfScope() { Vita_PerfAcc[Slot] += Vita_PerfNow() - Start; Vita_PerfCalls[Slot]++; }
};

#define VITA_PERF(slot) FVitaPerfScope vita_perf_scope_##slot(slot)

// Time spent in one thinker's Tick(), grouped by class (top ones are logged).
void Vita_PerfThinker(const void *cls, const char *name, unsigned long long us);

// ACS interpreter: opcode counts and ACSF function (PCD_CALLFUNC) times.
extern unsigned Vita_PcodeCounts[1024];
extern unsigned Vita_PcodeTotal;
extern unsigned Vita_PerfOps;	// opcode count passed along with the next Vita_PerfThinker
static inline void Vita_PerfPcode(int pcd) { if (Vita_PerfDetail) { Vita_PcodeCounts[pcd & 1023]++; Vita_PcodeTotal++; } }
void Vita_PerfAcsFunc(int func, unsigned long long us);

// Call right before the buffer swap (samples vitaGL's vertex pool use).
void Vita_PerfBeforeSwap();

// Call once per displayed frame (after the swap).
void Vita_PerfFrame();

#else
#define VITA_PERF(slot)
#endif
#endif
