// [Vita] Loading screen with progress while a level loads (the screen used
// to stay on the last frame for the whole load).

#include <psp2/kernel/processmgr.h>
#include "doomdef.h"
#include "doomstat.h"
#include "v_video.h"
#include "v_font.h"
#include "v_text.h"
#include "v_palette.h"
#include "g_level.h"
#include "vita/vita_system.h"

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

void Vita_LoadScreenEnd()
{
	Active = false;
}

bool Vita_LoadScreenActive()
{
	return Active;
}
