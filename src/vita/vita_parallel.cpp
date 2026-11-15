// [Vita] Game logic on a second CPU core while the 3D scene is drawn.
// See vita_parallel.h.

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <exception>

#include <psp2/kernel/processmgr.h>
#include "doomtype.h"
#include "dobject.h"
#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "c_cvars.h"
#include "tarray.h"
#include "zstring.h"
#include "network.h"
#include "gl/dynlights/gl_dynlight.h"
#include "vita/vita_parallel.h"
#include "vita/vita_system.h"
#include "vita/vita_perf.h"

CVAR(Bool, vita_parallel, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

void TryRunTics();

volatile bool Vita_Overlap;
bool Vita_LogicDone;

static bool FrameAllowsOverlap;
static SceUID LogicThread = -1, StartSema = -1, DoneSema = -1;
static SceUID LogicThreadId = -1;
static std::exception_ptr LogicError;

// Recursive locks. Taken by both threads only for short stretches.
static SceKernelLwMutexWork ArchiveMutex, PrintMutex;
static bool LocksMade;

static void MakeLocks()
{
	if (LocksMade) return;
	sceKernelCreateLwMutex(&ArchiveMutex, "vita_archive", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
	sceKernelCreateLwMutex(&PrintMutex, "vita_print", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
	LocksMade = true;
}

void Vita_ArchiveLock()
{
	if (!LocksMade) MakeLocks();
	sceKernelLockLwMutex(&ArchiveMutex, 1, NULL);
}

void Vita_ArchiveUnlock()
{
	sceKernelUnlockLwMutex(&ArchiveMutex, 1);
}

void Vita_PrintLock()
{
	if (!LocksMade) MakeLocks();
	sceKernelLockLwMutex(&PrintMutex, 1, NULL);
}

void Vita_PrintUnlock()
{
	sceKernelUnlockLwMutex(&PrintMutex, 1);
}

bool Vita_OnLogicThread()
{
	return LogicThreadId >= 0 && sceKernelGetThreadId() == LogicThreadId;
}

void Vita_SetFrameAllowsOverlap(bool allow)
{
	FrameAllowsOverlap = allow;
}

bool Vita_ParallelAllowed()
{
	return FrameAllowsOverlap && vita_parallel && !Vita_Overlap && !Vita_LogicDone
		&& (NETWORK_GetState() == NETSTATE_SINGLE || NETWORK_GetState() == NETSTATE_SINGLE_MULTIPLAYER)
		&& gamestate == GS_LEVEL && gameaction == ga_nothing && !demoplayback;
}

//==========================================================================
//
// Deferred dynamic light (un)links
//
//==========================================================================

struct FDeferredLight { ADynamicLight *Light; bool Unlink; };
static TArray<FDeferredLight> DeferredLights;

bool Vita_DeferLight(ADynamicLight *light, bool unlink)
{
	if (!Vita_Overlap || !Vita_OnLogicThread())
		return false;
	for (unsigned i = 0; i < DeferredLights.Size(); i++)
	{
		if (DeferredLights[i].Light == light)
		{
			if (unlink) DeferredLights[i].Unlink = true;
			return true;
		}
	}
	FDeferredLight d = { light, unlink };
	DeferredLights.Push(d);
	return true;
}

struct FDeferredCommand { FString Cmd; int Key; };
static TArray<FDeferredCommand> DeferredCommands;

bool Vita_DeferCommand(const char *cmd, int keynum)
{
	if (!Vita_Overlap || !Vita_OnLogicThread())
		return false;
	FDeferredCommand c = { cmd, keynum };
	DeferredCommands.Push(c);
	return true;
}

void C_DoCommand (const char *cmd, int keynum);

static void RunDeferredCommands()
{
	TArray<FDeferredCommand> cmds = DeferredCommands;
	DeferredCommands.Clear();
	for (unsigned i = 0; i < cmds.Size(); i++)
		C_DoCommand(cmds[i].Cmd.GetChars(), cmds[i].Key);
}

static void RunDeferredLights()
{
	// Nothing was freed during the overlap (the GC was held), so all these
	// pointers are still valid.
	for (unsigned i = 0; i < DeferredLights.Size(); i++)
	{
		ADynamicLight *l = DeferredLights[i].Light;
		if (DeferredLights[i].Unlink || (l->ObjectFlags & OF_EuthanizeMe))
			l->UnlinkLight();
		else
			l->LinkLight();
	}
	DeferredLights.Clear();
}

//==========================================================================
//
// The logic thread
//
//==========================================================================

static volatile SceUInt64 LogicSignalTime;
// Frame trace (vita_paralleltrace N: log N frames to boot.log)
CVAR(Int, vita_paralleltrace, 0, 0)
static SceUInt64 TrStart, TrTic0, TrTicEnd; static int TrTics;
unsigned long long Vita_TrRenderStart, Vita_TrOpaqueEnd;
void Vita_TraceTic(bool begin)
{
	if (!Vita_OnLogicThread()) return;
	SceUInt64 t = sceKernelGetProcessTimeWide();
	if (begin) { if (TrTics == 0) TrTic0 = t; TrTics++; }
	else TrTicEnd = t;
}
unsigned long long Vita_LogicStartDelay, Vita_LogicRunTime;	// for the profiler

static int LogicEntry(SceSize, void *)
{
	LogicThreadId = sceKernelGetThreadId();
	for (;;)
	{
		sceKernelWaitSema(StartSema, 1, NULL);
		const SceUInt64 t0 = sceKernelGetProcessTimeWide();
		Vita_LogicStartDelay += t0 - LogicSignalTime;
		TrStart = t0; TrTics = 0; TrTic0 = TrTicEnd = t0;
		try
		{
			TryRunTics();
		}
		catch (...)
		{
			LogicError = std::current_exception();
		}
		Vita_LogicRunTime += sceKernelGetProcessTimeWide() - t0;
		sceKernelSignalSema(DoneSema, 1);
	}
	return 0;
}

static bool StartThread()
{
	if (LogicThread >= 0) return true;
	MakeLocks();
	StartSema = sceKernelCreateSema("vita_logic_go", 0, 0, 1, NULL);
	DoneSema = sceKernelCreateSema("vita_logic_done", 0, 0, 1, NULL);
	// Same priority as the main thread; on the second core, with the big
	// stack the game logic needs (deep ACS / actor recursion).
	LogicThread = sceKernelCreateThread("vita_logic", LogicEntry, 0x10000100, 4 * 1024 * 1024, 0,
		SCE_KERNEL_CPU_MASK_USER_1, NULL);
	if (LogicThread < 0)
	{
		Vita_BootLog("logic thread: create failed 0x%08X", LogicThread);
		vita_parallel = false;
		return false;
	}
	// Keep the main thread off the logic thread's core: threads of equal
	// priority on one core don't time-slice, so the logic would only run
	// while the main thread blocks.
	{
		const int before = sceKernelGetThreadCpuAffinityMask(0);
		int r = sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
		Vita_BootLog("main thread affinity was 0x%08X", before);
		Vita_BootLog("main thread pinned to core 0: 0x%08X (affinity now 0x%08X)", r,
			sceKernelGetThreadCpuAffinityMask(0));
	}
	sceKernelStartThread(LogicThread, 0, NULL);
	while (LogicThreadId < 0) sceKernelDelayThread(100);
	Vita_BootLog("logic thread started");
	return true;
}

void Vita_LogicBegin()
{
	if (!StartThread()) return;
	Vita_Overlap = true;
	LogicSignalTime = sceKernelGetProcessTimeWide();
	sceKernelSignalSema(StartSema, 1);
}

void Vita_LogicEnd()
{
	if (!Vita_Overlap) return;
	const SceUInt64 joinStart = sceKernelGetProcessTimeWide();
	{
		VITA_PERF(VPERF_JOIN);
		sceKernelWaitSema(DoneSema, 1, NULL);
	}
	if (vita_paralleltrace > 0)
	{
		const SceUInt64 now = sceKernelGetProcessTimeWide();
		Vita_BootLog("ptrace: render %+lld opaque end %+lld", (long long)(Vita_TrRenderStart - LogicSignalTime), (long long)(Vita_TrOpaqueEnd - LogicSignalTime));
		Vita_BootLog("ptrace: signal 0 start %+lld tics %d first %+lld lastend %+lld | join at %+lld done %+lld",
			(long long)(TrStart - LogicSignalTime), TrTics, (long long)(TrTic0 - LogicSignalTime),
			(long long)(TrTicEnd - LogicSignalTime), (long long)(joinStart - LogicSignalTime), (long long)(now - LogicSignalTime));
		vita_paralleltrace = vita_paralleltrace - 1;
	}
	Vita_Overlap = false;
	Vita_LogicDone = true;
	RunDeferredLights();
	RunDeferredCommands();
	if (LogicError)
	{
		std::exception_ptr e = LogicError;
		LogicError = nullptr;
		std::rethrow_exception(e);
	}
}

//==========================================================================
//
// Garbage collection between frames
//
//==========================================================================

unsigned Vita_GCSteps;			// for the profiler
unsigned long long Vita_GCTime;

void Vita_GCCatchUp()
{
	const SceUInt64 start = sceKernelGetProcessTimeWide();
	unsigned n = 0;
	while (GC::AllocBytes >= GC::Threshold && n < 4000)
	{
		GC::Step();
		n++;
		if ((n & 15) == 0 && sceKernelGetProcessTimeWide() - start > 4000)
			break;
	}
	Vita_GCSteps += n;
	Vita_GCTime += sceKernelGetProcessTimeWide() - start;
}
