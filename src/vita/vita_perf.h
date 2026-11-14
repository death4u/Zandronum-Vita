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
	VPERF_COUNT
};

extern unsigned long long Vita_PerfAcc[VPERF_COUNT];
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

// Call once per displayed frame (after the swap).
void Vita_PerfFrame();

#else
#define VITA_PERF(slot)
#endif
#endif
