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
	VPERF_BSP,			// part of SCENE: BSP walk, setting up walls/flats/sprites
	VPERF_JOIN,			// part of SCENE: waiting for the logic thread (vita_parallel)
	VPERF_OPAQUE,		// part of SCENE: opaque passes (drawn alongside the logic)
	VPERF_DECALS,		// part of SCENE: decal pass
	VPERF_PORTALS,		// part of SCENE: portals and sky
	VPERF_TRANSL,		// part of SCENE: translucent pass
	VPERF_PLAYERS,		// part of TIC: P_PlayerThink for all players (bots included)
	VPERF_BOTTICK,		// part of PLAYERS: bot AI (CSkullBot::Tick, bot scripts)
	VPERF_PSPRITES,		// part of PLAYERS: weapon states (P_MovePsprites)
	VPERF_PMOVE,		// part of PLAYERS: P_MovePlayer + P_CalcHeight
	VPERF_PUSE,			// part of PLAYERS: P_UseLines / talk
	VPERF_SBAR,			// part of TIC: status bar tick
	VPERF_BOTAIM,		// part of TIC: bot enemy sight + aiming
	VPERF_EFFECTS,		// part of TIC: particles, specials, effects
	VPERF_SKYBOX,		// part of opaque: the first sky portal (sky box scenes)
	VPERF_TEXDECODE,	// texture image decoding (PNG/JPG, translations)
	VPERF_MEXPAND,		// part of MODELS: unpacking/blending vertices
	VPERF_MDRAW,		// part of MODELS: draw calls
	VPERF_MBIND,		// part of MODELS: skin binds
	VPERF_BATCHDRAW,	// drawing the collected wall/flat batches (vitaGL draw calls)
	VPERF_DWALLS,		// detail only: walls (all passes, collecting)
	VPERF_DFLATS,		// detail only: flats (all passes, collecting)
	VPERF_DSPRITES,		// detail only: sprites drawn from draw lists
	VPERF_DLIGHTS,		// detail only: dynamic light passes (walls + flats)
	VPERF_WARP,
	VPERF_BSPWALLS,	// bsp: wall processing (detail)
	VPERF_BSPTHINGS,	// bsp: sprites/things (detail)
	VPERF_BSPFLATS,	// bsp: sector/flat processing (detail)
	VPERF_WSETUP,	// wall draw: colour/fog/batch setup (detail)
	VPERF_WEMIT,	// wall draw: vertex emission incl. light prep (detail)
	VPERF_WLSEL,	// wall draw: light selection (detail)
	VPERF_SPRLIGHT,	// sprite drawing: dynamic light lookup (detail)
	VPERF_SPRBIND,	// sprite drawing: texture bind incl. first loads (detail)
	VPERF_SPRMODEL,	// sprite drawing: models (detail)
	VPERF_TSORT,	// translucent list sort/split against walls (detail)
	VPERF_PICKUP,	// first ticks of actors (pickup stagger, always)		// warped (wavy) texture updates
	VPERF_DUMMY,		// unused (scopes that time nothing)
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
extern bool Vita_PerfDetail;
struct FVitaPerfScopeD
{
	int Slot;
	unsigned long long Start;
	FVitaPerfScopeD(int slot) : Slot(slot), Start(Vita_PerfDetail ? Vita_PerfNow() : 0) {}
	~FVitaPerfScopeD() { if (Start) { Vita_PerfAcc[Slot] += Vita_PerfNow() - Start; Vita_PerfCalls[Slot]++; } }
};
// timed only in the detail windows (thousands of calls a frame)
#define VITA_PERF_D(slot) FVitaPerfScopeD vita_perf_scoped_##slot(slot)

// Time spent in one thinker's Tick(), grouped by class (top ones are logged).
void Vita_PerfThinker(const void *cls, const char *name, unsigned long long us);

// ACS interpreter: opcode counts and ACSF function (PCD_CALLFUNC) times.
extern unsigned Vita_PcodeCounts[1024];
extern unsigned Vita_PcodeTotal;
extern unsigned long long Vita_PcodeUs[1024];
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
