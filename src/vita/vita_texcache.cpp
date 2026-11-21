// [Vita] Texture cache: compressed (DXT) textures on the memory card.
//
// Decoding a 1024x1024 PNG/JPEG on the Vita takes 0.4-0.9 s and compressing it
// to DXT another 0.5 s; HaloDoom has hundreds of such textures, so every new
// texture in sight stalled the game and level precaching took minutes. Here
// the engine compresses the texture itself (all mip levels), uploads it with
// glCompressedTexImage2D, and keeps the compressed levels in
// ux0:data/zandronum/texcache/<key>.vtc. The next time the same texture is
// needed (next level, next session), it is read from there and uploaded
// directly: no decoding, no compression.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

#include "vita_texcache.h"
#include "c_cvars.h"
#include "w_wad.h"
#include "textures/textures.h"
#include "vita/vita_system.h"
#include "tarray.h"

CVAR(Bool, vita_texcache, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vita_texcachemips, 1, 0)
CVAR(Bool, vita_texdebug, false, 0)	// test switch: 0 = level 0 only

#define TEXCACHE_DIR "ux0:data/zandronum/texcache"

// [Vita] Cache files are read and written with sceIo directly, not stdio:
// three threads (game, cache writer, texture builder) open files at the
// same time, and 0037 crashed in newlib's lseek on a FILE whose descriptor
// had gone.
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
struct VFile { SceUID fd; };
static VFile *vfopen(const char *path, const char *mode)
{
	int flags = mode[0] == 'r' ? SCE_O_RDONLY : mode[0] == 'a' ? (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND) : (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC);
	SceUID fd = sceIoOpen(path, flags, 0777);
	if (fd < 0) return NULL;
	VFile *f = new VFile;
	f->fd = fd;
	return f;
}
static size_t vfread(void *p, size_t size, size_t n, VFile *f)
{
	const size_t want = size * n;
	size_t got = 0;
	while (got < want)
	{
		int r = sceIoRead(f->fd, (char *)p + got, want - got);
		if (r <= 0) break;
		got += r;
	}
	return size ? got / size : 0;
}
static size_t vfwrite(const void *p, size_t size, size_t n, VFile *f)
{
	const size_t want = size * n;
	size_t put = 0;
	while (put < want)
	{
		int r = sceIoWrite(f->fd, (const char *)p + put, want - put);
		if (r <= 0) break;
		put += r;
	}
	return size ? put / size : 0;
}
static int vfclose(VFile *f) { int r = sceIoClose(f->fd); delete f; return r < 0 ? -1 : 0; }
static int vremove(const char *p) { return sceIoRemove(p) < 0 ? -1 : 0; }
static int vrename(const char *a, const char *b) { return sceIoRename(a, b) < 0 ? -1 : 0; }
static int vmkdir(const char *p, int mode) { return sceIoMkdir(p, mode) < 0 ? -1 : 0; }

static const unsigned VTC_MAGIC = 0x31435456;	// "VTC1"
static const unsigned VTC_VERSION = 2;

struct FVtcHeader
{
	unsigned Magic, Version;
	unsigned Width, Height;
	unsigned Dxt5;			// 0 = DXT1
	unsigned Levels;
	int Masked, IsTransparent, GLIsTransparent;
	unsigned Reserved[3];
};

FVitaTexCachePending Vita_TexCachePending;
static bool DirMade;
static unsigned CacheHits, CacheWrites;

static unsigned long long Fnv(unsigned long long h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
	return h;
}

void Vita_IoBusy(unsigned ms);
bool Vita_TexUncompressed;
bool Vita_TexSmall(FTexture *tex)
{
	const int w = tex->GetWidth(), h = tex->GetHeight();
	if ((tex->UseType == FTexture::TEX_MiscPatch || tex->UseType == FTexture::TEX_FontChar) && w * h <= 256 * 256) return true;
	return w <= 64 && h <= 64;
}

unsigned long long Vita_TexCacheKey(FTexture *tex, int cm, int kind, bool hires, const void *trans, unsigned transsize)
{
	Vita_TexUncompressed = false;
	if (!vita_texcache || tex == NULL || tex->bHasCanvas || tex->bWarped) return 0;
	// Font characters have no name of their own and share their font's lump:
	// glyphs of one size all got the same key (0035: text was garbled).
	if (tex->UseType == FTexture::TEX_FontChar || (tex->Name[0] == 0 && tex->SourceLump < 0)) { Vita_TexUncompressed = true; return 0; }
	// HUD graphics and small textures: uploaded uncompressed and not cached
	// (0041: compressing and saving 1500 HUD pictures kept the memory card
	// busy for seconds after a level start, and every read waited for it).
	if (Vita_TexSmall(tex)) { Vita_TexUncompressed = true; return 0; }
	unsigned long long h = 14695981039346656037ull;
	if (trans != NULL)
	{
		h = Fnv(h, "trans", 5);
		h = Fnv(h, trans, transsize);
	}
	h = Fnv(h, &VTC_VERSION, sizeof(VTC_VERSION));
	h = Fnv(h, tex->Name, strlen(tex->Name));
	int vals[8] = { tex->SourceLump, 0, 0, tex->GetWidth(), tex->GetHeight(), cm, kind, hires };
	if (tex->SourceLump >= 0)
	{
		vals[1] = Wads.LumpLength(tex->SourceLump);
		vals[2] = Wads.GetLumpOffset(tex->SourceLump);
		const char *wad = Wads.GetWadName(Wads.GetLumpFile(tex->SourceLump));
		if (wad != NULL) h = Fnv(h, wad, strlen(wad));
	}
	h = Fnv(h, vals, sizeof(vals));
	h = Fnv(h, &tex->UseType, sizeof(tex->UseType));
	return h ? h : 1;
}

// Files are spread over 256 folders (texcache/<first two hex digits>/): one
// folder with hundreds of files made every open on the memory card slow
// (0.1 s for a file that isn't there, up to 0.7 s to read a cached texture).
// Which keys exist is kept in texcache/index.dat, so a texture that isn't
// cached costs no file access at all.
static void CachePath(char *buf, size_t n, unsigned long long key)
{
	snprintf(buf, n, TEXCACHE_DIR "/%02x/%016llx.vtc", (unsigned)(key >> 56), key);
}

static TMap<unsigned long long, bool> Index;
static bool IndexLoaded;
static SceUID IndexMutex = -1;

#include <dirent.h>
static void LoadIndex()
{
	if (IndexLoaded) return;
	IndexLoaded = true;
	IndexMutex = sceKernelCreateMutex("vita_texindex", 0, 0, NULL);
	const unsigned long long t0 = sceKernelGetProcessTimeWide();
	VFile *f = vfopen(TEXCACHE_DIR "/index.dat", "rb");
	if (f != NULL)
	{
		const int size = sceIoLseek32(f->fd, 0, SCE_SEEK_END);
		sceIoLseek32(f->fd, 0, SCE_SEEK_SET);
		TArray<unsigned long long> keys;
		if (size > 0) { keys.Resize(size / 8); keys.Resize((unsigned)vfread(&keys[0], 8, size / 8, f)); }
		for (unsigned i = 0; i < keys.Size(); i++) Index[keys[i]] = true;
		vfclose(f);
		Vita_BootLog("texture cache: %u textures in the index (%llu ms)", Index.CountUsed(), (sceKernelGetProcessTimeWide() - t0) / 1000);
		return;
	}
	// No index yet (cache from 0032-0034, or none): move flat files into
	// the folders once and write the index.
	vmkdir(TEXCACHE_DIR, 0777);
	DIR *d = opendir(TEXCACHE_DIR);
	TArray<unsigned long long> flat;
	if (d != NULL)
	{
		struct dirent *de;
		while ((de = readdir(d)) != NULL)
		{
			unsigned long long k;
			if (strlen(de->d_name) == 20 && sscanf(de->d_name, "%16llx.vtc", &k) == 1) flat.Push(k);
		}
		closedir(d);
	}
	extern void Vita_LoadScreenBegin(const char *), Vita_LoadScreenEnd(), Vita_LoadProgress(const char *, int, int, bool);
	extern bool Vita_LoadScreenActive();
	const bool screenwasactive = Vita_LoadScreenActive();
	if (flat.Size() > 0 && !screenwasactive) Vita_LoadScreenBegin("(first start of this version)");
	VFile *idx = vfopen(TEXCACHE_DIR "/index.dat", "wb");
	for (unsigned i = 0; i < flat.Size(); i++)
	{
		Vita_LoadProgress("Sorting the texture cache", i, flat.Size(), false);
		char from[96], to[96], dir[64];
		snprintf(from, sizeof(from), TEXCACHE_DIR "/%016llx.vtc", flat[i]);
		snprintf(dir, sizeof(dir), TEXCACHE_DIR "/%02x", (unsigned)(flat[i] >> 56));
		vmkdir(dir, 0777);
		CachePath(to, sizeof(to), flat[i]);
		if (vrename(from, to) == 0)
		{
			Index[flat[i]] = true;
			if (idx) vfwrite(&flat[i], sizeof(flat[i]), 1, idx);
		}
	}
	if (idx) vfclose(idx);
	if (flat.Size() > 0 && !screenwasactive) Vita_LoadScreenEnd();
	Vita_BootLog("texture cache: new index, %u textures moved into folders (%llu ms)", Index.CountUsed(), (sceKernelGetProcessTimeWide() - t0) / 1000);
}

void Vita_TexCacheInit()
{
	LoadIndex();
}

static bool InIndex(unsigned long long key)
{
	LoadIndex();
	sceKernelLockMutex(IndexMutex, 1, NULL);
	bool r = Index.CheckKey(key) != NULL;
	sceKernelUnlockMutex(IndexMutex, 1);
	return r;
}

static void AddToIndex(unsigned long long key)	// writer thread
{
	sceKernelLockMutex(IndexMutex, 1, NULL);
	Index[key] = true;
	sceKernelUnlockMutex(IndexMutex, 1);
	VFile *f = vfopen(TEXCACHE_DIR "/index.dat", "ab");
	if (f) { vfwrite(&key, sizeof(key), 1, f); vfclose(f); }
}

static inline bool IsPow2(int v) { return v >= 4 && (v & (v - 1)) == 0; }

static unsigned LevelSize(int w, int h, bool dxt5)
{
	return (unsigned)((w + 3) / 4) * ((h + 3) / 4) * (dxt5 ? 16 : 8);
}

// Uploads the levels of a compressed chain to the bound texture.
extern "C" int vgl_zand_prealloc_levels, vgl_zand_cpu_swizzle, vgl_zand_preswizzled;
extern "C" void SwizzleTexData64Bpp(uint8_t *dst, uint8_t *src, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t stride, uint32_t tileSize);
extern "C" void SwizzleTexData128Bpp(uint8_t *dst, uint8_t *src, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t stride, uint32_t tileSize);	// vitaGL (patched): allocate the whole mip chain with level 0

static void UploadLevels(const unsigned char *data, int w, int h, bool dxt5, int levels)
{
	vgl_zand_prealloc_levels = levels;
	vgl_zand_cpu_swizzle = 1;
	const GLenum fmt = dxt5 ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
	for (int l = 0; l < levels; l++)
	{
		const unsigned sz = LevelSize(w, h, dxt5);
		glCompressedTexImage2D(GL_TEXTURE_2D, l, fmt, w, h, 0, sz, data);
		data += sz;
		w = w > 4 ? w / 2 : 4;
		h = h > 4 ? h / 2 : 4;
	}
	vgl_zand_prealloc_levels = 0;
	vgl_zand_cpu_swizzle = 0;
}

// [Vita] 0045: the GPU layout of a compressed chain, made on the texbuild
// thread (same calls vitaGL makes in its CPU path), so the main thread only
// copies it in.
static unsigned char *SwizzleChain(const unsigned char *data, int w, int h, bool dxt5, int levels, unsigned total)
{
	unsigned char *out = (unsigned char *)malloc(total);
	if (out == NULL) return NULL;
	const unsigned char *src = data;
	unsigned char *dst = out;
	for (int l = 0; l < levels; l++)
	{
		const unsigned sz = LevelSize(w, h, dxt5);
		// in 4x4 blocks, like vitaGL's ALIGNBLOCK(x, 4)
		const int bw = (w + 3) / 4, bh = (h + 3) / 4;
		const int tile = ((w < h ? w : h) + 3) / 4;
		if (dxt5) SwizzleTexData128Bpp(dst, (uint8_t *)src, 0, 0, bw, bh, bw, tile);
		else SwizzleTexData64Bpp(dst, (uint8_t *)src, 0, 0, bw, bh, bw, tile);
		src += sz; dst += sz;
		w = w > 4 ? w / 2 : 4;
		h = h > 4 ? h / 2 : 4;
	}
	return out;
}

static void UploadLevelsPre(const unsigned char *swz, int w, int h, bool dxt5, int levels)
{
	vgl_zand_prealloc_levels = levels;
	vgl_zand_preswizzled = 1;
	const GLenum fmt = dxt5 ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
	for (int l = 0; l < levels; l++)
	{
		const unsigned sz = LevelSize(w, h, dxt5);
		glCompressedTexImage2D(GL_TEXTURE_2D, l, fmt, w, h, 0, sz, swz);
		swz += sz;
		w = w > 4 ? w / 2 : 4;
		h = h > 4 ? h / 2 : 4;
	}
	vgl_zand_prealloc_levels = 0;
	vgl_zand_preswizzled = 0;
}

// Ready textures uploaded per frame: at most ~1.5 MB of them (one 1024x1024
// skin) per frame; the rest keep their placeholder one frame longer.
static unsigned UploadFrameNo, UploadFrameBytes;
extern unsigned Vita_FrameNo;
CVAR(Bool, vita_preswizzle, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static bool LoadReady(unsigned long long key, FVitaTexCacheMeta &meta, bool mipmaps);
bool Vita_TexCacheLoad(unsigned long long key, FVitaTexCacheMeta &meta, bool mipmaps)
{
	if (key == 0) return false;
	if (LoadReady(key, meta, mipmaps)) return true;
	if (!InIndex(key)) return false;
	Vita_IoBusy(100);
	char path[96];
	CachePath(path, sizeof(path), key);
	VFile *f = vfopen(path, "rb");
	if (f == NULL) return false;
	FVtcHeader hd;
	bool ok = false;
	if (vfread(&hd, sizeof(hd), 1, f) != 1 || hd.Magic != VTC_MAGIC || hd.Version != VTC_VERSION) { vfclose(f); return false; }
	if (hd.Dxt5 == 2) { vfclose(f); return false; }	// 0035's raw entries: some were padded (wrong UVs)
	if (hd.Dxt5 == 3)
	{
		// small texture, raw RGBA
		const unsigned total = hd.Width * hd.Height * 4;
		unsigned char *data = hd.Width > 0 && hd.Width <= 64 && hd.Height > 0 && hd.Height <= 64 ? (unsigned char *)malloc(total) : NULL;
		if (data != NULL && vfread(data, 1, total, f) == total)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, hd.Width, hd.Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
			meta.Width = hd.Width; meta.Height = hd.Height;
			meta.Masked = hd.Masked; meta.IsTransparent = hd.IsTransparent; meta.GLIsTransparent = hd.GLIsTransparent;
			ok = true;
			CacheHits++;
		}
		free(data);
		vfclose(f);
		return ok;
	}
	if (IsPow2(hd.Width) && IsPow2(hd.Height) && hd.Levels >= 1 && hd.Levels <= 12)
	{
		unsigned total = 0;
		int w = hd.Width, h = hd.Height;
		for (unsigned l = 0; l < hd.Levels; l++) { total += LevelSize(w, h, !!hd.Dxt5); w = w > 4 ? w / 2 : 4; h = h > 4 ? h / 2 : 4; }
		unsigned char *data = (unsigned char *)malloc(total);
		if (data != NULL && vfread(data, 1, total, f) == total)
		{
			const int levels = mipmaps ? hd.Levels : 1;
			UploadLevels(data, hd.Width, hd.Height, !!hd.Dxt5, levels);
			meta.Width = hd.Width;
			meta.Height = hd.Height;
			meta.Masked = hd.Masked;
			meta.IsTransparent = hd.IsTransparent;
			meta.GLIsTransparent = hd.GLIsTransparent;
			ok = true;
			CacheHits++;
		}
		free(data);
	}
	vfclose(f);
	return ok;
}

// 2x2 box filter
static void Downsample(const unsigned char *src, int w, int h, unsigned char *dst)
{
	const int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
	for (int y = 0; y < dh; y++)
	{
		const unsigned char *r0 = src + (size_t)(y * 2) * w * 4;
		const unsigned char *r1 = src + (size_t)(h > 1 ? y * 2 + 1 : y * 2) * w * 4;
		for (int x = 0; x < dw; x++)
		{
			const int x0 = x * 2 * 4, x1 = (w > 1 ? x * 2 + 1 : x * 2) * 4;
			for (int c = 0; c < 4; c++)
				dst[(y * dw + x) * 4 + c] = (unsigned char)((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) >> 2);
		}
	}
}

static void CompressLevel(const unsigned char *rgba, int w, int h, bool dxt5, unsigned char *out)
{
	unsigned char block[64];
	for (int by = 0; by < h; by += 4)
	{
		for (int bx = 0; bx < w; bx += 4)
		{
			for (int y = 0; y < 4; y++)
			{
				const int sy = by + y < h ? by + y : h - 1;
				for (int x = 0; x < 4; x++)
				{
					const int sx = bx + x < w ? bx + x : w - 1;
					memcpy(block + (y * 4 + x) * 4, rgba + ((size_t)sy * w + sx) * 4, 4);
				}
			}
			stb_compress_dxt_block(out, block, dxt5, STB_DXT_NORMAL);
			out += dxt5 ? 16 : 8;
		}
	}
}

// Cache files are written by a thread on the third core: creating and
// renaming files on the memory card took 0.3-0.7 s per texture on the Vita,
// which stalled the game every time a new texture was compressed.
struct FVtcWrite { unsigned long long Key; FVtcHeader Header; unsigned char *Data; unsigned Size; };
static TArray<FVtcWrite> WriteQueue;
static unsigned WriteQueueBytes;
static SceUID WriteMutex = -1, WriteSema = -1, WriteThread = -1;

static void WriteFile(const FVtcWrite &w)
{
	if (!DirMade) { vmkdir(TEXCACHE_DIR, 0777); DirMade = true; }
	char path[96], tmp[100], dir[64];
	snprintf(dir, sizeof(dir), TEXCACHE_DIR "/%02x", (unsigned)(w.Key >> 56));
	vmkdir(dir, 0777);
	CachePath(path, sizeof(path), w.Key);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	VFile *f = vfopen(tmp, "wb");
	if (f == NULL) return;
	bool ok = vfwrite(&w.Header, sizeof(w.Header), 1, f) == 1 && vfwrite(w.Data, 1, w.Size, f) == w.Size;
	ok = (vfclose(f) == 0) && ok;
	if (ok) { vremove(path); ok = vrename(tmp, path) == 0; }
	if (!ok) vremove(tmp);
	else { CacheWrites++; AddToIndex(w.Key); }
}

// [Vita] The game's own file reads go first: the writer waits while they
// happen (and for a few seconds after a level starts) and leaves gaps
// between files. 0041: cache writes after a level start made 4 KB sound
// loads take 0.6 s and froze the game for seconds.
volatile unsigned long long Vita_IoQuietUntil;
void Vita_IoBusy(unsigned ms)
{
	const unsigned long long t = sceKernelGetProcessTimeWide() + ms * 1000ull;
	if (t > Vita_IoQuietUntil) Vita_IoQuietUntil = t;
}

static int WriterEntry(SceSize, void *)
{
	for (;;)
	{
		sceKernelWaitSema(WriteSema, 1, NULL);
		// (at most 6 s: streamed music reads all the time)
		for (int waited = 0; waited < 300 && sceKernelGetProcessTimeWide() < Vita_IoQuietUntil; waited++) sceKernelDelayThread(20000);
		sceKernelLockMutex(WriteMutex, 1, NULL);
		if (WriteQueue.Size() == 0) { sceKernelUnlockMutex(WriteMutex, 1); continue; }
		FVtcWrite w = WriteQueue[0];
		WriteQueue.Delete(0);
		sceKernelUnlockMutex(WriteMutex, 1);
		WriteFile(w);
		sceKernelDelayThread(15000);	// room for the game's reads
		sceKernelLockMutex(WriteMutex, 1, NULL);
		WriteQueueBytes -= w.Size;
		sceKernelUnlockMutex(WriteMutex, 1);
		free(w.Data);
	}
	return 0;
}

static bool QueueWrite(unsigned long long key, const FVtcHeader &hd, unsigned char *data, unsigned size)
{
	if (WriteThread < 0)
	{
		WriteMutex = sceKernelCreateMutex("vita_texcache", 0, 0, NULL);
		WriteSema = sceKernelCreateSema("vita_texcache", 0, 0, 0x7fffffff, NULL);
		WriteThread = sceKernelCreateThread("vita_texcache", WriterEntry, 0x10000100 + 20, 0x8000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
		if (WriteMutex < 0 || WriteSema < 0 || WriteThread < 0) { WriteThread = -1; return false; }
		sceKernelStartThread(WriteThread, 0, NULL);
	}
	sceKernelLockMutex(WriteMutex, 1, NULL);
	const bool room = WriteQueueBytes + size <= (24u << 20);
	if (room)
	{
		FVtcWrite w = { key, hd, data, size };
		WriteQueue.Push(w);
		WriteQueueBytes += size;
	}
	sceKernelUnlockMutex(WriteMutex, 1);
	if (room) sceKernelSignalSema(WriteSema, 1);
	return room;
}

// Compresses rgba (power-of-two size) into a DXT chain in memory.
static unsigned char *CompressChain(const unsigned char *rgba, int w, int h, bool mipmaps, bool &dxt5, int &levels, unsigned &total)
{
	dxt5 = false;
	{
		const unsigned *px = (const unsigned *)rgba;
		const size_t n = (size_t)w * h;
		for (size_t i = 0; i < n; i++) if ((px[i] >> 24) != 0xff) { dxt5 = true; break; }
	}
	levels = 1;
	if (mipmaps && vita_texcachemips) { int lw = w, lh = h; while (lw > 4 && lh > 4) { lw /= 2; lh /= 2; levels++; } }

	total = 0;
	{ int lw = w, lh = h; for (int l = 0; l < levels; l++) { total += LevelSize(lw, lh, dxt5); lw = lw > 4 ? lw / 2 : 4; lh = lh > 4 ? lh / 2 : 4; } }
	unsigned char *out = (unsigned char *)malloc(total);
	unsigned char *mip = levels > 1 ? (unsigned char *)malloc((size_t)(w / 2) * (h / 2) * 4 * 2) : NULL;
	if (out == NULL || (levels > 1 && mip == NULL)) { free(out); free(mip); return NULL; }

	// level 0 from the source, the others from successive downsamples
	unsigned char *dst = out;
	const unsigned char *cur = rgba;
	unsigned char *bufA = mip, *bufB = mip ? mip + (size_t)(w / 2) * (h / 2) * 4 : NULL;
	int lw = w, lh = h;
	for (int l = 0; l < levels; l++)
	{
		CompressLevel(cur, lw, lh, dxt5, dst);
		dst += LevelSize(lw, lh, dxt5);
		if (l + 1 < levels)
		{
			unsigned char *next = (cur == bufA) ? bufB : bufA;
			Downsample(cur, lw, lh, next);
			cur = next;
			lw /= 2; lh /= 2;
		}
	}
	free(mip);
	return out;
}

bool Vita_TexCacheCompressUpload(const unsigned char *rgba, int w, int h, bool mipmaps)
{
	if (!vita_texcache || !IsPow2(w) || !IsPow2(h) || rgba == NULL) return false;
	bool dxt5; int levels; unsigned total;
	unsigned char *out = CompressChain(rgba, w, h, mipmaps, dxt5, levels, total);
	if (out == NULL) return false;

	UploadLevels(out, w, h, dxt5, levels);

	if (Vita_TexCachePending.Key != 0)
	{
		LoadIndex();
		FVtcHeader hd;
		memset(&hd, 0, sizeof(hd));
		hd.Magic = VTC_MAGIC; hd.Version = VTC_VERSION;
		hd.Width = w; hd.Height = h; hd.Dxt5 = dxt5; hd.Levels = levels;
		hd.Masked = Vita_TexCachePending.Meta.Masked;
		hd.IsTransparent = Vita_TexCachePending.Meta.IsTransparent;
		hd.GLIsTransparent = Vita_TexCachePending.Meta.GLIsTransparent;
		if (QueueWrite(Vita_TexCachePending.Key, hd, out, total)) out = NULL;	// the writer frees it
		Vita_TexCachePending.Key = 0;
	}
	free(out);
	return true;
}

//==========================================================================
//
// Textures prepared on the third core (bot colour skins): reading the
// cache file, or translating (about 1 s for a 1024x1024 skin) and
// compressing them, stalled the game every time a bot appeared. The game
// draws the skin without the colours until it is ready.
//
//==========================================================================

struct FVtcJob
{
	unsigned long long Key;
	FVitaBuildFn Build;
	void *Ctx;
	int Ctx2, Ctx3;
	FVitaTexCacheMeta Meta;
};
struct FVtcReady
{
	FVtcHeader Header;
	unsigned char *Data;	// NULL: failed
	unsigned char *Swz;		// 0045: the same levels already swizzled for the GPU (or NULL)
	unsigned Size;
	bool FromFile;
};
static TArray<FVtcJob> Jobs;
static TMap<unsigned long long, FVtcReady> Ready;
static TMap<unsigned long long, bool> Queued;
static SceUID JobMutex = -1, JobSema = -1, JobThread = -1, JobIdleSema = -1;
static volatile int JobBusy;

static bool ReadCacheFile(unsigned long long key, FVtcReady &r)
{
	char path[96];
	CachePath(path, sizeof(path), key);
	VFile *f = vfopen(path, "rb");
	if (f == NULL) return false;
	bool ok = false;
	if (vfread(&r.Header, sizeof(r.Header), 1, f) == 1 && r.Header.Magic == VTC_MAGIC && r.Header.Version == VTC_VERSION
		&& r.Header.Dxt5 < 2 && IsPow2(r.Header.Width) && IsPow2(r.Header.Height) && r.Header.Levels >= 1 && r.Header.Levels <= 12)
	{
		unsigned total = 0;
		int w = r.Header.Width, h = r.Header.Height;
		for (unsigned l = 0; l < r.Header.Levels; l++) { total += LevelSize(w, h, !!r.Header.Dxt5); w = w > 4 ? w / 2 : 4; h = h > 4 ? h / 2 : 4; }
		r.Data = (unsigned char *)malloc(total);
		r.Size = total;
		if (r.Data != NULL && vfread(r.Data, 1, total, f) == total) ok = true;
		else { free(r.Data); r.Data = NULL; }
	}
	vfclose(f);
	r.FromFile = true;
	return ok;
}

// [Vita] Set while a job's build function runs (texbuild thread only): the
// build function may fill in the texture's flags it found while decoding.
FVitaTexCacheMeta *Vita_JobMeta;

static void RunJob(const FVtcJob &jobin)
{
	FVtcJob job = jobin;
	Vita_JobMeta = &job.Meta;
	FVtcReady r;
	memset(&r, 0, sizeof(r));
	bool ok = InIndex(job.Key) && ReadCacheFile(job.Key, r);
	if (!ok)
	{
		int w = 0, h = 0;
		unsigned char *rgba = NULL;
		try { rgba = job.Build(job.Ctx, job.Ctx2, job.Ctx3, job.Key, w, h); }
		catch (...) { rgba = NULL; }
		if (rgba != NULL && IsPow2(w) && IsPow2(h))
		{
			bool dxt5; int levels; unsigned total;
			r.Data = CompressChain(rgba, w, h, true, dxt5, levels, total);
			if (r.Data != NULL)
			{
				memset(&r.Header, 0, sizeof(r.Header));
				r.Header.Magic = VTC_MAGIC; r.Header.Version = VTC_VERSION;
				r.Header.Width = w; r.Header.Height = h; r.Header.Dxt5 = dxt5; r.Header.Levels = levels;
				r.Header.Masked = job.Meta.Masked;
				r.Header.IsTransparent = job.Meta.IsTransparent;
				r.Header.GLIsTransparent = job.Meta.GLIsTransparent;
				r.Size = total;
				r.FromFile = false;
			}
		}
		delete[] rgba;
	}
	if (r.Data != NULL && vita_preswizzle && IsPow2(r.Header.Width) && IsPow2(r.Header.Height) && r.Header.Dxt5 != 3 && r.Header.Levels >= 1)
		r.Swz = SwizzleChain(r.Data, r.Header.Width, r.Header.Height, !!r.Header.Dxt5, r.Header.Levels, r.Size);
	sceKernelLockMutex(JobMutex, 1, NULL);
	Ready[job.Key] = r;
	sceKernelUnlockMutex(JobMutex, 1);
}

static int JobEntry(SceSize, void *)
{
	for (;;)
	{
		sceKernelWaitSema(JobSema, 1, NULL);
		sceKernelLockMutex(JobMutex, 1, NULL);
		if (Jobs.Size() == 0) { sceKernelUnlockMutex(JobMutex, 1); continue; }
		FVtcJob job = Jobs[0];
		Jobs.Delete(0);
		JobBusy = 1;
		sceKernelUnlockMutex(JobMutex, 1);
		RunJob(job);
		sceKernelLockMutex(JobMutex, 1, NULL);
		JobBusy = 0;
		const bool idle = Jobs.Size() == 0;
		sceKernelUnlockMutex(JobMutex, 1);
		if (idle) sceKernelSignalSema(JobIdleSema, 1);
	}
	return 0;
}

int Vita_TexCacheAsync(unsigned long long key, FVitaBuildFn build, void *ctx, int ctx2, int ctx3, const FVitaTexCacheMeta &meta)
{
	if (key == 0 || !vita_texcache) return VTC_ASYNC_NO;
	LoadIndex();
	if (JobThread < 0)
	{
		JobMutex = sceKernelCreateMutex("vita_texbuild", 0, 0, NULL);
		JobSema = sceKernelCreateSema("vita_texbuild", 0, 0, 0x7fffffff, NULL);
		JobIdleSema = sceKernelCreateSema("vita_texidle", 0, 0, 0x7fffffff, NULL);
		JobThread = sceKernelCreateThread("vita_texbuild", JobEntry, 0x10000100 + 30, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
		if (JobMutex < 0 || JobSema < 0 || JobThread < 0 || JobIdleSema < 0) { JobThread = -1; return VTC_ASYNC_NO; }
		sceKernelStartThread(JobThread, 0, NULL);
	}
	sceKernelLockMutex(JobMutex, 1, NULL);
	int res;
	FVtcReady *r = Ready.CheckKey(key);
	if (r != NULL)
	{
		res = r->Data != NULL ? VTC_ASYNC_READY : VTC_ASYNC_NO;
		if (res == VTC_ASYNC_READY)
		{
			if (UploadFrameNo != Vita_FrameNo) { UploadFrameNo = Vita_FrameNo; UploadFrameBytes = 0; }
			if (UploadFrameBytes > 0 && UploadFrameBytes + r->Size > 1536u * 1024u) res = VTC_ASYNC_WAIT;	// next frame
		}
	}
	else if (Queued.CheckKey(key) != NULL) res = VTC_ASYNC_WAIT;
	else
	{
		FVtcJob job = { key, build, ctx, ctx2, ctx3, meta };
		Jobs.Push(job);
		Queued[key] = true;
		res = VTC_ASYNC_WAIT;
		sceKernelSignalSema(JobSema, 1);
	}
	sceKernelUnlockMutex(JobMutex, 1);
	return res;
}

// Uploads a ready texture to the bound GL texture (and saves it if it was built).
static bool LoadReady(unsigned long long key, FVitaTexCacheMeta &meta, bool mipmaps)
{
	if (JobThread < 0) return false;
	sceKernelLockMutex(JobMutex, 1, NULL);
	FVtcReady *pr = Ready.CheckKey(key);
	FVtcReady r;
	const bool have = pr != NULL && pr->Data != NULL;
	if (have) { r = *pr; Ready.Remove(key); Queued.Remove(key); }
	sceKernelUnlockMutex(JobMutex, 1);
	if (!have) return false;
	const FVtcHeader &hd = r.Header;
	if (r.Swz != NULL) UploadLevelsPre(r.Swz, hd.Width, hd.Height, !!hd.Dxt5, mipmaps ? hd.Levels : 1);
	else UploadLevels(r.Data, hd.Width, hd.Height, !!hd.Dxt5, mipmaps ? hd.Levels : 1);
	free(r.Swz);
	if (UploadFrameNo != Vita_FrameNo) { UploadFrameNo = Vita_FrameNo; UploadFrameBytes = 0; }
	UploadFrameBytes += r.Size;
	meta.Width = hd.Width; meta.Height = hd.Height;
	meta.Masked = hd.Masked; meta.IsTransparent = hd.IsTransparent; meta.GLIsTransparent = hd.GLIsTransparent;
	CacheHits++;
	if (!r.FromFile && QueueWrite(key, hd, r.Data, r.Size)) r.Data = NULL;
	free(r.Data);
	return true;
}

// Waits for the builder (level change: the textures it works on are about to be freed).
void Vita_TexCacheAsyncDrain()
{
	if (JobThread < 0) return;
	sceKernelLockMutex(JobMutex, 1, NULL);
	Jobs.Clear();
	bool busy = JobBusy != 0;
	sceKernelUnlockMutex(JobMutex, 1);
	while (busy)
	{
		SceUInt timeout = 20000;
		sceKernelWaitSema(JobIdleSema, 1, &timeout);
		sceKernelLockMutex(JobMutex, 1, NULL);
		busy = JobBusy != 0;
		sceKernelUnlockMutex(JobMutex, 1);
	}
	sceKernelLockMutex(JobMutex, 1, NULL);
	TMapIterator<unsigned long long, FVtcReady> it(Ready);
	TMap<unsigned long long, FVtcReady>::Pair *pair;
	while (it.NextPair(pair)) { free(pair->Value.Data); free(pair->Value.Swz); }
	Ready.Clear();
	Queued.Clear();
	sceKernelUnlockMutex(JobMutex, 1);
}

void Vita_TexCacheStats(unsigned &hits, unsigned &writes)
{
	hits = CacheHits; writes = CacheWrites;
}

void Vita_TexCacheSaveRaw(const unsigned char *rgba, int w, int h)
{
	const unsigned long long key = Vita_TexCachePending.Key;
	Vita_TexCachePending.Key = 0;
	if (key == 0 || !vita_texcache || rgba == NULL || w <= 0 || h <= 0 || w > 64 || h > 64) return;
	LoadIndex();
	FVtcHeader hd;
	memset(&hd, 0, sizeof(hd));
	hd.Magic = VTC_MAGIC; hd.Version = VTC_VERSION;
	hd.Width = w; hd.Height = h; hd.Dxt5 = 3; hd.Levels = 1;
	hd.Masked = Vita_TexCachePending.Meta.Masked;
	hd.IsTransparent = Vita_TexCachePending.Meta.IsTransparent;
	hd.GLIsTransparent = Vita_TexCachePending.Meta.GLIsTransparent;
	const unsigned size = w * h * 4;
	unsigned char *copy = (unsigned char *)malloc(size);
	if (copy == NULL) return;
	memcpy(copy, rgba, size);
	if (!QueueWrite(key, hd, copy, size)) free(copy);
}
