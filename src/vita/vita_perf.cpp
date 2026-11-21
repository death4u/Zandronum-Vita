// [Vita] See vita_perf.h.
unsigned Vita_BigBlocksKB();
int Vita_GLPhase;	// 0 2D/other, 1 opaque 3D, 2 translucent, 3 end of scene (blend, weapon), 4 models
unsigned Vita_GLPhaseDraws[5];
#include <stdio.h>
#include <malloc.h>
#include "vita_perf.h"
#include "vita_system.h"
#include "c_cvars.h"
#ifndef NO_GL
#include <vitaGL.h>
#include "dobject.h"
extern unsigned long long Vita_GCTime, Vita_LogicStartDelay, Vita_LogicRunTime;
#endif

CVAR(Bool, vita_perflog, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Bool, vita_perfdetail, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	Vita_PerfDetail = self;
}

bool Vita_PerfDetail;

// With vita_perfbursts, every 5th 3-second window is profiled in detail.
// 0046: off by default (renamed from vita_perfburst so saved configs don't
// keep it on): the detailed windows and their report froze the game for
// 230-460 ms every 15 s on the Vita.
CVAR(Bool, vita_perfbursts, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

unsigned long long Vita_PerfAcc[VPERF_COUNT];

#ifndef NO_GL
// vitaGL's immediate-mode vertex pool for the current frame.
extern "C" float *legacy_pool, *legacy_pool_ptr, *vgl_zand_legacy_base;
extern "C" int legacy_pool_size;
static unsigned LegacyPoolPeak;
#endif
int Vita_PerfCalls[VPERF_COUNT];

struct FPerfClass { const void *Cls; const char *Name; unsigned long long Us; int Calls; unsigned Ops; };
unsigned Vita_PcodeTotal;
static FPerfClass PerfClasses[512];
static int NumPerfClasses;
unsigned Vita_PerfOps;

// (hashed: a linear search over up to 512 classes per thinker tick made
// the detailed windows themselves slow)
static short PerfClassHash[1024];
void Vita_PerfThinker(const void *cls, const char *name, unsigned long long us)
{
	unsigned h = ((uintptr_t)cls >> 3) & 1023;
	for (int k = 0; k < 1024; k++, h = (h + 1) & 1023)
	{
		const int i = PerfClassHash[h] - 1;
		if (i < 0 || i >= NumPerfClasses) break;
		if (PerfClasses[i].Cls == cls) { PerfClasses[i].Us += us; PerfClasses[i].Calls++; PerfClasses[i].Ops += Vita_PerfOps; return; }
	}
	if (NumPerfClasses < 512)
	{
		PerfClasses[NumPerfClasses].Cls = cls;
		PerfClasses[NumPerfClasses].Name = name;
		PerfClasses[NumPerfClasses].Us = us;
		PerfClasses[NumPerfClasses].Calls = 1;
		PerfClasses[NumPerfClasses].Ops = Vita_PerfOps;
		NumPerfClasses++;
		PerfClassHash[h] = (short)NumPerfClasses;
	}
}

unsigned Vita_PcodeCounts[1024];
unsigned long long Vita_PcodeUs[1024];
static unsigned long long AcsFuncUs[1024];
static unsigned AcsFuncCalls[1024];

void Vita_PerfAcsFunc(int func, unsigned long long us)
{
	AcsFuncUs[func & 1023] += us;
	AcsFuncCalls[func & 1023]++;
}

void Vita_PerfBeforeSwap()
{
#ifndef NO_GL

	if (legacy_pool != NULL && vgl_zand_legacy_base != NULL && legacy_pool >= vgl_zand_legacy_base)
	{
		// legacy_pool is vitaGL's write pointer (patched vitaGL records the frame's start)
		unsigned used = (unsigned)((char *)legacy_pool - (char *)vgl_zand_legacy_base);
		if (used > LegacyPoolPeak) LegacyPoolPeak = used;
	}
#endif
}


// [Vita] The report is written by its own thread from a copy of the
// counters: formatting it (and mallinfo(), which walks the whole heap) on the
// main thread stalled one frame every 3 s (50-60 ms, Brutal Doom) and after
// every detailed window (150-370 ms, HaloDoom).
struct FPerfSnap
{
	bool Detail;
	int Frames;
	unsigned long long Period, Worst;
	unsigned long long Acc[VPERF_COUNT];
	int Calls[VPERF_COUNT];
	FPerfClass Classes[512];
	int NumClasses;
	unsigned PcodeCounts[1024];
	unsigned long long PcodeUs[1024];
	unsigned long long AcsUs[1024];
	unsigned AcsCalls[1024];
	unsigned GLCount[8];
	unsigned GLPhase[5];
	int RLines, RFlats, RSprites;
	unsigned long long GCTime, LogicStartDelay, LogicRunTime;
	unsigned HeapMB, BigKB, ObjMB, VglRam, VglVram, PoolPeak, PoolSize;
	char Callers[256];
	unsigned WallHits, WallMisses, SurfHits, SurfMisses;
	unsigned long long IoMain, IoLogic;
};
static FPerfSnap PerfSnap;
static unsigned long long Vita_PerfGpuQueryUs;

static void Vita_PerfFormat(FPerfSnap &S)
{
	{
		const double f = S.Frames > 0 ? 1.0 / S.Frames : 0;
		
		char line[2048];
		snprintf(line, sizeof(line),
			"[perf] %.1f fps, worst frame %.0f ms | per frame (ms): logic %.1f (players %.1f [bot AI %.1f, weapons %.1f, move %.1f, use %.1f], thinkers %.1f, hud %.1f, bot aim %.1f, particles %.1f), 3D %.1f (bsp %.1f, opaque %.1f [sky %.1f, batch draws %.1f], waiting for logic %.1f, decals %.1f, portals+sky %.1f, translucent %.1f, models %.1f [expand %.1f draw %.1f bind %.1f]), 2D+swap %.1f (swap %.1f)"
			" | in %.0f s: %d texture uploads %.0f ms (decode %.0f ms), %d warp updates %.0f ms, %d model loads %.0f ms | logic thread: start delay %.1f, run %.1f | heap reserved %u MB + %u MB outside (objects %u MB, gc %.1f ms/frame)"
#ifndef NO_GL
			", vitaGL free RAM %u MB CDRAM %u MB, immediate-mode pool peak %u of %u KB | kept between frames: walls %u%%, wall vertices %u%% | memory card waits per frame: main %.1f ms, logic %.1f ms"
#endif
			"\n",
			S.Frames * 1e6 / S.Period, S.Worst / 1000.0,
			S.Acc[VPERF_TIC] * f / 1000.0, S.Acc[VPERF_PLAYERS] * f / 1000.0, S.Acc[VPERF_BOTTICK] * f / 1000.0, S.Acc[VPERF_PSPRITES] * f / 1000.0, S.Acc[VPERF_PMOVE] * f / 1000.0, S.Acc[VPERF_PUSE] * f / 1000.0, S.Acc[VPERF_THINKERS] * f / 1000.0,
			S.Acc[VPERF_SBAR] * f / 1000.0, S.Acc[VPERF_BOTAIM] * f / 1000.0, S.Acc[VPERF_EFFECTS] * f / 1000.0,
			S.Acc[VPERF_SCENE] * f / 1000.0, S.Acc[VPERF_BSP] * f / 1000.0,
			S.Acc[VPERF_OPAQUE] * f / 1000.0, S.Acc[VPERF_SKYBOX] * f / 1000.0, S.Acc[VPERF_BATCHDRAW] * f / 1000.0, S.Acc[VPERF_JOIN] * f / 1000.0,
			S.Acc[VPERF_DECALS] * f / 1000.0, S.Acc[VPERF_PORTALS] * f / 1000.0, S.Acc[VPERF_TRANSL] * f / 1000.0,
			S.Acc[VPERF_MODELS] * f / 1000.0, S.Acc[VPERF_MEXPAND] * f / 1000.0, S.Acc[VPERF_MDRAW] * f / 1000.0, S.Acc[VPERF_MBIND] * f / 1000.0,
			S.Acc[VPERF_UPDATE] * f / 1000.0,
			S.Acc[VPERF_SWAP] * f / 1000.0,
			S.Period / 1e6, S.Calls[VPERF_UPLOAD], S.Acc[VPERF_UPLOAD] / 1000.0, S.Acc[VPERF_TEXDECODE] / 1000.0, S.Calls[VPERF_WARP], S.Acc[VPERF_WARP] / 1000.0,
			S.Calls[VPERF_MODELLOAD], S.Acc[VPERF_MODELLOAD] / 1000.0,
			S.LogicStartDelay * f / 1000.0, S.LogicRunTime * f / 1000.0,
			S.HeapMB, S.BigKB >> 10, S.ObjMB, S.GCTime * f / 1000.0
#ifndef NO_GL
			, S.VglRam, S.VglVram, (S.PoolPeak >> 10), S.PoolSize, S.WallHits + S.WallMisses ? S.WallHits * 100 / (S.WallHits + S.WallMisses) : 0, S.SurfHits + S.SurfMisses ? S.SurfHits * 100 / (S.SurfHits + S.SurfMisses) : 0, S.IoMain * f / 1000.0, S.IoLogic * f / 1000.0
#endif
			);
		Vita_ConsoleWrite(line);
	}
	if (S.Detail)
	{
		const double f = S.Frames > 0 ? 1.0 / S.Frames : 0;
		char line[2048];

		// Where the logic time goes: thinkers in total, GC, database, and the
		// most expensive thinker classes (ms per frame, ticks per frame).
		size_t len = snprintf(line, sizeof(line), "[perf]   thinkers %.1f (gc %.1f), database %.1f (%d calls), ACS starts %.1f (x%d), cvar lookups %.1f (x%d) | top:",
			S.Acc[VPERF_THINKERS] * f / 1000.0, S.Acc[VPERF_GC] * f / 1000.0,
			S.Acc[VPERF_DB] * f / 1000.0, S.Calls[VPERF_DB],
			S.Acc[VPERF_ACSNEW] * f / 1000.0, (int)(S.Calls[VPERF_ACSNEW] * f + 0.5),
			S.Acc[VPERF_FINDCVAR] * f / 1000.0, (int)(S.Calls[VPERF_FINDCVAR] * f + 0.5));
		for (int n = 0; n < 10; n++)
		{
			int best = -1;
			for (int i = 0; i < S.NumClasses; i++)
				if (S.Classes[i].Us > 0 && (best < 0 || S.Classes[i].Us > S.Classes[best].Us)) best = i;
			if (best < 0 || len >= sizeof(line) - 64) break;
			if (S.Classes[best].Ops > 0)
				len += snprintf(line + len, sizeof(line) - len, " %s %.1f (x%d, %.0f op)", S.Classes[best].Name,
					S.Classes[best].Us * f / 1000.0, (int)(S.Classes[best].Calls * f + 0.5), S.Classes[best].Ops * f);
			else
				len += snprintf(line + len, sizeof(line) - len, " %s %.1f (x%d)", S.Classes[best].Name,
					S.Classes[best].Us * f / 1000.0, (int)(S.Classes[best].Calls * f + 0.5));
			S.Classes[best].Ops = 0;
			S.Classes[best].Us = 0;
		}
		snprintf(line + len, sizeof(line) - len, "\n");
		Vita_ConsoleWrite(line);

#ifndef NO_GL
		{
			snprintf(line, sizeof(line), "[perf]   render per frame: walls %.0f, flats %.0f, sprites %.0f | GL draw calls %.0f (vertices %.0f), glBegin %.0f, texture binds %.0f, colour changes %.0f, fog changes %.0f | draws+glBegin: opaque %.0f, translucent %.0f, models %.0f, weapon/blend %.0f, 2D %.0f\n",
				S.RLines * f, S.RFlats * f, S.RSprites * f,
				S.GLCount[0] * f, S.GLCount[5] * f, S.GLCount[1] * f, S.GLCount[2] * f, S.GLCount[3] * f, S.GLCount[4] * f,
				S.GLPhase[1] * f, S.GLPhase[2] * f, S.GLPhase[4] * f, S.GLPhase[3] * f, S.GLPhase[0] * f);
			Vita_ConsoleWrite(line);
			snprintf(line, sizeof(line), "[perf]   draw lists per frame (ms): walls %.1f (x%.0f), flats %.1f (x%.0f), sprites %.1f (x%.0f), of these light passes %.1f (x%.0f)\n",
				S.Acc[VPERF_DWALLS] * f / 1000.0, S.Calls[VPERF_DWALLS] * f,
				S.Acc[VPERF_DFLATS] * f / 1000.0, S.Calls[VPERF_DFLATS] * f,
				S.Acc[VPERF_DSPRITES] * f / 1000.0, S.Calls[VPERF_DSPRITES] * f,
				S.Acc[VPERF_DLIGHTS] * f / 1000.0, S.Calls[VPERF_DLIGHTS] * f);
			Vita_ConsoleWrite(line);
						snprintf(line, sizeof(line), "[perf]   bsp per frame (ms): walls %.1f (x%.0f), things %.1f, sectors/flats %.1f | wall draw: setup %.1f, emit %.1f (x%.0f), light selection %.1f\n",
				S.Acc[VPERF_BSPWALLS] * f / 1000.0, S.Calls[VPERF_BSPWALLS] * f, S.Acc[VPERF_BSPTHINGS] * f / 1000.0, S.Acc[VPERF_BSPFLATS] * f / 1000.0,
				S.Acc[VPERF_WSETUP] * f / 1000.0, S.Acc[VPERF_WEMIT] * f / 1000.0, S.Calls[VPERF_WEMIT] * f, S.Acc[VPERF_WLSEL] * f / 1000.0);
			Vita_ConsoleWrite(line);
			snprintf(line, sizeof(line), "[perf]   sprites per frame (ms): dynamic lights %.1f, texture binds/first loads %.1f, models %.1f | translucent sort/split %.1f\n",
				S.Acc[VPERF_SPRLIGHT] * f / 1000.0, S.Acc[VPERF_SPRBIND] * f / 1000.0, S.Acc[VPERF_SPRMODEL] * f / 1000.0, S.Acc[VPERF_TSORT] * f / 1000.0);
			Vita_ConsoleWrite(line);
			const char *cb = S.Callers;
			snprintf(line, sizeof(line), "[perf]   opaque glDrawArrays callers (3 s):%s | RenderScene %.1f/frame, DrawSubsectors %.0f/frame\n", cb, S.GLCount[6] * f, S.GLCount[7] * f);
			Vita_ConsoleWrite(line);
		}
#endif
		// ACS: most executed opcodes and the most expensive ACSF functions.
		unsigned total = 0;
		for (int i = 0; i < 1024; i++) total += S.PcodeCounts[i];
		len = snprintf(line, sizeof(line), "[perf]   ACS opcodes/frame %.0f | top:", total * f);
		for (int n = 0; n < 12; n++)
		{
			int best = -1;
			for (int i = 0; i < 1024; i++)
				if (S.PcodeCounts[i] > 0 && (best < 0 || S.PcodeCounts[i] > S.PcodeCounts[best])) best = i;
			if (best < 0 || len >= sizeof(line) - 32) break;
			len += snprintf(line + len, sizeof(line) - len, " %d:%.0f", best, S.PcodeCounts[best] * f);
			S.PcodeCounts[best] = 0;
		}
		len += snprintf(line + len, sizeof(line) - len, " | op ms:");
		for (int n = 0; n < 8; n++)
		{
			int best = -1;
			for (int i = 0; i < 1024; i++)
				if (S.PcodeUs[i] > 0 && (best < 0 || S.PcodeUs[i] > S.PcodeUs[best])) best = i;
			if (best < 0 || len >= sizeof(line) - 40) break;
			len += snprintf(line + len, sizeof(line) - len, " %d:%.2f", best, S.PcodeUs[best] * f / 1000.0);
			S.PcodeUs[best] = 0;
		}
		len += snprintf(line + len, sizeof(line) - len, " | ACSF ms:");
		for (int n = 0; n < 8; n++)
		{
			int best = -1;
			for (int i = 0; i < 1024; i++)
				if (S.AcsUs[i] > 0 && (best < 0 || S.AcsUs[i] > S.AcsUs[best])) best = i;
			if (best < 0 || len >= sizeof(line) - 40) break;
			len += snprintf(line + len, sizeof(line) - len, " f%d %.2f (x%.0f)", best, S.AcsUs[best] * f / 1000.0, S.AcsCalls[best] * f);
			S.AcsUs[best] = 0;
		}
		snprintf(line + len, sizeof(line) - len, "\n");
		Vita_ConsoleWrite(line);
	}
}

// Heap size without mallinfo() (which walks the heap): newlib's running
// count of the memory the heap has taken from the system so far.
extern "C" struct mallinfo __malloc_current_mallinfo;
static unsigned Vita_HeapReservedMB()
{
	return (unsigned)(__malloc_current_mallinfo.arena >> 20);
}

// 0047: formatted by the log thread (Vita_PerfLogThreadHook); the main
// thread only copies the counters. If the log thread hasn't taken the last
// copy after 5 s, it is formatted here (so the report never goes missing).
static volatile int PerfPending;
static unsigned long long PerfPendingSince;
void Vita_PerfLogThreadHook()
{
	if (__atomic_load_n(&PerfPending, __ATOMIC_ACQUIRE))
	{
		Vita_PerfFormat(PerfSnap);
		__atomic_store_n(&PerfPending, 0, __ATOMIC_RELEASE);
	}
}
static void Vita_PerfSnapshot(int frames, unsigned long long period, unsigned long long worst)
{
	if (__atomic_load_n(&PerfPending, __ATOMIC_ACQUIRE))
	{
		if (Vita_PerfNow() - PerfPendingSince < 5000000ull) return;	// log thread still busy with the last one
		Vita_PerfFormat(PerfSnap);
		PerfPending = 0;
		Vita_BootLog("profiler report written by the game thread (log thread late)");
	}
	FPerfSnap &S = PerfSnap;
	S.Detail = Vita_PerfDetail;
	S.Frames = frames; S.Period = period; S.Worst = worst;
	{
		extern unsigned Vita_WallCacheHits, Vita_WallCacheMisses, Vita_SurfHits, Vita_SurfMisses;
		S.WallHits = Vita_WallCacheHits; S.WallMisses = Vita_WallCacheMisses; S.SurfHits = Vita_SurfHits; S.SurfMisses = Vita_SurfMisses;
		Vita_WallCacheHits = Vita_WallCacheMisses = Vita_SurfHits = Vita_SurfMisses = 0;
		extern unsigned long long Vita_IoUsMain, Vita_IoUsLogic;
		S.IoMain = Vita_IoUsMain; S.IoLogic = Vita_IoUsLogic; Vita_IoUsMain = Vita_IoUsLogic = 0;
	}
	memcpy(S.Acc, Vita_PerfAcc, sizeof(S.Acc));
	memcpy(S.Calls, Vita_PerfCalls, sizeof(S.Calls));
	S.GCTime = Vita_GCTime; S.LogicStartDelay = Vita_LogicStartDelay; S.LogicRunTime = Vita_LogicRunTime;
	S.HeapMB = Vita_HeapReservedMB(); S.BigKB = Vita_BigBlocksKB(); S.ObjMB = (unsigned)(GC::AllocBytes >> 20);
#ifndef NO_GL
	{
		// (0046: timed on its own; the report took 30-90 ms every 3 s)
		const unsigned long long g0 = Vita_PerfNow();
		static unsigned vram, vvram; static int age = 99;
		if (++age >= 10) { age = 0; vram = (unsigned)(vglMemFree(VGL_MEM_RAM) >> 20); vvram = (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 20); }
		S.VglRam = vram; S.VglVram = vvram;
		Vita_PerfGpuQueryUs = Vita_PerfNow() - g0;
	}
	S.PoolPeak = LegacyPoolPeak; S.PoolSize = (unsigned)(legacy_pool_size >> 10);
#endif
	if (S.Detail)
	{
		memcpy(S.Classes, PerfClasses, sizeof(FPerfClass) * NumPerfClasses);
		S.NumClasses = NumPerfClasses;
		memcpy(S.PcodeCounts, Vita_PcodeCounts, sizeof(S.PcodeCounts));
		memcpy(S.PcodeUs, Vita_PcodeUs, sizeof(S.PcodeUs));
		memcpy(S.AcsUs, AcsFuncUs, sizeof(S.AcsUs));
		memcpy(S.AcsCalls, AcsFuncCalls, sizeof(S.AcsCalls));
#ifndef NO_GL
		extern unsigned Vita_GLCount[8];
		memcpy(S.GLCount, Vita_GLCount, sizeof(S.GLCount));
		memcpy(S.GLPhase, Vita_GLPhaseDraws, sizeof(S.GLPhase));
		extern int rendered_lines, rendered_flats, rendered_sprites;
		S.RLines = rendered_lines; S.RFlats = rendered_flats; S.RSprites = rendered_sprites;
		extern void Vita_PrintDrawCallers(char *, size_t);
		Vita_PrintDrawCallers(S.Callers, sizeof(S.Callers));
#endif
	}
	// 0045: formatted right here again. 0044 handed this to its own thread,
	// whose output never appeared on the Vita (fine in Vita3K); the stalls
	// came from mallinfo(), which is gone.
	PerfPendingSince = Vita_PerfNow();
	__atomic_store_n(&PerfPending, 1, __ATOMIC_RELEASE);
	{ extern void Vita_LogKick(); Vita_LogKick(); }
}

unsigned Vita_FrameNo;
void Vita_PerfFrame()
{
	Vita_FrameNo++;
	Vita_FrameShown();
	static unsigned long long periodStart = 0, lastFrame = 0, worst = 0;
	static int frames = 0;
	const unsigned long long now = Vita_PerfNow();

	if (periodStart == 0) { periodStart = lastFrame = now; return; }
	if (now - lastFrame > worst) worst = now - lastFrame;
	lastFrame = now;
	frames++;

	const unsigned long long period = now - periodStart;
	if (period < 3000000) return;

	if (vita_perflog)
	{
		const unsigned long long vs0 = Vita_PerfNow();
		Vita_PerfSnapshot(frames, period, worst);
		const unsigned long long vs = Vita_PerfNow() - vs0;
		if (vs > 5000) Vita_BootLog("profiler report took %llu ms (GPU memory query %llu ms)", vs / 1000, Vita_PerfGpuQueryUs / 1000);
	}
#ifndef NO_GL
	{ extern unsigned Vita_GLCount[8]; memset(Vita_GLCount, 0, sizeof(Vita_GLCount)); memset(Vita_GLPhaseDraws, 0, sizeof(Vita_GLPhaseDraws)); }
#endif
	{ extern int rendered_lines, rendered_flats, rendered_sprites; rendered_lines = rendered_flats = rendered_sprites = 0; }
	for (int i = 0; i < 1024; i++) { Vita_PcodeUs[i] = 0; Vita_PcodeCounts[i] = 0; AcsFuncUs[i] = 0; AcsFuncCalls[i] = 0; }
	NumPerfClasses = 0;
	memset(PerfClassHash, 0, sizeof(PerfClassHash));
	for (int i = 0; i < VPERF_COUNT; i++) { Vita_PerfAcc[i] = 0; Vita_PerfCalls[i] = 0; }
	Vita_GCTime = 0; Vita_LogicStartDelay = 0; Vita_LogicRunTime = 0;
	frames = 0;
	worst = 0;
	{
		static int window = 0;
		window++;
		if (!vita_perfdetail)
			Vita_PerfDetail = vita_perfbursts && (window % 5) == 4;
	}
#ifndef NO_GL
	LegacyPoolPeak = 0;
#endif
	periodStart = now;
}

#ifndef NO_GL
// [Vita] GL call counters for the render detail line (linked with --wrap).
unsigned Vita_GLCount[8];	// draws, glBegin, binds, colour, fog, vertices

void Vita_SpriteBindHook(unsigned id);
extern "C" {
void __real_glDrawArrays(unsigned mode, int first, int count);
static void *Vita_DrawCallers[64]; static unsigned Vita_DrawCallerN[64];
static void Vita_NoteCaller(void *a) { unsigned h = ((uintptr_t)a >> 2) & 63; for (int k = 0; k < 64; k++, h = (h + 1) & 63) { if (Vita_DrawCallers[h] == a) { Vita_DrawCallerN[h]++; return; } if (Vita_DrawCallers[h] == NULL) { Vita_DrawCallers[h] = a; Vita_DrawCallerN[h] = 1; return; } } }
extern "C++" void Vita_PrintDrawCallers(char *buf, size_t n) { size_t len = 0; buf[0] = 0; for (int r = 0; r < 4; r++) { int b = -1; for (int i = 0; i < 64; i++) if (Vita_DrawCallerN[i] && (b < 0 || Vita_DrawCallerN[i] > Vita_DrawCallerN[b])) b = i; if (b < 0) break; len += snprintf(buf + len, n - len, " %p:%u", Vita_DrawCallers[b], Vita_DrawCallerN[b]); Vita_DrawCallerN[b] = 0; } memset(Vita_DrawCallers, 0, sizeof(Vita_DrawCallers)); memset(Vita_DrawCallerN, 0, sizeof(Vita_DrawCallerN)); }
void __wrap_glDrawArrays(unsigned mode, int first, int count) { if (Vita_PerfDetail && Vita_GLPhase == 1) Vita_NoteCaller(__builtin_return_address(0)); Vita_GLCount[0]++; Vita_GLPhaseDraws[Vita_GLPhase]++; Vita_GLCount[5] += count; __real_glDrawArrays(mode, first, count); }
void __real_glDrawElements(unsigned mode, int count, unsigned type, const void *ind);
void __wrap_glDrawElements(unsigned mode, int count, unsigned type, const void *ind) { Vita_GLCount[0]++; Vita_GLPhaseDraws[Vita_GLPhase]++; Vita_GLCount[5] += count; __real_glDrawElements(mode, count, type, ind); }
void __real_glDrawRangeElements(unsigned mode, unsigned s, unsigned e, int count, unsigned type, const void *ind);
void __wrap_glDrawRangeElements(unsigned mode, unsigned s, unsigned e, int count, unsigned type, const void *ind) { Vita_GLCount[0]++; Vita_GLPhaseDraws[Vita_GLPhase]++; Vita_GLCount[5] += count; __real_glDrawRangeElements(mode, s, e, count, type, ind); }
void __real_glBegin(unsigned mode);
void __wrap_glBegin(unsigned mode) { Vita_GLCount[1]++; Vita_GLPhaseDraws[Vita_GLPhase]++; __real_glBegin(mode); }
void __real_glBindTexture(unsigned t, unsigned id);

void __wrap_glBindTexture(unsigned t, unsigned id) { Vita_SpriteBindHook(id); Vita_GLCount[2]++; __real_glBindTexture(t, id); }
void __real_glColor4f(float r, float g, float b, float a);
static inline unsigned Vita_ClampByte(float v) { int i = (int)(v * 255.f + 0.5f); return i < 0 ? 0 : i > 255 ? 255 : i; }
static inline void Vita_NoteColor(float r, float g, float b, float a) { extern unsigned Vita_CurColor; Vita_CurColor = Vita_ClampByte(r) | (Vita_ClampByte(g) << 8) | (Vita_ClampByte(b) << 16) | (Vita_ClampByte(a) << 24); }
void __wrap_glColor4f(float r, float g, float b, float a) { Vita_GLCount[3]++; Vita_NoteColor(r, g, b, a); __real_glColor4f(r, g, b, a); }
void __real_glColor3f(float r, float g, float b);
void __wrap_glColor3f(float r, float g, float b) { Vita_GLCount[3]++; Vita_NoteColor(r, g, b, 1.f); __real_glColor3f(r, g, b); }
void __real_glColor4fv(const float *v);
void __wrap_glColor4fv(const float *v) { Vita_GLCount[3]++; Vita_NoteColor(v[0], v[1], v[2], v[3]); __real_glColor4fv(v); }
void __real_glColor3fv(const float *v);
void __wrap_glColor3fv(const float *v) { Vita_GLCount[3]++; Vita_NoteColor(v[0], v[1], v[2], 1.f); __real_glColor3fv(v); }
void __real_glColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void __wrap_glColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { extern unsigned Vita_CurColor; Vita_CurColor = r | (g << 8) | (b << 16) | ((unsigned)a << 24); __real_glColor4ub(r, g, b, a); }
void __real_glColor3ub(unsigned char r, unsigned char g, unsigned char b);
void __wrap_glColor3ub(unsigned char r, unsigned char g, unsigned char b) { extern unsigned Vita_CurColor; Vita_CurColor = r | (g << 8) | (b << 16) | 0xff000000u; __real_glColor3ub(r, g, b); }
void __real_glFogf(unsigned p, float v);
void __wrap_glFogf(unsigned p, float v) { Vita_GLCount[4]++; __real_glFogf(p, v); }
void __real_glFogfv(unsigned p, const float *v);
void __wrap_glFogfv(unsigned p, const float *v) { Vita_GLCount[4]++; __real_glFogfv(p, v); }
}
#endif

// [Vita] ACS watchdog: a thread on the third core notices when one ACS
// instruction (not in the fast path) runs for over 30 ms and logs which
// one once it's done (0042: a 26-instruction bot script took 224 ms).
#include <psp2/kernel/threadmgr.h>
volatile int Vita_ACSPcd, Vita_ACSScript, Vita_ACSActive;
volatile unsigned Vita_ACSSeq;
static int Vita_ACSWatchFunc(SceSize, void *)
{
	unsigned last = 0;
	int pcd = 0, script = 0;
	unsigned long long since = 0;
	bool slow = false;
	for (;;)
	{
		sceKernelDelayThread(10000);
		const unsigned seq = Vita_ACSSeq;
		const unsigned long long now = sceKernelGetProcessTimeWide();
		if (seq != last)
		{
			if (slow) Vita_BootLog("slow ACS instruction: pcode %d in script %d took about %llu ms", pcd, script, (now - since) / 1000);
			last = seq; since = now; pcd = Vita_ACSPcd; script = Vita_ACSScript; slow = false;
		}
		else if (Vita_ACSActive > 0 && !slow && now - since > 30000) slow = true;
	}
	return 0;
}
void Vita_ACSWatchStart()
{
	static bool started = false;
	if (started) return;
	started = true;
	SceUID t = sceKernelCreateThread("vita_acswatch", Vita_ACSWatchFunc, 0x10000100 + 50, 0x2000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
	if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

// [Vita] What a slow actor tick spent its time on (0044: HaloDoom's weapon
// pickups took 75-125 ms each at match start; no single ACS instruction was
// slow). Logic thread only; reset before each timed tick.
unsigned long long Vita_TickAcsUs, Vita_TickAcsLongestUs, Vita_TickSoundUs;
int Vita_TickAcsRuns;
static char Vita_TickAcsLongest[64];
void Vita_NoteTickScript(const char *name, unsigned long long us)
{
	Vita_TickAcsUs += us;
	Vita_TickAcsRuns++;
	if (us > Vita_TickAcsLongestUs)
	{
		Vita_TickAcsLongestUs = us;
		snprintf(Vita_TickAcsLongest, sizeof(Vita_TickAcsLongest), "%s", name ? name : "?");
	}
}
void Vita_TickBreakdownReset() { Vita_TickAcsUs = Vita_TickAcsLongestUs = Vita_TickSoundUs = 0; Vita_TickAcsRuns = 0; Vita_TickAcsLongest[0] = 0; }
const char *Vita_TickBreakdown()
{
	static char buf[200];
	snprintf(buf, sizeof(buf), "ACS %llu ms in %d runs (longest %s %llu ms), sound loads %llu ms",
		Vita_TickAcsUs / 1000, Vita_TickAcsRuns, Vita_TickAcsLongest[0] ? Vita_TickAcsLongest : "-", Vita_TickAcsLongestUs / 1000, Vita_TickSoundUs / 1000);
	return buf;
}

// [Vita] 0045: while a slow-tick-timed actor update runs, this thread samples
// the state of the thread running it every 2 ms: running, ready (wanting the
// CPU but preempted) or waiting (and on which kernel object). 0044 showed
// first ticks of 56-190 ms that spent 0 ms in ACS, sounds and file locks.
volatile SceUID Vita_TickWatchThread = -1;	// set by the ticking thread; -1 = idle
static volatile unsigned Vita_TickSamples[3];	// running, ready, waiting
static volatile SceUID Vita_TickWaitId;
static volatile unsigned Vita_TickWaitType, Vita_TickWaitIdCount;
// Game threads by name, so the watcher can say which ones ran while the
// watched thread waited.
struct FVitaNamedThread { SceUID Id; const char *Name; volatile unsigned RunWhileWaiting; };
static FVitaNamedThread Vita_Threads[16];
static volatile int Vita_NumThreads;
void Vita_RegisterThread(const char *name)
{
	const int n = __atomic_fetch_add(&Vita_NumThreads, 1, __ATOMIC_SEQ_CST);
	if (n >= 16) return;
	Vita_Threads[n].Name = name;
	Vita_Threads[n].RunWhileWaiting = 0;
	__atomic_store_n(&Vita_Threads[n].Id, sceKernelGetThreadId(), __ATOMIC_SEQ_CST);
}

static int Vita_TickWatchFunc(SceSize, void *)
{
	SceUID lastId = -1; unsigned lastCount = 0;
	for (;;)
	{
		sceKernelDelayThread(2000);
		const SceUID t = Vita_TickWatchThread;
		if (t < 0) { lastId = -1; lastCount = 0; continue; }
		SceKernelThreadInfo info;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelGetThreadInfo(t, &info) < 0) continue;
		if (!(info.status & SCE_THREAD_RUNNING))
		{
			// who runs instead
			const int n = Vita_NumThreads < 16 ? Vita_NumThreads : 16;
			for (int i = 0; i < n; i++)
			{
				const SceUID id = Vita_Threads[i].Id;
				if (id <= 0 || id == t) continue;
				SceKernelThreadInfo oi; memset(&oi, 0, sizeof(oi)); oi.size = sizeof(oi);
				if (sceKernelGetThreadInfo(id, &oi) >= 0 && (oi.status & SCE_THREAD_RUNNING)) Vita_Threads[i].RunWhileWaiting++;
			}
		}
		if (info.status & SCE_THREAD_RUNNING) Vita_TickSamples[0]++;
		else if (info.status & SCE_THREAD_READY) Vita_TickSamples[1]++;
		else
		{
			Vita_TickSamples[2]++;
			if (info.waitId == lastId) lastCount++;
			else { lastId = info.waitId; lastCount = 1; }
			if (lastCount > Vita_TickWaitIdCount) { Vita_TickWaitIdCount = lastCount; Vita_TickWaitId = info.waitId; Vita_TickWaitType = info.waitType; }
		}
	}
	return 0;
}
unsigned long long Vita_TickLogUs, Vita_TickScriptStartUs;
void Vita_TickWatchBegin()
{
	static bool started = false;
	if (!started)
	{
		started = true;
		SceUID t = sceKernelCreateThread("vita_tickwatch", Vita_TickWatchFunc, 0x10000100 + 10, 0x2000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
		if (t >= 0) sceKernelStartThread(t, 0, NULL);
		Vita_BootLog("tick watcher %s", t >= 0 ? "started" : "could not start");
	}
	Vita_TickSamples[0] = Vita_TickSamples[1] = Vita_TickSamples[2] = 0;
	Vita_TickWaitId = -1; Vita_TickWaitType = 0; Vita_TickWaitIdCount = 0;
	for (int i = 0; i < 16; i++) Vita_Threads[i].RunWhileWaiting = 0;
	Vita_TickLogUs = Vita_TickScriptStartUs = 0;
	{ extern unsigned long long Vita_TickMallocUs, Vita_TickIoUs; extern unsigned Vita_TickMallocCalls; Vita_TickMallocUs = 0; Vita_TickMallocCalls = 0; Vita_TickIoUs = 0; }
	Vita_TickWatchThread = sceKernelGetThreadId();
}
void Vita_TickWatchEnd() { Vita_TickWatchThread = -1; }
const char *Vita_TickWatchReport()
{
	static char buf[420];
	char what[48] = "-";
	if (Vita_TickWaitIdCount > 0)
	{
		SceKernelMutexInfo mi; memset(&mi, 0, sizeof(mi)); mi.size = sizeof(mi);
		SceKernelSemaInfo si; memset(&si, 0, sizeof(si)); si.size = sizeof(si);
		if (sceKernelGetMutexInfo(Vita_TickWaitId, &mi) >= 0) snprintf(what, sizeof(what), "mutex %s", mi.name);
		else if (sceKernelGetSemaInfo(Vita_TickWaitId, &si) >= 0) snprintf(what, sizeof(what), "semaphore %s", si.name);
		else snprintf(what, sizeof(what), "type %x id %x", Vita_TickWaitType, (unsigned)Vita_TickWaitId);
	}
	extern unsigned long long Vita_TickMallocUs, Vita_TickIoUs; extern unsigned Vita_TickMallocCalls;
	char others[96] = "";
	{
		size_t ol = 0;
		const int n = Vita_NumThreads < 16 ? Vita_NumThreads : 16;
		for (int i = 0; i < n && ol < sizeof(others) - 24; i++)
			if (Vita_Threads[i].RunWhileWaiting > 0)
				ol += snprintf(others + ol, sizeof(others) - ol, "%s%s %u", ol ? ", " : "", Vita_Threads[i].Name, Vita_Threads[i].RunWhileWaiting);
		if (ol == 0) snprintf(others, sizeof(others), "none of the game's threads");
	}
	snprintf(buf, sizeof(buf), "samples every 2 ms: running %u, preempted %u, waiting %u (mostly on %s); memory allocation %llu ms in %u calls, memory card %llu ms, log writing %llu ms, script starts %llu ms; running meanwhile: %s",
		Vita_TickSamples[0], Vita_TickSamples[1], Vita_TickSamples[2], what, Vita_TickMallocUs / 1000, Vita_TickMallocCalls, Vita_TickIoUs / 1000, Vita_TickLogUs / 1000, Vita_TickScriptStartUs / 1000, others);
	return buf;
}
