// [Vita] See vita_perf.h.
#include <stdio.h>
#include <malloc.h>
#include "vita_perf.h"
#include "vita_system.h"
#include "c_cvars.h"
#ifndef NO_GL
#include <vitaGL.h>
#endif

CVAR(Bool, vita_perflog, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Bool, vita_perfdetail, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	Vita_PerfDetail = self;
}

bool Vita_PerfDetail;

unsigned long long Vita_PerfAcc[VPERF_COUNT];

#ifndef NO_GL
// vitaGL's immediate-mode vertex pool for the current frame.
extern "C" float *legacy_pool, *legacy_pool_ptr;
static unsigned LegacyPoolPeak;
#endif
int Vita_PerfCalls[VPERF_COUNT];

struct FPerfClass { const void *Cls; const char *Name; unsigned long long Us; int Calls; };
static FPerfClass PerfClasses[512];
static int NumPerfClasses;

void Vita_PerfThinker(const void *cls, const char *name, unsigned long long us)
{
	for (int i = 0; i < NumPerfClasses; i++)
	{
		if (PerfClasses[i].Cls == cls) { PerfClasses[i].Us += us; PerfClasses[i].Calls++; return; }
	}
	if (NumPerfClasses < 512)
	{
		PerfClasses[NumPerfClasses].Cls = cls;
		PerfClasses[NumPerfClasses].Name = name;
		PerfClasses[NumPerfClasses].Us = us;
		PerfClasses[NumPerfClasses].Calls = 1;
		NumPerfClasses++;
	}
}

unsigned Vita_PcodeCounts[1024];
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
	if (legacy_pool != NULL && legacy_pool_ptr >= legacy_pool)
	{
		unsigned used = (unsigned)((char *)legacy_pool_ptr - (char *)legacy_pool);
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
		char line[1024];
		snprintf(line, sizeof(line),
			"[perf] %.1f fps, worst frame %.0f ms | per frame (ms): logic %.1f, 3D %.1f (models %.1f), 2D+swap %.1f (swap %.1f)"
			" | in %.0f s: %d texture uploads %.0f ms, %d model loads %.0f ms | heap %u MB"
#ifndef NO_GL
			", vitaGL free RAM %u MB CDRAM %u MB"
#endif
			"\n",
			frames * 1e6 / period, worst / 1000.0,
			Vita_PerfAcc[VPERF_TIC] * f / 1000.0, Vita_PerfAcc[VPERF_SCENE] * f / 1000.0,
			Vita_PerfAcc[VPERF_MODELS] * f / 1000.0, Vita_PerfAcc[VPERF_UPDATE] * f / 1000.0,
			Vita_PerfAcc[VPERF_SWAP] * f / 1000.0,
			period / 1e6, Vita_PerfCalls[VPERF_UPLOAD], Vita_PerfAcc[VPERF_UPLOAD] / 1000.0,
			Vita_PerfCalls[VPERF_MODELLOAD], Vita_PerfAcc[VPERF_MODELLOAD] / 1000.0,
			(unsigned)(mi.uordblks >> 20)
#ifndef NO_GL
			, (unsigned)(vglMemFree(VGL_MEM_RAM) >> 20), (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 20)
#endif
			);
		Vita_ConsoleWrite(line);
	}
	if (vita_perflog && Vita_PerfDetail)
	{
		const double f = frames > 0 ? 1.0 / frames : 0;
		char line[1024];

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
			len += snprintf(line + len, sizeof(line) - len, " %s %.1f (x%d)", PerfClasses[best].Name,
				PerfClasses[best].Us * f / 1000.0, (int)(PerfClasses[best].Calls * f + 0.5));
			PerfClasses[best].Us = 0;
		}
		snprintf(line + len, sizeof(line) - len, "\n");
		Vita_ConsoleWrite(line);

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
	for (int i = 0; i < 1024; i++) { Vita_PcodeCounts[i] = 0; AcsFuncUs[i] = 0; AcsFuncCalls[i] = 0; }
	NumPerfClasses = 0;
	for (int i = 0; i < VPERF_COUNT; i++) { Vita_PerfAcc[i] = 0; Vita_PerfCalls[i] = 0; }
	frames = 0;
	worst = 0;
#ifndef NO_GL
	LegacyPoolPeak = 0;
#endif
	periodStart = now;
}
