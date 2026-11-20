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

// Without vita_perfdetail, every 5th 3-second window is profiled in detail
// anyway, so play-test logs always say where the time goes.
CVAR(Bool, vita_perfburst, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

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

void Vita_PerfThinker(const void *cls, const char *name, unsigned long long us)
{
	for (int i = 0; i < NumPerfClasses; i++)
	{
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

void Vita_PerfFrame()
{
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
		const double f = frames > 0 ? 1.0 / frames : 0;
		struct mallinfo mi = mallinfo();
		char line[2048];
		snprintf(line, sizeof(line),
			"[perf] %.1f fps, worst frame %.0f ms | per frame (ms): logic %.1f (players %.1f [bot AI %.1f, weapons %.1f, move %.1f, use %.1f], thinkers %.1f, hud %.1f, bot aim %.1f, particles %.1f), 3D %.1f (bsp %.1f, opaque %.1f [sky %.1f], waiting for logic %.1f, decals %.1f, portals+sky %.1f, translucent %.1f, models %.1f [expand %.1f draw %.1f bind %.1f]), 2D+swap %.1f (swap %.1f)"
			" | in %.0f s: %d texture uploads %.0f ms (decode %.0f ms), %d model loads %.0f ms | logic thread: start delay %.1f, run %.1f | heap %u MB + %u MB outside (objects %u MB, gc %.1f ms/frame)"
#ifndef NO_GL
			", vitaGL free RAM %u MB CDRAM %u MB, immediate-mode pool peak %u of %u KB"
#endif
			"\n",
			frames * 1e6 / period, worst / 1000.0,
			Vita_PerfAcc[VPERF_TIC] * f / 1000.0, Vita_PerfAcc[VPERF_PLAYERS] * f / 1000.0, Vita_PerfAcc[VPERF_BOTTICK] * f / 1000.0, Vita_PerfAcc[VPERF_PSPRITES] * f / 1000.0, Vita_PerfAcc[VPERF_PMOVE] * f / 1000.0, Vita_PerfAcc[VPERF_PUSE] * f / 1000.0, Vita_PerfAcc[VPERF_THINKERS] * f / 1000.0,
			Vita_PerfAcc[VPERF_SBAR] * f / 1000.0, Vita_PerfAcc[VPERF_BOTAIM] * f / 1000.0, Vita_PerfAcc[VPERF_EFFECTS] * f / 1000.0,
			Vita_PerfAcc[VPERF_SCENE] * f / 1000.0, Vita_PerfAcc[VPERF_BSP] * f / 1000.0,
			Vita_PerfAcc[VPERF_OPAQUE] * f / 1000.0, Vita_PerfAcc[VPERF_SKYBOX] * f / 1000.0, Vita_PerfAcc[VPERF_JOIN] * f / 1000.0,
			Vita_PerfAcc[VPERF_DECALS] * f / 1000.0, Vita_PerfAcc[VPERF_PORTALS] * f / 1000.0, Vita_PerfAcc[VPERF_TRANSL] * f / 1000.0,
			Vita_PerfAcc[VPERF_MODELS] * f / 1000.0, Vita_PerfAcc[VPERF_MEXPAND] * f / 1000.0, Vita_PerfAcc[VPERF_MDRAW] * f / 1000.0, Vita_PerfAcc[VPERF_MBIND] * f / 1000.0,
			Vita_PerfAcc[VPERF_UPDATE] * f / 1000.0,
			Vita_PerfAcc[VPERF_SWAP] * f / 1000.0,
			period / 1e6, Vita_PerfCalls[VPERF_UPLOAD], Vita_PerfAcc[VPERF_UPLOAD] / 1000.0, Vita_PerfAcc[VPERF_TEXDECODE] / 1000.0,
			Vita_PerfCalls[VPERF_MODELLOAD], Vita_PerfAcc[VPERF_MODELLOAD] / 1000.0,
			Vita_LogicStartDelay * f / 1000.0, Vita_LogicRunTime * f / 1000.0,
			(unsigned)(mi.uordblks >> 20), Vita_BigBlocksKB() >> 10, (unsigned)(GC::AllocBytes >> 20), Vita_GCTime * f / 1000.0
#ifndef NO_GL
			, (unsigned)(vglMemFree(VGL_MEM_RAM) >> 20), (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 20), LegacyPoolPeak >> 10, (unsigned)(legacy_pool_size >> 10)
#endif
			);
		Vita_ConsoleWrite(line);
	}
	if (vita_perflog && Vita_PerfDetail)
	{
		const double f = frames > 0 ? 1.0 / frames : 0;
		char line[2048];

		// Where the logic time goes: thinkers in total, GC, database, and the
		// most expensive thinker classes (ms per frame, ticks per frame).
		size_t len = snprintf(line, sizeof(line), "[perf]   thinkers %.1f (gc %.1f), database %.1f (%d calls), ACS starts %.1f (x%d), cvar lookups %.1f (x%d) | top:",
			Vita_PerfAcc[VPERF_THINKERS] * f / 1000.0, Vita_PerfAcc[VPERF_GC] * f / 1000.0,
			Vita_PerfAcc[VPERF_DB] * f / 1000.0, Vita_PerfCalls[VPERF_DB],
			Vita_PerfAcc[VPERF_ACSNEW] * f / 1000.0, (int)(Vita_PerfCalls[VPERF_ACSNEW] * f + 0.5),
			Vita_PerfAcc[VPERF_FINDCVAR] * f / 1000.0, (int)(Vita_PerfCalls[VPERF_FINDCVAR] * f + 0.5));
		for (int n = 0; n < 10; n++)
		{
			int best = -1;
			for (int i = 0; i < NumPerfClasses; i++)
				if (PerfClasses[i].Us > 0 && (best < 0 || PerfClasses[i].Us > PerfClasses[best].Us)) best = i;
			if (best < 0 || len >= sizeof(line) - 64) break;
			if (PerfClasses[best].Ops > 0)
				len += snprintf(line + len, sizeof(line) - len, " %s %.1f (x%d, %.0f op)", PerfClasses[best].Name,
					PerfClasses[best].Us * f / 1000.0, (int)(PerfClasses[best].Calls * f + 0.5), PerfClasses[best].Ops * f);
			else
				len += snprintf(line + len, sizeof(line) - len, " %s %.1f (x%d)", PerfClasses[best].Name,
					PerfClasses[best].Us * f / 1000.0, (int)(PerfClasses[best].Calls * f + 0.5));
			PerfClasses[best].Ops = 0;
			PerfClasses[best].Us = 0;
		}
		snprintf(line + len, sizeof(line) - len, "\n");
		Vita_ConsoleWrite(line);

#ifndef NO_GL
		{
			extern unsigned Vita_GLCount[8];
			extern int rendered_lines, rendered_flats, rendered_sprites;
			snprintf(line, sizeof(line), "[perf]   render per frame: walls %.0f, flats %.0f, sprites %.0f | GL draw calls %.0f (vertices %.0f), glBegin %.0f, texture binds %.0f, colour changes %.0f, fog changes %.0f | draws+glBegin: opaque %.0f, translucent %.0f, models %.0f, weapon/blend %.0f, 2D %.0f\n",
				rendered_lines * f, rendered_flats * f, rendered_sprites * f,
				Vita_GLCount[0] * f, Vita_GLCount[5] * f, Vita_GLCount[1] * f, Vita_GLCount[2] * f, Vita_GLCount[3] * f, Vita_GLCount[4] * f,
				Vita_GLPhaseDraws[1] * f, Vita_GLPhaseDraws[2] * f, Vita_GLPhaseDraws[4] * f, Vita_GLPhaseDraws[3] * f, Vita_GLPhaseDraws[0] * f);
			Vita_ConsoleWrite(line);
			extern void Vita_PrintDrawCallers(char *, size_t);
			char cb[256]; Vita_PrintDrawCallers(cb, sizeof(cb));
			snprintf(line, sizeof(line), "[perf]   opaque glDrawArrays callers (3 s):%s | RenderScene %.1f/frame, DrawSubsectors %.0f/frame\n", cb, Vita_GLCount[6] * f, Vita_GLCount[7] * f);
			Vita_ConsoleWrite(line);
		}
#endif
		// ACS: most executed opcodes and the most expensive ACSF functions.
		unsigned total = 0;
		for (int i = 0; i < 1024; i++) total += Vita_PcodeCounts[i];
		len = snprintf(line, sizeof(line), "[perf]   ACS opcodes/frame %.0f | top:", total * f);
		for (int n = 0; n < 12; n++)
		{
			int best = -1;
			for (int i = 0; i < 1024; i++)
				if (Vita_PcodeCounts[i] > 0 && (best < 0 || Vita_PcodeCounts[i] > Vita_PcodeCounts[best])) best = i;
			if (best < 0 || len >= sizeof(line) - 32) break;
			len += snprintf(line + len, sizeof(line) - len, " %d:%.0f", best, Vita_PcodeCounts[best] * f);
			Vita_PcodeCounts[best] = 0;
		}
		len += snprintf(line + len, sizeof(line) - len, " | op ms:");
		for (int n = 0; n < 8; n++)
		{
			int best = -1;
			for (int i = 0; i < 1024; i++)
				if (Vita_PcodeUs[i] > 0 && (best < 0 || Vita_PcodeUs[i] > Vita_PcodeUs[best])) best = i;
			if (best < 0 || len >= sizeof(line) - 40) break;
			len += snprintf(line + len, sizeof(line) - len, " %d:%.2f", best, Vita_PcodeUs[best] * f / 1000.0);
			Vita_PcodeUs[best] = 0;
		}
		len += snprintf(line + len, sizeof(line) - len, " | ACSF ms:");
		for (int n = 0; n < 8; n++)
		{
			int best = -1;
			for (int i = 0; i < 1024; i++)
				if (AcsFuncUs[i] > 0 && (best < 0 || AcsFuncUs[i] > AcsFuncUs[best])) best = i;
			if (best < 0 || len >= sizeof(line) - 40) break;
			len += snprintf(line + len, sizeof(line) - len, " f%d %.2f (x%.0f)", best, AcsFuncUs[best] * f / 1000.0, AcsFuncCalls[best] * f);
			AcsFuncUs[best] = 0;
		}
		snprintf(line + len, sizeof(line) - len, "\n");
		Vita_ConsoleWrite(line);
	}
#ifndef NO_GL
	{ extern unsigned Vita_GLCount[8]; memset(Vita_GLCount, 0, sizeof(Vita_GLCount)); memset(Vita_GLPhaseDraws, 0, sizeof(Vita_GLPhaseDraws)); }
#endif
	{ extern int rendered_lines, rendered_flats, rendered_sprites; rendered_lines = rendered_flats = rendered_sprites = 0; }
	for (int i = 0; i < 1024; i++) { Vita_PcodeUs[i] = 0; Vita_PcodeCounts[i] = 0; AcsFuncUs[i] = 0; AcsFuncCalls[i] = 0; }
	NumPerfClasses = 0;
	for (int i = 0; i < VPERF_COUNT; i++) { Vita_PerfAcc[i] = 0; Vita_PerfCalls[i] = 0; }
	Vita_GCTime = 0; Vita_LogicStartDelay = 0; Vita_LogicRunTime = 0;
	frames = 0;
	worst = 0;
	{
		static int window = 0;
		window++;
		if (!vita_perfdetail)
			Vita_PerfDetail = vita_perfburst && (window % 5) == 4;
	}
#ifndef NO_GL
	LegacyPoolPeak = 0;
#endif
	periodStart = now;
}

#ifndef NO_GL
// [Vita] GL call counters for the render detail line (linked with --wrap).
unsigned Vita_GLCount[8];	// draws, glBegin, binds, colour, fog, vertices

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
void __wrap_glBindTexture(unsigned t, unsigned id) { Vita_GLCount[2]++; __real_glBindTexture(t, id); }
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
