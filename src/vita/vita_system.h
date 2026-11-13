// PS Vita platform support for Zandronum.
#ifndef __VITA_SYSTEM_H__
#define __VITA_SYSTEM_H__

#ifdef __vita__

#ifdef __cplusplus
extern "C" {
#endif

// Appends one line to ux0:data/zandronum/boot.log (opened and closed each
// time, so it survives a crash or a hard exit).
void Vita_BootLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Clocks, data folders, stdout/stderr redirection, system modules, network.
void Vita_EarlyInit(void);

// Shows a fatal error on screen and waits for the user to close it.
void Vita_FatalError(const char *text);

// Start-up launcher: lets the user pick the IWAD and mods, then replaces
// argv with the original arguments plus -iwad/-file/-deh (and args.txt).
void Vita_RunLauncher(int *argc, char ***argv);

// Start-up progress shown on the launcher's screen until the game's video
// mode is set (then Vita_LoadingDone stops it).
void Vita_LoadingStatus(const char *line);
void Vita_LoadingDone(void);

// The console's IP address (dotted) on the current network, or 127.0.0.1.
void Vita_GetLocalIP(char *buf, int size);
void Vita_Checkpoint(const char *stage);

#ifdef __cplusplus
}
#endif

#endif // __vita__
#endif
