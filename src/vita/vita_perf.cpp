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

unsigned long long Vita_PerfAcc[VPERF_COUNT];
int Vita_PerfCalls[VPERF_COUNT];

struct FPerfClass { const void *Cls; const char *Name; unsigned long long Us; int Calls; };
static FPerfClass PerfClasses[256];
static int NumPerfClasses;

void Vita_PerfThinker(const void *cls, const char *name, unsigned long long us)
{
	for (int i = 0; i < NumPerfClasses; i++)
	{
		if (PerfClasses[i].Cls == cls) { PerfClasses[i].Us += us; PerfClasses[i].Calls++; return; }
	}
	if (NumPerfClasses < 256)
	{
		PerfClasses[NumPerfClasses].Cls = cls;
		PerfClasses[NumPerfClasses].Name = name;
		PerfClasses[NumPerfClasses].Us = us;
		PerfClasses[NumPerfClasses].Calls = 1;
		NumPerfClasses++;
	}
}

void Vita_PerfFrame()
{
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
		char line[512];
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

		// Where the logic time goes: thinkers in total, GC, database, and the
		// most expensive thinker classes (ms per frame, ticks per frame).
		size_t len = snprintf(line, sizeof(line), "[perf]   thinkers %.1f (gc %.1f), database %.1f (%d calls) | top:",
			Vita_PerfAcc[VPERF_THINKERS] * f / 1000.0, Vita_PerfAcc[VPERF_GC] * f / 1000.0,
			Vita_PerfAcc[VPERF_DB] * f / 1000.0, Vita_PerfCalls[VPERF_DB]);
		for (int n = 0; n < 6; n++)
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
	}
	NumPerfClasses = 0;
	for (int i = 0; i < VPERF_COUNT; i++) { Vita_PerfAcc[i] = 0; Vita_PerfCalls[i] = 0; }
	frames = 0;
	worst = 0;
	periodStart = now;
}
