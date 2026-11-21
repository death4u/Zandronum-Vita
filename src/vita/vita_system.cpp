// PS Vita platform support for Zandronum: process setup, boot logging and
// on-screen fatal errors.

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sqlite.h>
#include <psp2/apputil.h>

#include <SDL/SDL.h>

#include "vita_system.h"
#include <psp2/message_dialog.h>
#include <psp2/common_dialog.h>

// ---------------------------------------------------------------------------
// Process memory. The SFO sets ATTRIBUTE2=12 (extended memory mode), which
// gives the app roughly 365 MB of main memory; the newlib heap takes 224 MB
// of it and vitaGL/SDL's GPU pools, the network pool and thread stacks the rest.
// Zandronum recurses deeply (BSP traversal, ACS, parsers), so the main
// thread gets a large stack instead of the 256 KB default.
// ---------------------------------------------------------------------------
extern "C" {
int _newlib_heap_size_user = 224 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
}

#define VITA_DATA_ROOT "ux0:data/zandronum"
#define VITA_BOOT_LOG VITA_DATA_ROOT "/boot.log"

static void Vita_MkDir(const char *path)
{
	SceIoStat st;
	if (sceIoGetstat(path, &st) < 0)
		sceIoMkdir(path, 0777);
}

// [Vita] Log files are written by a thread on the third core: opening,
// writing and closing a file on the memory card for every line took the
// main thread 100-200 ms whenever the profiler wrote its detail lines (a
// stall every 15 s in 0040). Lines wait in a buffer per file; the thread
// writes them out within a few ms. Before the thread runs (start-up) and
// on fatal errors they are written directly.
// [Vita] 0046: appending never waits. Each file has two buffers; writers
// reserve space in the active one with an atomic add and copy their line in,
// the log thread swaps buffers and waits (briefly) for writers still copying
// into the old one. In 0045 writers took a kernel mutex, and while the log
// thread holding it was pushed off its core, game threads waited 64-200 ms
// per log line.
struct FVitaLogBuf
{
	char Data[96 * 1024];
	volatile unsigned Len;		// reserved bytes (may exceed the size: dropped)
	volatile int Writers;		// writers still copying in
};
struct FVitaLogFile
{
	const char *Path;
	FVitaLogBuf Buf[2];
	volatile int Active;
	volatile unsigned Dropped;
	bool Truncate;		// first write empties the file
};
static FVitaLogFile VitaLogs[2];
static void Vita_LogInitFiles()
{
	if (VitaLogs[0].Path != NULL) return;
	VitaLogs[0].Path = VITA_BOOT_LOG;
	VitaLogs[1].Path = VITA_DATA_ROOT "/console.txt";
	VitaLogs[1].Truncate = true;
}
static SceUID VitaLogSema = -1, VitaLogThread = -1;
static volatile int VitaLogPending;

static void Vita_LogWriteDirect(FVitaLogFile &f, const char *text, unsigned len)
{
	Vita_LogInitFiles();
	// Opened per write: Vita3K only commits a file's data when it is closed.
	SceUID fd = sceIoOpen(f.Path, SCE_O_WRONLY | SCE_O_CREAT | (f.Truncate ? SCE_O_TRUNC : SCE_O_APPEND), 0666);
	f.Truncate = false;
	if (fd >= 0)
	{
		sceIoWrite(fd, text, len);
		sceIoClose(fd);
	}
}

static void Vita_LogFlushFile(int i)
{
	FVitaLogFile &f = VitaLogs[i];
	const int old = f.Active;
	FVitaLogBuf &b = f.Buf[old];
	if (b.Len == 0) return;
	f.Buf[1 - old].Len = 0;
	__atomic_store_n(&f.Active, 1 - old, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&b.Writers, __ATOMIC_SEQ_CST) != 0) sceKernelDelayThread(100);
	unsigned len = b.Len;
	if (len > sizeof(b.Data)) len = sizeof(b.Data);
	if (len > 0) Vita_LogWriteDirect(f, b.Data, len);
	const unsigned dropped = __atomic_exchange_n(&f.Dropped, 0u, __ATOMIC_SEQ_CST);
	if (dropped > 0)
	{
		char msg[64];
		int n = snprintf(msg, sizeof(msg), "(log: %u lines dropped, buffer full)\n", dropped);
		Vita_LogWriteDirect(f, msg, n);
	}
	b.Len = 0;
}

static int Vita_LogThreadFunc(SceSize, void *)
{
	{ extern void Vita_RegisterThread(const char *); Vita_RegisterThread("log writer"); }
	for (;;)
	{
		sceKernelWaitSema(VitaLogSema, 1, NULL);
		// written at most once a second (0045)
		sceKernelDelayThread(1000000);
		VitaLogPending = 0;
		for (int i = 0; i < 2; i++) Vita_LogFlushFile(i);
	}
	return 0;
}

static void Vita_LogStartThread()
{
	VitaLogSema = sceKernelCreateSema("vita_log", 0, 0, 0x7fffffff, NULL);
	SceUID t = sceKernelCreateThread("vita_log", Vita_LogThreadFunc, 0x10000100 + 40, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
	if (VitaLogSema >= 0 && t >= 0)
	{
		VitaLogThread = t;
		sceKernelStartThread(t, 0, NULL);
	}
}

static void Vita_LogAppend(int file, const char *text, unsigned len)
{
	FVitaLogFile &f = VitaLogs[file];
	if (VitaLogThread < 0)
	{
		Vita_LogWriteDirect(f, text, len);
		return;
	}
	for (;;)
	{
		const int a = __atomic_load_n(&f.Active, __ATOMIC_SEQ_CST);
		FVitaLogBuf &b = f.Buf[a];
		__atomic_add_fetch(&b.Writers, 1, __ATOMIC_SEQ_CST);
		if (__atomic_load_n(&f.Active, __ATOMIC_SEQ_CST) != a)
		{	// swapped meanwhile: use the new one
			__atomic_sub_fetch(&b.Writers, 1, __ATOMIC_SEQ_CST);
			continue;
		}
		const unsigned pos = __atomic_fetch_add(&b.Len, len, __ATOMIC_SEQ_CST);
		if (pos + len <= sizeof(b.Data)) memcpy(b.Data + pos, text, len);
		else __atomic_add_fetch(&f.Dropped, 1u, __ATOMIC_SEQ_CST);
		__atomic_sub_fetch(&b.Writers, 1, __ATOMIC_SEQ_CST);
		break;
	}
	if (!VitaLogPending) { VitaLogPending = 1; sceKernelSignalSema(VitaLogSema, 1); }
}

// Writes out everything pending, from this thread (fatal errors, exit).
extern "C" void Vita_LogFlushNow(void)
{
	if (VitaLogThread < 0) return;
	for (int i = 0; i < 2; i++) Vita_LogFlushFile(i);
}

extern "C" void Vita_BootLog(const char *fmt, ...)
{
	char buf[1024];
	// Seconds since the app started, so slow start-up steps stand out.
	unsigned long long us = sceKernelGetProcessTimeWide();
	int pre = snprintf(buf, sizeof(buf), "%4u.%02u ", (unsigned)(us / 1000000), (unsigned)(us / 10000 % 100));
	va_list ap;
	va_start(ap, fmt);
	int len = vsnprintf(buf + pre, sizeof(buf) - 2 - pre, fmt, ap);
	va_end(ap);
	if (len < 0)
		return;
	len += pre;
	if (len > (int)sizeof(buf) - 2)
		len = sizeof(buf) - 2;
	buf[len++] = '\n';
	Vita_LogAppend(0, buf, len);
	{
		extern volatile SceUID Vita_TickWatchThread;
		extern unsigned long long Vita_TickLogUs;
		if (Vita_TickWatchThread >= 0 && Vita_TickWatchThread == sceKernelGetThreadId()) Vita_TickLogUs += sceKernelGetProcessTimeWide() - us;
	}
}

// [Vita] System events (PS button, quit requests) are logged to boot.log,
// with a sign of life every 5 s while the game is in the background, to find
// out what happens when the game gets closed from the system.
#include <psp2/appmgr.h>
static int Vita_EventThread(SceSize, void *)
{
	bool background = false;
	unsigned ticks = 0;
	for (;;)
	{
		SceAppMgrSystemEvent ev;
		memset(&ev, 0, sizeof(ev));
		static unsigned lastEvent, lastTick;
		for (int k = 0; k < 4 && sceAppMgrReceiveSystemEvent(&ev) == 0; k++)
		{
			// (Vita3K returns the same event over and over)
			if (ev.systemEvent == lastEvent && ticks - lastTick < 50) { lastTick = ticks; break; }
			lastEvent = ev.systemEvent; lastTick = ticks;
			const char *what = ev.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_DEACTIVATE ? "deactivated (PS button / system screen)" :
				ev.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_ACTIVATE ? "activated again" :
				ev.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_RESUME ? "resumed from suspend" :
				ev.systemEvent == SCE_APPMGR_SYSTEMEVENT_REQUEST_QUIT ? "quit requested by the system" : "other";
			Vita_BootLog("system event 0x%08x: %s", (unsigned)ev.systemEvent, what);
			if (ev.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_DEACTIVATE) background = true;
			if (ev.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_ACTIVATE || ev.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_RESUME) background = false;
			memset(&ev, 0, sizeof(ev));
		}
		sceKernelDelayThread(100 * 1000);
		++ticks;
		if (background && (ticks % 50) == 0) Vita_BootLog("still running in the background");
	}
	return 0;
}

static void Vita_StartEventWatch()
{
	SceUID t = sceKernelCreateThread("vita_events", Vita_EventThread, 0x10000100 + 60, 0x4000, 0, 0, NULL);
	if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

static void Vita_LogExit(void)
{
	Vita_BootLog("exit() called");
	Vita_LogFlushNow();
}

static void *Vita_SqliteMalloc(int size) { return malloc(size); }
static void *Vita_SqliteRealloc(void *p, int size) { return realloc(p, size); }
static void Vita_SqliteFree(void *p) { free(p); }

#define VITA_NET_MEMORY (1 * 1024 * 1024)
static char vita_net_memory[VITA_NET_MEMORY];

static SceUID Vita_MainThreadId = -1;	// see Vita_RetryAlloc

extern "C" void Vita_EarlyInit(void)
{
	// The data folders must exist before anything is logged.
	Vita_MkDir("ux0:data");
	Vita_MkDir(VITA_DATA_ROOT);
	Vita_MkDir(VITA_DATA_ROOT "/saves");
	Vita_MkDir(VITA_DATA_ROOT "/screenshots");
	sceIoRemove(VITA_BOOT_LOG);

	Vita_BootLog("Zandronum for PS Vita, built " __DATE__ " " __TIME__);
	Vita_MainThreadId = sceKernelGetThreadId();
	Vita_BootLog("heap %d MB, main stack %u KB",
		_newlib_heap_size_user / (1024 * 1024), sceUserMainThreadStackSize / 1024);
	{
		SceKernelFreeMemorySizeInfo info;
		info.size = sizeof(info);
		if (sceKernelGetFreeMemorySize(&info) >= 0)
			Vita_BootLog("free memory: user %d KB, cdram %d KB, phycont %d KB",
				info.size_user / 1024, info.size_cdram / 1024, info.size_phycont / 1024);
	}
	atexit(Vita_LogExit);
	Vita_StartEventWatch();
	Vita_LogStartThread();
	{ extern void Vita_RegisterThread(const char *); Vita_RegisterThread("main"); }

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	Vita_BootLog("clocks set");

	// Everything the engine prints goes to files in the data folder.
	if (!freopen(VITA_DATA_ROOT "/stdout.txt", "w", stdout))
		Vita_BootLog("could not redirect stdout: %s", strerror(errno));
	if (!freopen(VITA_DATA_ROOT "/stderr.txt", "w", stderr))
		Vita_BootLog("could not redirect stderr: %s", strerror(errno));
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	Vita_BootLog("stdout/stderr redirected");

	SceAppUtilInitParam initParam;
	SceAppUtilBootParam bootParam;
	memset(&initParam, 0, sizeof(initParam));
	memset(&bootParam, 0, sizeof(bootParam));
	sceAppUtilInit(&initParam, &bootParam);

	// SQLite comes from the system's SceSqlite module.
	int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE);
	Vita_BootLog("SceSqlite load: 0x%08X", ret);
	if (ret >= 0)
	{
		static SceSqliteMallocMethods methods = { Vita_SqliteMalloc, Vita_SqliteRealloc, Vita_SqliteFree };
		ret = sceSqliteConfigMallocMethods(&methods);
		Vita_BootLog("sceSqliteConfigMallocMethods: 0x%08X", ret);
	}

	// Network (UDP game traffic, master server).
	ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	Vita_BootLog("SceNet load: 0x%08X", ret);
	if (ret >= 0)
	{
		SceNetInitParam netParam;
		netParam.memory = vita_net_memory;
		netParam.size = VITA_NET_MEMORY;
		netParam.flags = 0;
		ret = sceNetInit(&netParam);
		Vita_BootLog("sceNetInit: 0x%08X", ret);
		ret = sceNetCtlInit();
		Vita_BootLog("sceNetCtlInit: 0x%08X", ret);
	}
}

extern "C" void Vita_FatalError(const char *text)
{
	static bool shown = false;
	Vita_BootLog("FATAL ERROR: %s", text);
	Vita_LogFlushNow();
	fprintf(stderr, "FATAL ERROR: %s\n", text);
	fflush(stderr);
	fflush(stdout);
	if (shown)
		return;
	shown = true;

	// The message dialog can hold about 500 bytes of UTF-8.
	char msg[512];
	snprintf(msg, sizeof(msg), "Zandronum stopped with an error:\n\n%s", text);
	msg[sizeof(msg) - 1] = '\0';
	// Zandronum text may contain color escapes (\x1c + one char); drop them.
	char *d = msg;
	for (const char *s = msg; *s; s++)
	{
		if (*s == '\x1c') { if (s[1]) s++; continue; }
		*d++ = *s;
	}
	*d = '\0';
	if (Vita_GLDialogFrame != NULL)
	{
		// vitaGL owns the GPU: run the dialog on its frames instead of SDL's.
		SceMsgDialogParam param;
		SceMsgDialogUserMessageParam msgParam;
		sceMsgDialogParamInit(&param);
		param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
		memset(&msgParam, 0, sizeof(msgParam));
		msgParam.msg = (const SceChar8 *)msg;
		msgParam.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
		param.userMsgParam = &msgParam;
		if (sceMsgDialogInit(&param) >= 0)
		{
			while (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING)
				Vita_GLDialogFrame();
			sceMsgDialogTerm();
		}
		return;
	}
	SDL_VITA_ShowMessageBox(msg);
}

void (*Vita_GLDialogFrame)(void) = NULL;

extern "C" void Vita_ConsoleWrite(const char *text)
{
	if (text == NULL || *text == 0) return;
	Vita_LogAppend(1, text, strlen(text));
}

extern "C" int Vita_HaveShaderCompiler(void)
{
	SceIoStat st;
	return sceIoGetstat("ur0:data/libshacccg.suprx", &st) >= 0 ||
		sceIoGetstat("ur0:data/external/libshacccg.suprx", &st) >= 0;
}

// ---------------------------------------------------------------------------
// Allocation failures are logged with their size and caller (the engine only
// reports "Failed to allocate memory"). Linked with --wrap=malloc/realloc.
// ---------------------------------------------------------------------------
#include <malloc.h>

extern "C" void *__real_malloc(size_t size);
extern "C" void *__real_realloc(void *p, size_t size);

unsigned Vita_BigBlocksKB();
static void Vita_LogAllocFailure(const char *what, size_t size, void *caller)
{
	struct mallinfo mi = mallinfo();
	Vita_BootLog("%s(%u) failed, caller %p; heap arena %u KB, in use %u KB, free %u KB; big blocks outside the heap %u KB",
		what, (unsigned)size, caller, (unsigned)mi.arena / 1024,
		(unsigned)mi.uordblks / 1024, (unsigned)mi.fordblks / 1024, Vita_BigBlocksKB());
}

// [Vita] When the heap is too fragmented for a big block (>= 1 MB), it is
// taken from the system as its own memory block instead (free user memory
// outside the heap). free/realloc/malloc_usable_size recognise those blocks.
extern "C" void __real_free(void *p);
extern "C" size_t __real_malloc_usable_size(void *p);

struct FVitaBigBlock { char *p; size_t size; SceUID uid; };
static FVitaBigBlock Vita_BigBlocks[128];
static volatile int Vita_BigCount;
static volatile int Vita_BigLock;
static unsigned Vita_BigTotal;

static void Vita_BigLockAcquire() { while (__atomic_exchange_n(&Vita_BigLock, 1, __ATOMIC_ACQUIRE)) sceKernelDelayThread(0); }
static void Vita_BigLockRelease() { __atomic_store_n(&Vita_BigLock, 0, __ATOMIC_RELEASE); }

// Index of the big block p belongs to, or -1. Caller holds the lock.
static int Vita_BigFind(void *p)
{
	for (int i = 0; i < Vita_BigCount; i++)
		if (Vita_BigBlocks[i].p == p) return i;
	return -1;
}

static void *Vita_BigAlloc(size_t size)
{
	if (size < (1u << 20)) return NULL;
	size_t rounded = (size + 0xFFF) & ~(size_t)0xFFF;
	SceUID uid = sceKernelAllocMemBlock("zandbig", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, rounded, NULL);
	if (uid < 0) return NULL;
	void *base = NULL;
	if (sceKernelGetMemBlockBase(uid, &base) < 0 || base == NULL) { sceKernelFreeMemBlock(uid); return NULL; }
	Vita_BigLockAcquire();
	if (Vita_BigCount >= (int)(sizeof(Vita_BigBlocks) / sizeof(Vita_BigBlocks[0])))
	{
		Vita_BigLockRelease();
		sceKernelFreeMemBlock(uid);
		return NULL;
	}
	Vita_BigBlocks[Vita_BigCount].p = (char *)base;
	Vita_BigBlocks[Vita_BigCount].size = size;
	Vita_BigBlocks[Vita_BigCount].uid = uid;
	Vita_BigCount++;
	Vita_BigTotal += rounded >> 10;
	Vita_BigLockRelease();
	return base;
}

// Frees p if it is a big block; returns its size (0 if not one).
static size_t Vita_BigFree(void *p)
{
	if (Vita_BigCount == 0 || p == NULL) return 0;
	Vita_BigLockAcquire();
	int i = Vita_BigFind(p);
	if (i < 0) { Vita_BigLockRelease(); return 0; }
	FVitaBigBlock b = Vita_BigBlocks[i];
	Vita_BigBlocks[i] = Vita_BigBlocks[--Vita_BigCount];
	Vita_BigTotal -= ((b.size + 0xFFF) & ~(size_t)0xFFF) >> 10;
	Vita_BigLockRelease();
	sceKernelFreeMemBlock(b.uid);
	return b.size;
}

static size_t Vita_BigSize(void *p)
{
	if (Vita_BigCount == 0 || p == NULL) return 0;
	Vita_BigLockAcquire();
	int i = Vita_BigFind(p);
	size_t sz = i >= 0 ? Vita_BigBlocks[i].size : 0;
	Vita_BigLockRelease();
	return sz;
}

unsigned Vita_BigBlocksKB() { return Vita_BigTotal; }

// [Vita] 0046: time spent in malloc/free/realloc by the thread whose actor
// update is being watched (slow-tick line), to tell whether its waits are on
// newlib's single malloc lock.
extern volatile SceUID Vita_TickWatchThread;
unsigned long long Vita_TickMallocUs;
unsigned Vita_TickMallocCalls;
struct FVitaMallocTimer
{
	unsigned long long t0;
	FVitaMallocTimer() : t0(Vita_TickWatchThread >= 0 && Vita_TickWatchThread == sceKernelGetThreadId() ? sceKernelGetProcessTimeWide() : 0) {}
	~FVitaMallocTimer() { if (t0) { Vita_TickMallocUs += sceKernelGetProcessTimeWide() - t0; Vita_TickMallocCalls++; } }
};

extern "C" void __wrap_free(void *p)
{
	if (p == NULL) return;
	FVitaMallocTimer vt;
	if (Vita_BigCount != 0 && Vita_BigFree(p) != 0) return;
	__real_free(p);
}

extern "C" size_t __wrap_malloc_usable_size(void *p)
{
	size_t sz = Vita_BigSize(p);
	return sz != 0 ? sz : __real_malloc_usable_size(p);
}

extern "C" void *__wrap_malloc(size_t size)
{
	FVitaMallocTimer vt;
	void *p = __real_malloc(size);
	if (p == NULL && size >= (1u << 20))
	{
		p = Vita_BigAlloc(size);
		if (p != NULL) return p;
	}
	if (p == NULL && size != 0)
		Vita_LogAllocFailure("malloc", size, __builtin_return_address(0));
	return p;
}

extern "C" void *__wrap_realloc(void *old, size_t size)
{
	FVitaMallocTimer vt;
	size_t oldbig = Vita_BigSize(old);
	if (oldbig != 0)
	{
		if (size == 0) { Vita_BigFree(old); return NULL; }
		void *n = __real_malloc(size);
		if (n == NULL) n = Vita_BigAlloc(size);
		if (n == NULL) { Vita_LogAllocFailure("realloc", size, __builtin_return_address(0)); return NULL; }
		memcpy(n, old, oldbig < size ? oldbig : size);
		Vita_BigFree(old);
		return n;
	}
	void *p = __real_realloc(old, size);
	if (p == NULL && size >= (1u << 20))
	{
		p = Vita_BigAlloc(size);
		if (p != NULL)
		{
			if (old != NULL)
			{
				size_t os = __real_malloc_usable_size(old);
				memcpy(p, old, os < size ? os : size);
				__real_free(old);
			}
			return p;
		}
	}
	if (p == NULL && size != 0)
		Vita_LogAllocFailure("realloc", size, __builtin_return_address(0));
	return p;
}

// operator new: log big requests that fail with *their* caller (the malloc
// wrapper only sees operator new itself), then behave as usual.
#include <new>
extern "C" void *__real__Znwj(size_t size);
extern "C" void *__real__Znaj(size_t size);
// [Vita] Called when a big allocation fails, to free caches and retry
// (set by the renderer: unloads models that weren't drawn recently).
// Only used on the main thread.
void (*Vita_MemoryPressureHook)(void);

static void *Vita_RetryAlloc(size_t size)
{
	if (size < (256u << 10) || Vita_MemoryPressureHook == NULL || sceKernelGetThreadId() != Vita_MainThreadId)
		return NULL;
	Vita_MemoryPressureHook();
	return malloc(size);
}

// For M_Malloc/M_Realloc: one more try after freeing caches, then log the
// engine-level caller.
void *Vita_RetryMRealloc(void *old, size_t size, void *caller)
{
	void *p = NULL;
	if (size >= (256u << 10) && Vita_MemoryPressureHook != NULL && sceKernelGetThreadId() == Vita_MainThreadId)
	{
		Vita_MemoryPressureHook();
		p = old ? realloc(old, size) : malloc(size);
	}
	if (p == NULL) Vita_LogAllocFailure(old ? "M_Realloc" : "M_Malloc", size, caller);
	return p;
}

extern "C" void *__wrap__Znwj(size_t size)
{
	void *p = malloc(size ? size : 1);
	if (p == NULL) p = Vita_RetryAlloc(size);
	if (p == NULL)
	{
		Vita_LogAllocFailure("new", size, __builtin_return_address(0));
		throw std::bad_alloc();
	}
	return p;
}
extern "C" void *__wrap__Znaj(size_t size)
{
	void *p = malloc(size ? size : 1);
	if (p == NULL) p = Vita_RetryAlloc(size);
	if (p == NULL)
	{
		Vita_LogAllocFailure("new[]", size, __builtin_return_address(0));
		throw std::bad_alloc();
	}
	return p;
}

// Log a startup stage and whether the malloc heap still looks sane.
extern "C" void Vita_Checkpoint(const char *stage)
{
	struct mallinfo mi = mallinfo();
	bool sane = mi.fordblks <= mi.arena && mi.uordblks <= mi.arena;
	// Only the engine's own progress messages go on screen; code-location
	// checkpoints and format strings stay in boot.log.
	if (strchr(stage, '(') == NULL && strchr(stage, '%') == NULL)
		Vita_LoadingStatus(stage);
	Vita_BootLog("[%s] heap arena %u KB, used %u KB, free %u KB%s", stage,
		(unsigned)mi.arena / 1024, (unsigned)mi.uordblks / 1024, (unsigned)mi.fordblks / 1024,
		sane ? "" : "  <-- HEAP CORRUPT");
}

// ---------------------------------------------------------------------------
// Networking: never block start-up on DNS.
//
// newlib's gethostbyname calls sceNetResolverStartNtoa with no timeout, which
// can wait a very long time on hardware (the engine resolves its own host
// name at start-up, and server addresses later). This wrapper (linked with
// --wrap=gethostbyname) answers numeric addresses and localhost directly,
// fails at once when Wi-Fi isn't connected, and otherwise resolves with a
// short timeout.
// ---------------------------------------------------------------------------
#include <netdb.h>
#include <arpa/inet.h>

extern "C" struct hostent *__real_gethostbyname(const char *name);

static bool Vita_NetConnected(void)
{
	int state = 0;
	return sceNetCtlInetGetState(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED;
}

extern "C" struct hostent *__wrap_gethostbyname(const char *name)
{
	static struct hostent ent;
	static char sname[256];
	static struct in_addr addr;
	static char *addrlist[2] = { (char *)&addr, NULL };

	if (name == NULL)
		return NULL;

	bool found = false;
	if (strcmp(name, "localhost") == 0)
	{
		addr.s_addr = htonl(0x7F000001);
		found = true;
	}
	else if (inet_aton(name, &addr))
	{
		found = true;
	}
	else if (!Vita_NetConnected())
	{
		Vita_BootLog("resolve %s: not connected", name);
		return NULL;
	}
	else
	{
		int rid = sceNetResolverCreate("zandronum", NULL, 0);
		if (rid >= 0)
		{
			SceNetInAddr sa;
			// Timeout is in microseconds.
			int err = sceNetResolverStartNtoa(rid, name, &sa, 3 * 1000 * 1000, 1, 0);
			sceNetResolverDestroy(rid);
			if (err >= 0)
			{
				addr.s_addr = sa.s_addr;
				found = true;
			}
			Vita_BootLog("resolve %s: 0x%08X", name, err);
		}
	}
	if (!found)
		return NULL;

	strncpy(sname, name, sizeof(sname) - 1);
	ent.h_name = sname;
	ent.h_aliases = NULL;
	ent.h_addrtype = AF_INET;
	ent.h_length = sizeof(struct in_addr);
	ent.h_addr_list = addrlist;
	return &ent;
}

// The console's IP address on the current network, or 127.0.0.1.
extern "C" void Vita_GetLocalIP(char *buf, int size)
{
	SceNetCtlInfo info;
	if (Vita_NetConnected() && sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0)
		snprintf(buf, size, "%s", info.ip_address);
	else
		snprintf(buf, size, "127.0.0.1");
}

// Self-test of the big-block path (vita_bigtest console command calls this).
int Vita_BigSelfTest()
{
	char *p = (char *)Vita_BigAlloc(3u << 20);
	if (p == NULL) return 1;
	memset(p, 0x5A, 3u << 20);
	if (malloc_usable_size(p) != (3u << 20)) return 2;
	char *q = (char *)realloc(p, 5u << 20);	// moves into the heap or another block
	if (q == NULL || q[0] != 0x5A || q[(3u << 20) - 1] != 0x5A) return 3;
	if (Vita_BigCount != 0 && Vita_BigSize(q) == 0 && Vita_BigFind(p) >= 0) return 4;
	free(q);
	char *r = (char *)Vita_BigAlloc(2u << 20);
	if (r == NULL) return 5;
	free(r);
	return Vita_BigCount == 0 ? 0 : 6;
}
