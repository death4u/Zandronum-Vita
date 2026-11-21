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

#include <zlib.h>
#include <set>

#include "vita_system.h"
#include "vita_font.inc"
#include "vita_logo.inc"

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
static const unsigned C_BG = RGB(14, 16, 12);
static const unsigned C_PANEL = RGB(26, 32, 22);
static const unsigned C_SEL = RGB(64, 96, 28);
static const unsigned C_TEXT = RGB(225, 220, 210);
static const unsigned C_DIM = RGB(140, 132, 124);
static const unsigned C_TITLE = RGB(170, 210, 80);
static const unsigned C_HEAD = RGB(140, 200, 70);
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

// Draws the logo centred horizontally with its top at y, blended over
// whatever is already there.
static void DrawLogo(int y)
{
	unsigned *fb = Fb[FbCur];
	const int x0 = (SCR_W - VITA_LOGO_W) / 2;
	for (int ly = 0; ly < VITA_LOGO_H; ++ly)
	{
		if (y + ly < 0 || y + ly >= SCR_H) continue;
		unsigned *p = fb + (y + ly) * SCR_W + x0;
		const unsigned char *s = VitaLogo + ly * VITA_LOGO_W * 4;
		for (int lx = 0; lx < VITA_LOGO_W; ++lx, s += 4)
		{
			unsigned a = s[3];
			if (a == 0) continue;
			unsigned d = p[lx];
			unsigned r = (s[0] * a + (d & 0xFF) * (255 - a)) / 255;
			unsigned g = (s[1] * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
			unsigned b = (s[2] * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
			p[lx] = 0xFF000000u | (b << 16) | (g << 8) | r;
		}
	}
}

static void TextCentered(int row, unsigned color, const char *s)
{
	int len = (int)strlen(s);
	Text((COLS - len) / 2, row, color, s);
}

// ---------------------------------------------------------------------------
// Loading screen: after the launcher, the engine's start-up steps are shown
// on the launcher framebuffer until SDL puts up the game's own display.
// ---------------------------------------------------------------------------

static bool LoadingActive = false;
static std::vector<std::string> LoadingLines;

static void DrawLoading()
{
	FillRect(0, 0, SCR_W, SCR_H, C_BG);
	FillRect(0, 0, SCR_W, 4 * CHAR_H, C_PANEL);
	DrawLogo((4 * CHAR_H - VITA_LOGO_H) / 2);
	TextCentered(4, C_TITLE, "LOADING");
	int first = (int)LoadingLines.size() - (ROWS - 6);
	if (first < 0) first = 0;
	for (int i = first, row = 6; i < (int)LoadingLines.size(); ++i, ++row)
		Text(1, row, i + 1 == (int)LoadingLines.size() ? C_TEXT : C_DIM, LoadingLines[i].c_str(), COLS - 2);
	FbFlip();
}

extern "C" void Vita_LoadingStatus(const char *line)
{
	if (!LoadingActive || line == NULL)
		return;
	std::string s = line;
	while (!s.empty() && (s[s.size() - 1] == '\n' || s[s.size() - 1] == ' ')) s.erase(s.size() - 1);
	if (s.empty())
		return;
	LoadingLines.push_back(s);
	DrawLoading();
}

extern "C" void Vita_LoadingDone(void)
{
	LoadingActive = false;
	LoadingLines.clear();

	// The game's renderer has its own display buffers now. Black out the
	// launcher's and detach them from the display from this (the main)
	// thread, so nothing the system restores after an overlay (PS button,
	// PS+Select screenshot) can bring back the old loading screen.
	if (FbBlock[0] >= 0 && FbBlock[1] >= 0)
	{
		for (int i = 0; i < 2; ++i)
			memset(Fb[i], 0, FB_SIZE);
		sceDisplaySetFrameBuf(NULL, SCE_DISPLAY_SETBUF_NEXTFRAME);
	}
}

// Called for every frame the game shows. The launcher framebuffers are kept
// (black): 0015 freed them here, and the system killed the game when it went
// back to them after PS / PS+Select.
extern "C" void Vita_FrameShown(void)
{
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

// The engine's own resource files ship inside the VPK (app0:). Copies of
// them in the data folder (e.g. from a PC install) must not be loaded again:
// the engine refuses files that override its core lumps.
static bool IsEngineFile(const std::string &name)
{
	static const char *const names[] = { "zandronum.pk3", "skulltag_actors.pk3", "brightmaps.pk3", "zdoom.pk3", "gzdoom.pk3" };
	std::string lname = Lower(name);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
		if (lname == names[i])
			return true;
	return false;
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
			if (depth > 0 && lname != "saves" && lname != "screenshots" && lname != "cache"
				&& lname != "texcache" && lname != "controls")	// the texture cache has hundreds of files
				subdirs.push_back(name);
			continue;
		}
		if (IsEngineFile(name))
		{
			Vita_BootLog("launcher: ignoring engine file %s", path.c_str());
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

// launcher.cfg format version (3: the built-in Skulltag actors are no longer
// a list entry; they load automatically when a mod needs them).
#define LAUNCHER_CFG_VERSION 3

static void LoadConfig(std::string &iwad, std::vector<std::string> &mods, int &version)
{
	version = 0;
	FILE *f = fopen(LAUNCHER_CFG, "r");
	if (f == NULL) return;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
		if (strncmp(line, "iwad=", 5) == 0) iwad = line + 5;
		else if (strncmp(line, "mod=", 4) == 0) mods.push_back(line + 4);
		else if (strncmp(line, "version=", 8) == 0) version = atoi(line + 8);
	}
	fclose(f);
}

static void SaveConfig(const std::string &iwad, const std::vector<std::string> &mods)
{
	FILE *f = fopen(LAUNCHER_CFG, "w");
	if (f == NULL) return;
	fprintf(f, "version=%d\n", LAUNCHER_CFG_VERSION);
	fprintf(f, "iwad=%s\n", iwad.c_str());
	for (size_t i = 0; i < mods.size(); ++i)
		fprintf(f, "mod=%s\n", mods[i].c_str());
	fclose(f);
}


// ---------------------------------------------------------------------------
// Does a mod need Skulltag's actors? Many Zandronum mods inherit from or
// spawn Skulltag's classes (RedArmor, DarkImp, BFG10K...), which PC users get
// by loading skulltag_actors.pk3 or skulltag_content. The launcher looks at
// the DECORATE code of the ticked mods and adds the built-in
// app0:/skulltag_actors.pk3 only when one of them names a Skulltag actor.
// ---------------------------------------------------------------------------

static unsigned Rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned Rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

// Calls fn(name, text) for each DECORATE text in a zip/pk3 (text files named
// decorate* or under actors/), or each DECORATE lump of a WAD.
template<class F> static void ForEachDecorate(const std::string &path, F fn)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == NULL) return;
	unsigned char hdr[12];
	if (fread(hdr, 1, 12, f) != 12) { fclose(f); return; }
	if (!memcmp(hdr, "PWAD", 4) || !memcmp(hdr, "IWAD", 4))
	{
		unsigned num = Rd32(hdr + 4), dir = Rd32(hdr + 8);
		std::vector<unsigned char> d(num * 16);
		if (num < 65536 && fseek(f, dir, SEEK_SET) == 0 && fread(d.data(), 16, num, f) == num)
		{
			for (unsigned i = 0; i < num; i++)
			{
				const unsigned char *e = &d[i * 16];
				char name[9] = {};
				memcpy(name, e + 8, 8);
				unsigned pos = Rd32(e), size = Rd32(e + 4);
				if (strncasecmp(name, "DECORATE", 8) || size == 0 || size > (4u << 20)) continue;
				std::string text(size, '\0');
				if (fseek(f, pos, SEEK_SET) == 0 && fread(&text[0], 1, size, f) == size) fn(name, text);
			}
		}
		fclose(f);
		return;
	}
	if (memcmp(hdr, "PK\3\4", 4)) { fclose(f); return; }
	// Find the end-of-central-directory record in the last 64 KB.
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	long tail = fsize < 65558 ? fsize : 65558;
	std::vector<unsigned char> t(tail);
	fseek(f, fsize - tail, SEEK_SET);
	if (fread(t.data(), 1, tail, f) != (size_t)tail) { fclose(f); return; }
	long eocd = -1;
	for (long i = tail - 22; i >= 0; i--)
		if (Rd32(&t[i]) == 0x06054b50) { eocd = i; break; }
	if (eocd < 0) { fclose(f); return; }
	unsigned entries = Rd16(&t[eocd + 10]), cdsize = Rd32(&t[eocd + 12]), cdoff = Rd32(&t[eocd + 16]);
	std::vector<unsigned char> cd(cdsize);
	if (fseek(f, cdoff, SEEK_SET) != 0 || fread(cd.data(), 1, cdsize, f) != cdsize) { fclose(f); return; }
	size_t o = 0;
	for (unsigned i = 0; i < entries && o + 46 <= cdsize; i++)
	{
		const unsigned char *e = &cd[o];
		if (Rd32(e) != 0x02014b50) break;
		unsigned method = Rd16(e + 10), csize = Rd32(e + 20), usize = Rd32(e + 24);
		unsigned nlen = Rd16(e + 28), xlen = Rd16(e + 30), clen = Rd16(e + 32), loff = Rd32(e + 42);
		std::string name((const char *)e + 46, nlen);
		o += 46 + nlen + xlen + clen;
		std::string lname = Lower(name);
		size_t slash = lname.find_last_of('/');
		std::string base = slash == std::string::npos ? lname : lname.substr(slash + 1);
		bool want = lname.find("decorate") != std::string::npos || lname.compare(0, 7, "actors/") == 0;
		std::string ext = Extension(lname);
		if (!want || usize == 0 || usize > (4u << 20) || ext == "png" || ext == "jpg" || ext == "md3" || ext == "wav" || ext == "ogg")
			continue;
		unsigned char lh[30];
		if (fseek(f, loff, SEEK_SET) != 0 || fread(lh, 1, 30, f) != 30 || Rd32(lh) != 0x04034b50) continue;
		fseek(f, loff + 30 + Rd16(lh + 26) + Rd16(lh + 28), SEEK_SET);
		std::vector<unsigned char> comp(csize);
		if (fread(comp.data(), 1, csize, f) != csize) continue;
		std::string text(usize, '\0');
		if (method == 0 && csize == usize)
			memcpy(&text[0], comp.data(), usize);
		else if (method == 8)
		{
			z_stream zs = {};
			if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) continue;
			zs.next_in = comp.data(); zs.avail_in = csize;
			zs.next_out = (Bytef *)&text[0]; zs.avail_out = usize;
			int r = inflate(&zs, Z_FINISH);
			inflateEnd(&zs);
			if (r != Z_STREAM_END) continue;
		}
		else continue;
		fn(name, text);
	}
	fclose(f);
}

// Does the zip/pk3 contain a file with this name (lower case, full path)?
static bool ZipHasFile(const std::string &path, const char *want)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == NULL) return false;
	bool found = false;
	unsigned char hdr[4];
	if (fread(hdr, 1, 4, f) == 4 && !memcmp(hdr, "PK\3\4", 4))
	{
		fseek(f, 0, SEEK_END);
		long fsize = ftell(f);
		long tail = fsize < 65558 ? fsize : 65558;
		std::vector<unsigned char> t(tail);
		fseek(f, fsize - tail, SEEK_SET);
		long eocd = -1;
		if (fread(t.data(), 1, tail, f) == (size_t)tail)
			for (long i = tail - 22; i >= 0; i--)
				if (Rd32(&t[i]) == 0x06054b50) { eocd = i; break; }
		if (eocd >= 0)
		{
			unsigned entries = Rd16(&t[eocd + 10]), cdsize = Rd32(&t[eocd + 12]), cdoff = Rd32(&t[eocd + 16]);
			std::vector<unsigned char> cd(cdsize);
			if (fseek(f, cdoff, SEEK_SET) == 0 && fread(cd.data(), 1, cdsize, f) == cdsize)
			{
				size_t o = 0;
				for (unsigned i = 0; i < entries && o + 46 <= cdsize && !found; i++)
				{
					const unsigned char *e = &cd[o];
					if (Rd32(e) != 0x02014b50) break;
					unsigned nlen = Rd16(e + 28), xlen = Rd16(e + 30), clen = Rd16(e + 32);
					if (o + 46 + nlen > cdsize) break;
					found = Lower(std::string((const char *)e + 46, nlen)) == want;
					o += 46 + nlen + xlen + clen;
				}
			}
		}
	}
	fclose(f);
	return found;
}

// Actor names in DECORATE text. parentsonly = false: the names defined by
// "actor <name> ..." headers. parentsonly = true: the classes a mod builds
// on (": <parent>", "replaces <x>"), plus every "quoted" word if wantquoted.
static void CollectNames(const std::string &text, std::set<std::string> &out, bool parentsonly, bool wantquoted)
{
	size_t i = 0, n = text.size();
	while (i < n)
	{
		size_t eol = text.find('\n', i);
		if (eol == std::string::npos) eol = n;
		std::string line = text.substr(i, eol - i);
		i = eol + 1;
		if (wantquoted)
		{
			for (size_t q = line.find('"'); q != std::string::npos; )
			{
				size_t e = line.find('"', q + 1);
				if (e == std::string::npos) break;
				out.insert(Lower(line.substr(q + 1, e - q - 1)));
				q = line.find('"', e + 1);
			}
		}
		// tokenize the header
		std::vector<std::string> tok;
		std::string cur;
		for (size_t k = 0; k <= line.size(); k++)
		{
			char c = k < line.size() ? line[k] : ' ';
			if (c == '/' && k + 1 < line.size() && line[k + 1] == '/') c = ' ', k = line.size();
			if (isspace((unsigned char)c) || c == ':' || c == '{')
			{
				if (!cur.empty()) tok.push_back(Lower(cur));
				cur.clear();
				if (c == ':') tok.push_back(":");
				if (c == '{') break;
			}
			else cur += c;
		}
		if (tok.size() < 2 || tok[0] != "actor") continue;
		if (!parentsonly)
		{	// definitions: only the defined names
			out.insert(tok[1]);
			continue;
		}
		for (size_t k = 2; k + 1 < tok.size(); k++)
			if (tok[k] == ":" || tok[k] == "replaces") out.insert(tok[k + 1]);
	}
}

static bool ModsNeedSkulltagActors(const std::vector<std::string> &mods)
{
	// Skulltag-only classes: defined in skulltag_actors.pk3 but not among the
	// engine's own (it also redefines some, e.g. BulletPuff).
	std::set<std::string> stnames, engine;
	ForEachDecorate("app0:/skulltag_actors.pk3", [&](const std::string &, const std::string &text) {
		CollectNames(text, stnames, false, false);
	});
	ForEachDecorate("app0:/zandronum.pk3", [&](const std::string &, const std::string &text) {
		CollectNames(text, engine, false, false);
	});
	for (std::set<std::string>::iterator it = engine.begin(); it != engine.end(); ++it)
		stnames.erase(*it);
	if (stnames.empty()) return false;
	for (size_t m = 0; m < mods.size(); m++)
	{
		std::set<std::string> refs;
		ForEachDecorate(mods[m], [&](const std::string &, const std::string &text) {
			CollectNames(text, refs, true, true);
		});
		for (std::set<std::string>::iterator it = refs.begin(); it != refs.end(); ++it)
		{
			if (stnames.count(*it))
			{
				Vita_BootLog("launcher: %s uses Skulltag actor '%s', loading skulltag_actors.pk3", mods[m].c_str(), it->c_str());
				return true;
			}
		}
	}
	return false;
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
	// Skulltag's own resources (skulltag_content/_data and the like) go
	// first, as PC launchers do, since mods build on them. skulltag_content
	// already contains Skulltag's actors; otherwise the built-in
	// skulltag_actors.pk3 is added when a ticked mod uses Skulltag actors.
	bool havecontent = false;
	for (size_t i = 0; i < mods.size(); ++i)
	{
		size_t slash = mods[i].find_last_of("/:");
		std::string base = Lower(slash == std::string::npos ? mods[i] : mods[i].substr(slash + 1));
		if (base.compare(0, 16, "skulltag_content") == 0)
			havecontent = true;
	}
	if (!havecontent && !mods.empty() && ModsNeedSkulltagActors(mods))
		files.push_back("app0:/skulltag_actors.pk3");
	for (int pass = 0; pass < 2; ++pass)
	{
		for (size_t i = 0; i < mods.size(); ++i)
		{
			size_t slash = mods[i].find_last_of("/:");
			std::string base = Lower(slash == std::string::npos ? mods[i] : mods[i].substr(slash + 1));
			bool st = base.compare(0, 9, "skulltag_") == 0;
			if (st != (pass == 0))
				continue;
			std::string ext = Extension(mods[i]);
			(ext == "deh" || ext == "bex" ? dehs : files).push_back(mods[i]);
		}
	}
	// Vita-specific fixes for HaloDoom (baseline copies of textures that are
	// too big to decode on the Vita, see jpegtexture.cpp): only with HaloDoom.
	{
		bool halo = false;
		for (size_t i = 0; i < files.size() && !halo; i++)
			halo = ZipHasFile(files[i], "acs/halodoomgamemodes.o");
		FILE *t = halo ? fopen("app0:/vita-halodoom.pk3", "rb") : NULL;
		if (t != NULL) { fclose(t); files.push_back("app0:/vita-halodoom.pk3"); }
		Vita_BootLog("launcher: HaloDoom %s", halo ? "found: loading vita-halodoom.pk3" : "not loaded");
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
	int cfgversion;
	LoadConfig(lastiwad, lastmods, cfgversion);
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
	const int listtop = 6, listrows = ROWS - listtop - 4;

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
		FillRect(0, 0, SCR_W, 4 * CHAR_H, C_PANEL);
		DrawLogo((4 * CHAR_H - VITA_LOGO_H) / 2);
		Text(1, 4, C_DIM, "Files from ux0:data/zandronum/ and its mods/ folder");

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

	if (!quit)
	{
		LoadingActive = true;
		LoadingLines.push_back("Starting...");
		DrawLoading();
	}
	else
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
