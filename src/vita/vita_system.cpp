// PS Vita platform support for Zandronum: process setup, boot logging and
// on-screen fatal errors.

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
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
// gives the app roughly 365 MB of main memory; the newlib heap takes 256 MB
// of it and vitaGL/SDL's GPU pools, the network pool and thread stacks the rest.
// Zandronum recurses deeply (BSP traversal, ACS, parsers), so the main
// thread gets a large stack instead of the 256 KB default.
// ---------------------------------------------------------------------------
extern "C" {
int _newlib_heap_size_user = 256 * 1024 * 1024;
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

extern "C" void Vita_BootLog(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int len = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	if (len < 0)
		return;
	if (len > (int)sizeof(buf) - 2)
		len = sizeof(buf) - 2;
	buf[len++] = '\n';

	SceUID fd = sceIoOpen(VITA_BOOT_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd >= 0)
	{
		sceIoWrite(fd, buf, len);
		sceIoClose(fd);
	}
}

static void Vita_LogExit(void)
{
	Vita_BootLog("exit() called");
}

static void *Vita_SqliteMalloc(int size) { return malloc(size); }
static void *Vita_SqliteRealloc(void *p, int size) { return realloc(p, size); }
static void Vita_SqliteFree(void *p) { free(p); }

#define VITA_NET_MEMORY (1 * 1024 * 1024)
static char vita_net_memory[VITA_NET_MEMORY];

extern "C" void Vita_EarlyInit(void)
{
	// The data folders must exist before anything is logged.
	Vita_MkDir("ux0:data");
	Vita_MkDir(VITA_DATA_ROOT);
	Vita_MkDir(VITA_DATA_ROOT "/saves");
	Vita_MkDir(VITA_DATA_ROOT "/screenshots");
	sceIoRemove(VITA_BOOT_LOG);

	Vita_BootLog("Zandronum for PS Vita, built " __DATE__ " " __TIME__);
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
	// Opened per write: Vita3K only commits a file's data when it is closed.
	static bool truncated = false;
	if (text == NULL || *text == 0) return;
	SceUID fd = sceIoOpen(VITA_DATA_ROOT "/console.txt",
		SCE_O_WRONLY | SCE_O_CREAT | (truncated ? SCE_O_APPEND : SCE_O_TRUNC), 0777);
	truncated = true;
	if (fd >= 0)
	{
		sceIoWrite(fd, text, strlen(text));
		sceIoClose(fd);
	}
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

static void Vita_LogAllocFailure(const char *what, size_t size, void *caller)
{
	struct mallinfo mi = mallinfo();
	Vita_BootLog("%s(%u) failed, caller %p; heap arena %u KB, in use %u KB, free %u KB",
		what, (unsigned)size, caller, (unsigned)mi.arena / 1024,
		(unsigned)mi.uordblks / 1024, (unsigned)mi.fordblks / 1024);
}

extern "C" void *__wrap_malloc(size_t size)
{
	void *p = __real_malloc(size);
	if (p == NULL && size != 0)
		Vita_LogAllocFailure("malloc", size, __builtin_return_address(0));
	return p;
}

extern "C" void *__wrap_realloc(void *old, size_t size)
{
	void *p = __real_realloc(old, size);
	if (p == NULL && size != 0)
		Vita_LogAllocFailure("realloc", size, __builtin_return_address(0));
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
