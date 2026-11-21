// PS Vita platform support for Zandronum.
#ifndef __VITA_SYSTEM_H__
#define __VITA_SYSTEM_H__

#ifdef __vita__

#ifdef __cplusplus
extern "C" {
#endif

// Appends one line to ux0:data/zandronum/boot.log. Written by a background
// thread within a few ms (directly during start-up); Vita_LogFlushNow
// writes out everything pending from the calling thread.
void Vita_BootLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Vita_LogFlushNow(void);

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

// Once per frame the game displays (frees the launcher's framebuffers).
void Vita_FrameShown(void);

// The console's IP address (dotted) on the current network, or 127.0.0.1.
void Vita_GetLocalIP(char *buf, int size);
void Vita_Checkpoint(const char *stage);

// Set by the OpenGL renderer once vitaGL owns the screen: draws one frame
// (clear + swap) so system dialogs such as the error box can show.
extern void (*Vita_GLDialogFrame)(void);

// Appends engine console output to ux0:data/zandronum/console.txt
// (truncated at start-up).
void Vita_ConsoleWrite(const char *text);

// Whether Sony's shader compiler (needed by vitaGL) is installed.
int Vita_HaveShaderCompiler(void);

#ifdef __cplusplus
}
#endif

#endif // __vita__
#endif
