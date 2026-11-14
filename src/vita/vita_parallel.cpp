// [Vita] Game logic on a second CPU core while the 3D scene is drawn.
// See vita_parallel.h.

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <exception>

#include "doomtype.h"
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

static int LogicEntry(SceSize, void *)
{
	LogicThreadId = sceKernelGetThreadId();
	for (;;)
	{
		sceKernelWaitSema(StartSema, 1, NULL);
		try
		{
			TryRunTics();
		}
		catch (...)
		{
			LogicError = std::current_exception();
		}
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
	sceKernelStartThread(LogicThread, 0, NULL);
	while (LogicThreadId < 0) sceKernelDelayThread(100);
	Vita_BootLog("logic thread started");
	return true;
}

void Vita_LogicBegin()
{
	if (!StartThread()) return;
	Vita_Overlap = true;
	sceKernelSignalSema(StartSema, 1);
}

void Vita_LogicEnd()
{
	if (!Vita_Overlap) return;
	{
		VITA_PERF(VPERF_JOIN);
		sceKernelWaitSema(DoneSema, 1, NULL);
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
