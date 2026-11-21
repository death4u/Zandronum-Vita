// [Vita] Loading screen with progress while a level loads (the screen used
// to stay on the last frame for the whole load).

#include <psp2/kernel/processmgr.h>
#include <malloc.h>
#include "doomdef.h"
#include "doomstat.h"
#include "v_video.h"
#include "v_font.h"
#include "v_text.h"
#include "v_palette.h"
#include "g_level.h"
#include "vita/vita_system.h"
#include "c_cvars.h"

static unsigned long long LastDraw;
static FString Title;
static bool Active;

// what: the step ("Map", "Models", "Textures"...); done/total: progress of
// that step (total 0: no bar). Draws at most 5 times a second unless force.
void Vita_LoadProgress(const char *what, int done, int total, bool force)
{
	if (!Active || screen == NULL || SmallFont == NULL) return;
	const unsigned long long now = sceKernelGetProcessTimeWide();
	if (!force && now - LastDraw < 200000) return;
	LastDraw = now;

	const int W = SCREENWIDTH, H = SCREENHEIGHT;
	screen->Lock(false);
	screen->SetBlendingRect(0, 0, 0, 0);
	screen->Begin2D(false);
	screen->Clear(0, 0, W, H, GPalette.BlackIndex, 0);
	const int scale = CleanXfac;
	FFont *big = BigFont != NULL ? BigFont : SmallFont;
	const int ty = H / 2 - big->GetHeight() * scale - 24;
	screen->DrawText(big, CR_GREEN, (W - big->StringWidth(Title) * scale) / 2, ty, Title,
		DTA_CleanNoMove, true, TAG_DONE);
	FString line;
	if (total > 0) line.Format("%s  %d / %d", what, done, total);
	else line = what;
	screen->DrawText(SmallFont, CR_GRAY, (W - SmallFont->StringWidth(line) * scale) / 2, H / 2 + 8, line,
		DTA_CleanNoMove, true, TAG_DONE);
	if (total > 0)
	{
		const int bw = W * 2 / 3, bh = 12, bx = (W - bw) / 2, by = H / 2 + 8 + SmallFont->GetHeight() * scale + 16;
		screen->Clear(bx, by, bx + bw, by + bh, -1, MAKEARGB(255, 48, 56, 40));
		const int fw = (int)((long long)bw * MIN(done, total) / total);
		if (fw > 0) screen->Clear(bx, by, bx + fw, by + bh, -1, MAKEARGB(255, 120, 200, 60));
	}
	screen->Update();
}

void Vita_LoadScreenBegin(const char *mapname)
{
	Active = true;
	Title.Format("Loading %s", mapname);
	Vita_LoadProgress("Map", 0, 0, true);
}

void Vita_WarmupBegin();
void Vita_LoadScreenEnd()
{
	Active = false;
	Vita_WarmupBegin();
}

bool Vita_LoadScreenActive()
{
	return Active;
}

// [Vita] 0046: level-start warm-up. The first seconds of a level (first
// updates of every actor, first draws of everything in view, background
// texture uploads) froze the game: Brutal Doom MAP01 had one 9.6 s frame
// and ~6 s at 0-8 fps after the loading screen. The game now runs normally
// under this screen until it has run 1 s of game time, the texture builder
// is idle and 8 frames in a row took under 200 ms, i.e. no more freezes
// (at most 10 s).
CVAR(Bool, vita_levelwarmup, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
static bool Warmup;
static unsigned long long WarmupStart, WarmupLastFrame;
static int WarmupGoodFrames;
static unsigned long long WarmupCalmSince;	// 0047: last frame over 200 ms
extern bool Vita_TexBuildIdle();

void Vita_WarmupBegin()
{
	if (!vita_levelwarmup || !stricmp(level.mapname, "TITLEMAP")) return;
	Warmup = true;
	WarmupStart = WarmupLastFrame = sceKernelGetProcessTimeWide();
	WarmupGoodFrames = 0;
	WarmupCalmSince = WarmupStart;
}

bool Vita_WarmupActive() { return Warmup; }

// Called just before the frame is shown.
void Vita_WarmupOverlay()
{
	if (!Warmup || screen == NULL || SmallFont == NULL) return;
	const unsigned long long now = sceKernelGetProcessTimeWide();
	const unsigned long long ft = now - WarmupLastFrame;
	WarmupLastFrame = now;
	if (ft < 200000) WarmupGoodFrames++; else { WarmupGoodFrames = 0; WarmupCalmSince = now; }
	// 0047: shown after 2 s in a row without a frame over 200 ms (0046's
	// 8 frames let HaloDoom through while it was still freezing), 30 s at most
	if (gamestate != GS_LEVEL || now - WarmupStart > 30000000ull ||
		(level.maptime >= TICRATE && now - WarmupCalmSince >= 2000000ull && Vita_TexBuildIdle()))
	{
		{
			extern unsigned Vita_BigBlocksKB();
			extern int Vita_ModelBytes();
			struct mallinfo mi = mallinfo();
			Vita_BootLog("level start: game shown after %llu ms of warm-up (%d game tics); heap in use %u MB, free in heap %u MB, big blocks %u MB, models %d MB",
				(now - WarmupStart) / 1000, level.maptime, (unsigned)(mi.uordblks >> 20), (unsigned)(mi.fordblks >> 20), Vita_BigBlocksKB() >> 10, Vita_ModelBytes() >> 20);
		}
		Warmup = false;
		return;
	}
	const int W = SCREENWIDTH, H = SCREENHEIGHT;
	screen->Clear(0, 0, W, H, GPalette.BlackIndex, 0);
	const int scale = CleanXfac;
	FFont *big = BigFont != NULL ? BigFont : SmallFont;
	FString title; title.Format("Loading %s", level.mapname);
	const int ty = H / 2 - big->GetHeight() * scale - 24;
	screen->DrawText(big, CR_GREEN, (W - big->StringWidth(title) * scale) / 2, ty, title, DTA_CleanNoMove, true, TAG_DONE);
	const char *line = "Getting ready";
	screen->DrawText(SmallFont, CR_GRAY, (W - SmallFont->StringWidth(line) * scale) / 2, H / 2 + 8, line, DTA_CleanNoMove, true, TAG_DONE);
}
