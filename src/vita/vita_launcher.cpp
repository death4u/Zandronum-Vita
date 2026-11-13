// PS Vita start-up launcher: pick the game (IWAD) and any mods (PWAD, pk3,
// pk7, zip, deh/bex) before the engine starts, then hand them to Zandronum
// as -iwad / -file / -deh arguments.
//
// It draws straight to a display framebuffer and reads the pad with sceCtrl,
// so it runs before SDL and the engine exist. The last choice is remembered
// in ux0:data/zandronum/launcher.cfg.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <algorithm>

#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include "vita_system.h"
#include "vita_font.inc"

#define DATA_DIR "ux0:data/zandronum"
#define LAUNCHER_CFG DATA_DIR "/launcher.cfg"
#define ARGS_TXT DATA_DIR "/args.txt"

// ---------------------------------------------------------------------------
// Drawing: two 960x544 RGBA framebuffers in CDRAM, 8x8 font drawn at 2x.
// ---------------------------------------------------------------------------

enum { SCR_W = 960, SCR_H = 544, FB_SIZE = 2 * 1024 * 1024, CHAR_W = 16, CHAR_H = 20 };	// 16x16 glyphs, 4 px leading
enum { COLS = SCR_W / CHAR_W, ROWS = SCR_H / CHAR_H };	// 60 x 27

// Colours are ABGR in memory.
static inline unsigned RGB(unsigned r, unsigned g, unsigned b) { return 0xFF000000u | (b << 16) | (g << 8) | r; }
static const unsigned C_BG = RGB(16, 12, 12);
static const unsigned C_PANEL = RGB(40, 24, 22);
static const unsigned C_SEL = RGB(140, 30, 24);
static const unsigned C_TEXT = RGB(225, 220, 210);
static const unsigned C_DIM = RGB(140, 132, 124);
static const unsigned C_TITLE = RGB(240, 190, 80);
static const unsigned C_HEAD = RGB(230, 120, 70);
static const unsigned C_OK = RGB(120, 220, 120);

static SceUID FbBlock[2] = { -1, -1 };
static unsigned *Fb[2];
static int FbCur;

static bool FbInit()
{
	for (int i = 0; i < 2; ++i)
	{
		FbBlock[i] = sceKernelAllocMemBlock("launcher_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_SIZE, NULL);
		if (FbBlock[i] < 0)
			return false;
		sceKernelGetMemBlockBase(FbBlock[i], (void **)&Fb[i]);
	}
	FbCur = 0;
	return true;
}


static void FbFlip()
{
	SceDisplayFrameBuf frame;
	memset(&frame, 0, sizeof(frame));
	frame.size = sizeof(frame);
	frame.base = Fb[FbCur];
	frame.pitch = SCR_W;
	frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	frame.width = SCR_W;
	frame.height = SCR_H;
	sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
	sceDisplayWaitVblankStart();
	FbCur ^= 1;
}

static void FillRect(int x, int y, int w, int h, unsigned color)
{
	unsigned *fb = Fb[FbCur];
	for (int yy = y; yy < y + h && yy < SCR_H; ++yy)
	{
		if (yy < 0) continue;
		unsigned *p = fb + yy * SCR_W;
		for (int xx = x < 0 ? 0 : x; xx < x + w && xx < SCR_W; ++xx)
			p[xx] = color;
	}
}

// Leaves a black frame on screen. The two framebuffers (4 MB of CDRAM) stay
// allocated: the display keeps scanning the last one until SDL sets up its
// own, and freeing memory the display is reading is not safe.
static void FbShutdown()
{
	if (FbBlock[0] < 0 || FbBlock[1] < 0)
		return;
	FillRect(0, 0, SCR_W, SCR_H, 0xFF000000u);
	FbFlip();
	FillRect(0, 0, SCR_W, SCR_H, 0xFF000000u);
	FbFlip();
}

// Draws text at a character cell; returns the column after it.
static int Text(int col, int row, unsigned color, const char *s, int maxcols = COLS)
{
	unsigned *fb = Fb[FbCur];
	int n = 0;
	for (; *s && n < maxcols && col + n < COLS; ++s, ++n)
	{
		const unsigned char *g = &VitaFont8x8[(unsigned char)*s * 8];
		int px = (col + n) * CHAR_W, py = row * CHAR_H + 2;
		for (int gy = 0; gy < 8; ++gy)
		{
			unsigned char bits = g[gy];
			for (int gx = 0; gx < 8; ++gx)
			{
				if (bits & (0x80 >> gx))
				{
					unsigned *p = fb + (py + gy * 2) * SCR_W + px + gx * 2;
					p[0] = p[1] = p[SCR_W] = p[SCR_W + 1] = color;
				}
			}
		}
	}
	return col + n;
}

static void TextCentered(int row, unsigned color, const char *s)
{
	int len = (int)strlen(s);
	Text((COLS - len) / 2, row, color, s);
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

struct Entry
{
	std::string Path;	// full path
	std::string Name;	// shown (relative to the data folder)
	bool IsIWAD;
};

static std::string Lower(std::string s)
{
	for (size_t i = 0; i < s.size(); ++i) s[i] = (char)tolower((unsigned char)s[i]);
	return s;
}

static std::string Extension(const std::string &name)
{
	size_t dot = name.rfind('.');
	return dot == std::string::npos ? "" : Lower(name.substr(dot + 1));
}

static bool ReadMagic(const std::string &path, char magic[4])
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == NULL) return false;
	bool ok = fread(magic, 1, 4, f) == 4;
	fclose(f);
	return ok;
}

static void ScanDir(const std::string &dir, const std::string &prefix, std::vector<Entry> &out, int depth)
{
	DIR *d = opendir(dir.c_str());
	if (d == NULL)
		return;
	struct dirent *de;
	std::vector<std::string> subdirs;
	while ((de = readdir(d)) != NULL)
	{
		std::string name = de->d_name;
		if (name == "." || name == ".." || name.empty() || name[0] == '.')
			continue;
		std::string path = dir + "/" + name;
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			std::string lname = Lower(name);
			if (depth > 0 && lname != "saves" && lname != "screenshots" && lname != "cache")
				subdirs.push_back(name);
			continue;
		}
		std::string ext = Extension(name);
		bool iwad;
		if (ext == "iwad" || ext == "ipk3" || ext == "ipk7")
			iwad = true;
		else if (ext == "wad")
		{
			char magic[4];
			if (!ReadMagic(path, magic)) continue;
			if (memcmp(magic, "IWAD", 4) == 0) iwad = true;
			else if (memcmp(magic, "PWAD", 4) == 0) iwad = false;
			else continue;
		}
		else if (ext == "pk3" || ext == "pk7" || ext == "zip" || ext == "7z" || ext == "deh" || ext == "bex")
			iwad = false;
		else
			continue;
		Entry e;
		e.Path = path;
		e.Name = prefix + name;
		e.IsIWAD = iwad;
		out.push_back(e);
	}
	closedir(d);
	for (size_t i = 0; i < subdirs.size(); ++i)
		ScanDir(dir + "/" + subdirs[i], prefix + subdirs[i] + "/", out, depth - 1);
}

// Case-insensitive "natural" order, so map2 sorts before map10.
static bool NameLess(const Entry &ea, const Entry &eb)
{
	const char *a = ea.Name.c_str(), *b = eb.Name.c_str();
	while (*a && *b)
	{
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b))
		{
			char *ea2, *eb2;
			unsigned long na = strtoul(a, &ea2, 10), nb = strtoul(b, &eb2, 10);
			if (na != nb) return na < nb;
			a = ea2; b = eb2;
			continue;
		}
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb) return ca < cb;
		a++; b++;
	}
	return *a == 0 && *b != 0;
}

// ---------------------------------------------------------------------------
// Remembered choice
// ---------------------------------------------------------------------------

static void LoadConfig(std::string &iwad, std::vector<std::string> &mods)
{
	FILE *f = fopen(LAUNCHER_CFG, "r");
	if (f == NULL) return;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
		if (strncmp(line, "iwad=", 5) == 0) iwad = line + 5;
		else if (strncmp(line, "mod=", 4) == 0) mods.push_back(line + 4);
	}
	fclose(f);
}

static void SaveConfig(const std::string &iwad, const std::vector<std::string> &mods)
{
	FILE *f = fopen(LAUNCHER_CFG, "w");
	if (f == NULL) return;
	fprintf(f, "iwad=%s\n", iwad.c_str());
	for (size_t i = 0; i < mods.size(); ++i)
		fprintf(f, "mod=%s\n", mods[i].c_str());
	fclose(f);
}

// ---------------------------------------------------------------------------
// The launcher
// ---------------------------------------------------------------------------

// Builds the new argv: the original arguments, then -iwad, -file, -deh and
// whatever args.txt adds.
static void BuildArgs(int *argc, char ***argv, const std::string &iwad, const std::vector<std::string> &mods)
{
	std::vector<std::string> args;
	for (int i = 0; i < *argc; ++i)
		args.push_back((*argv)[i]);
	if (!iwad.empty())
	{
		args.push_back("-iwad");
		args.push_back(iwad);
	}
	std::vector<std::string> files, dehs;
	for (size_t i = 0; i < mods.size(); ++i)
	{
		std::string ext = Extension(mods[i]);
		(ext == "deh" || ext == "bex" ? dehs : files).push_back(mods[i]);
	}
	if (!files.empty())
	{
		args.push_back("-file");
		args.insert(args.end(), files.begin(), files.end());
	}
	for (size_t i = 0; i < dehs.size(); ++i)
	{
		args.push_back("-deh");
		args.push_back(dehs[i]);
	}

	// Extra parameters for advanced use (one or more per line, # = comment).
	FILE *f = fopen(ARGS_TXT, "r");
	if (f != NULL)
	{
		char line[1024];
		while (fgets(line, sizeof(line), f))
		{
			char *p = line;
			while (*p)
			{
				while (*p && isspace((unsigned char)*p)) p++;
				if (*p == 0 || *p == '#') break;
				std::string tok;
				if (*p == '"')
				{
					p++;
					while (*p && *p != '"') tok += *p++;
					if (*p == '"') p++;
				}
				else
				{
					while (*p && !isspace((unsigned char)*p)) tok += *p++;
				}
				args.push_back(tok);
			}
		}
		fclose(f);
	}

	char **nv = (char **)malloc(sizeof(char *) * (args.size() + 1));
	for (size_t i = 0; i < args.size(); ++i)
	{
		nv[i] = strdup(args[i].c_str());
		Vita_BootLog("arg %u: %s", (unsigned)i, nv[i]);
	}
	nv[args.size()] = NULL;
	*argc = (int)args.size();
	*argv = nv;
}

extern "C" void Vita_RunLauncher(int *argc, char ***argv)
{
	// Make sure the mods folder exists so users know where to put things.
	mkdir(DATA_DIR "/mods", 0777);

	std::vector<Entry> files;
	ScanDir(DATA_DIR, "", files, 2);
	std::vector<Entry> iwads, mods;
	for (size_t i = 0; i < files.size(); ++i)
		(files[i].IsIWAD ? iwads : mods).push_back(files[i]);
	std::sort(iwads.begin(), iwads.end(), NameLess);
	std::sort(mods.begin(), mods.end(), NameLess);
	Vita_BootLog("launcher: %u IWADs, %u mods", (unsigned)iwads.size(), (unsigned)mods.size());

	// Restore the last choice where the files still exist.
	std::string lastiwad;
	std::vector<std::string> lastmods;
	LoadConfig(lastiwad, lastmods);
	int iwadsel = iwads.empty() ? -1 : 0;
	for (size_t i = 0; i < iwads.size(); ++i)
		if (iwads[i].Path == lastiwad) iwadsel = (int)i;
	std::vector<int> order;	// indices into mods, in load order
	for (size_t j = 0; j < lastmods.size(); ++j)
		for (size_t i = 0; i < mods.size(); ++i)
			if (mods[i].Path == lastmods[j]) order.push_back((int)i);

	if (!FbInit())
	{
		Vita_BootLog("launcher: no framebuffer, starting with the saved choice");
		FbShutdown();
		std::vector<std::string> modpaths;
		for (size_t i = 0; i < order.size(); ++i) modpaths.push_back(mods[order[i]].Path);
		BuildArgs(argc, argv, iwadsel >= 0 ? iwads[iwadsel].Path : "", modpaths);
		return;
	}

	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

	if (iwads.empty())
	{	// Nothing to play: explain and exit on any button.
		unsigned old = ~0u;
		for (;;)
		{
			FillRect(0, 0, SCR_W, SCR_H, C_BG);
			TextCentered(6, C_TITLE, "ZANDRONUM");
			TextCentered(10, C_TEXT, "No game found.");
			TextCentered(12, C_TEXT, "Copy doom2.wad, doom.wad, freedoom2.wad");
			TextCentered(13, C_TEXT, "(or another IWAD) into ux0:data/zandronum/");
			TextCentered(14, C_TEXT, "and start Zandronum again.");
			TextCentered(17, C_DIM, "Mods go in ux0:data/zandronum/mods/");
			TextCentered(22, C_DIM, "Press any button to exit");
			FbFlip();
			SceCtrlData pad;
			sceCtrlPeekBufferPositive(0, &pad, 1);
			if (old != ~0u && (pad.buttons & ~old)) break;
			old = pad.buttons;
		}
		FbShutdown();
		sceKernelExitProcess(0);
	}

	// Item list: 0 = Start, then IWADs, then mods.
	const int numitems = 1 + (int)iwads.size() + (int)mods.size();
	int cursor = 0;
	int scroll = 0;
	unsigned oldbuttons = ~0u;	// ignore buttons held at boot
	int repeatdelay = 0;
	bool launch = false, quit = false;

	// Layout
	const int listtop = 5, listrows = ROWS - listtop - 4;

	while (!launch && !quit)
	{
		// ---- input ----
		SceCtrlData pad;
		memset(&pad, 0, sizeof(pad));
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned buttons = pad.buttons;
		if (pad.ly < 40) buttons |= SCE_CTRL_UP;
		if (pad.ly > 215) buttons |= SCE_CTRL_DOWN;
		unsigned pressed = buttons & ~oldbuttons;
		if (oldbuttons == ~0u) pressed = 0;
		if (buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN))
		{
			if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN))
				repeatdelay = 20;
			else if (--repeatdelay <= 0)
			{
				pressed |= buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN);
				repeatdelay = 4;
			}
		}
		oldbuttons = buttons;

		if (pressed & SCE_CTRL_UP) cursor = cursor > 0 ? cursor - 1 : numitems - 1;
		if (pressed & SCE_CTRL_DOWN) cursor = cursor < numitems - 1 ? cursor + 1 : 0;
		if (pressed & SCE_CTRL_LTRIGGER) cursor = std::max(0, cursor - listrows);
		if (pressed & SCE_CTRL_RTRIGGER) cursor = std::min(numitems - 1, cursor + listrows);
		if (pressed & SCE_CTRL_START)
			launch = true;
		if (pressed & SCE_CTRL_SQUARE)
			order.clear();
		if (pressed & SCE_CTRL_CROSS)
		{
			if (cursor == 0)
				launch = true;
			else if (cursor <= (int)iwads.size())
				iwadsel = cursor - 1;
			else
			{
				int m = cursor - 1 - (int)iwads.size();
				std::vector<int>::iterator it = std::find(order.begin(), order.end(), m);
				if (it != order.end()) order.erase(it);
				else order.push_back(m);
			}
		}
		if (pressed & SCE_CTRL_SELECT)
			quit = true;
		if (launch && iwadsel < 0)
			launch = false;	// nothing to play

		// ---- keep the cursor visible (rows: items plus two headings) ----
		// Row index of an item in the scrolling list.
		int itemrow = cursor == 0 ? 0 : cursor <= (int)iwads.size() ? cursor + 1 : cursor + 2;
		if (itemrow < scroll) scroll = itemrow;
		if (itemrow >= scroll + listrows) scroll = itemrow - listrows + 1;
		if (cursor == 1 || cursor == 0) scroll = 0;

		// ---- draw ----
		FillRect(0, 0, SCR_W, SCR_H, C_BG);
		FillRect(0, 0, SCR_W, 3 * CHAR_H, C_PANEL);
		TextCentered(1, C_TITLE, "ZANDRONUM");
		Text(1, 3, C_DIM, "Files from ux0:data/zandronum/ and its mods/ folder");

		int row = 0;	// logical row in the scrolling list
		for (int item = 0; item < numitems; ++item)
		{
			// Headings before the IWAD and mod sections.
			if (item == 1 || item == 1 + (int)iwads.size())
			{
				int r = row - scroll;
				bool modhead = item == 1 + (int)iwads.size();
				if (r >= 0 && r < listrows)
					Text(1, listtop + r, C_HEAD, modhead ? "MODS (load order)" : "GAME");
				row++;
			}
			int r = row - scroll;
			row++;
			if (r < 0 || r >= listrows)
				continue;
			int y = listtop + r;
			if (item == cursor)
				FillRect(0, y * CHAR_H, SCR_W, CHAR_H, C_SEL);

			char buf[128];
			if (item == 0)
			{
				Text(2, y, iwadsel >= 0 ? C_OK : C_DIM, iwadsel >= 0 ? "> START GAME" : "> START GAME (no game found)");
			}
			else if (item <= (int)iwads.size())
			{
				int i = item - 1;
				snprintf(buf, sizeof(buf), "(%c) ", i == iwadsel ? '*' : ' ');
				int c = Text(3, y, i == iwadsel ? C_OK : C_TEXT, buf);
				Text(c, y, i == iwadsel ? C_OK : C_TEXT, iwads[i].Name.c_str(), COLS - c - 1);
			}
			else
			{
				int m = item - 1 - (int)iwads.size();
				std::vector<int>::iterator it = std::find(order.begin(), order.end(), m);
				if (it != order.end())
					snprintf(buf, sizeof(buf), "[%2d] ", int(it - order.begin()) + 1);
				else
					snprintf(buf, sizeof(buf), "[  ] ");
				unsigned col = it != order.end() ? C_OK : C_TEXT;
				int c = Text(3, y, col, buf);
				Text(c, y, col, mods[m].Name.c_str(), COLS - c - 1);
			}
		}
		if (mods.empty())
		{
			int r = 1 + (int)iwads.size() + 1 - scroll;
			if (r >= 0 && r < listrows)
				Text(1, listtop + r, C_HEAD, "MODS");
			if (r + 1 >= 0 && r + 1 < listrows)
				Text(3, listtop + r + 1, C_DIM, "none - put .wad/.pk3 files in ux0:data/zandronum/mods/");
		}

		FillRect(0, (ROWS - 3) * CHAR_H, SCR_W, 3 * CHAR_H, C_PANEL);
		Text(1, ROWS - 3, C_TEXT, "X select/toggle (mods load in that order)   START play");
		Text(1, ROWS - 2, C_DIM, "SQUARE clear mods   L/R page   SELECT quit");
		FbFlip();
	}

	FbShutdown();

	if (quit)
	{
		Vita_BootLog("launcher: quit");
		sceKernelExitProcess(0);
	}

	std::vector<std::string> modpaths;
	for (size_t i = 0; i < order.size(); ++i)
		modpaths.push_back(mods[order[i]].Path);
	std::string iwad = iwadsel >= 0 ? iwads[iwadsel].Path : "";
	SaveConfig(iwad, modpaths);
	BuildArgs(argc, argv, iwad, modpaths);
}
