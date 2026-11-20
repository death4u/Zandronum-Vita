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

CVAR(Bool, vita_texcache, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vita_texcachemips, 1, 0)
CVAR(Bool, vita_texdebug, false, 0)	// test switch: 0 = level 0 only

#define TEXCACHE_DIR "ux0:data/zandronum/texcache"
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

unsigned long long Vita_TexCacheKey(FTexture *tex, int cm, int kind, bool hires)
{
	if (!vita_texcache || tex == NULL || tex->bHasCanvas || tex->bWarped) return 0;
	unsigned long long h = 14695981039346656037ull;
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

static void CachePath(char *buf, size_t n, unsigned long long key)
{
	snprintf(buf, n, TEXCACHE_DIR "/%016llx.vtc", key);
}

static inline bool IsPow2(int v) { return v >= 4 && (v & (v - 1)) == 0; }

static unsigned LevelSize(int w, int h, bool dxt5)
{
	return (unsigned)((w + 3) / 4) * ((h + 3) / 4) * (dxt5 ? 16 : 8);
}

// Uploads the levels of a compressed chain to the bound texture.
extern "C" int vgl_zand_prealloc_levels, vgl_zand_cpu_swizzle;	// vitaGL (patched): allocate the whole mip chain with level 0

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

bool Vita_TexCacheLoad(unsigned long long key, FVitaTexCacheMeta &meta, bool mipmaps)
{
	if (key == 0) return false;
	char path[96];
	CachePath(path, sizeof(path), key);
	FILE *f = fopen(path, "rb");
	if (f == NULL) return false;
	FVtcHeader hd;
	bool ok = false;
	if (fread(&hd, sizeof(hd), 1, f) == 1 && hd.Magic == VTC_MAGIC && hd.Version == VTC_VERSION
		&& IsPow2(hd.Width) && IsPow2(hd.Height) && hd.Levels >= 1 && hd.Levels <= 12)
	{
		unsigned total = 0;
		int w = hd.Width, h = hd.Height;
		for (unsigned l = 0; l < hd.Levels; l++) { total += LevelSize(w, h, !!hd.Dxt5); w = w > 4 ? w / 2 : 4; h = h > 4 ? h / 2 : 4; }
		unsigned char *data = (unsigned char *)malloc(total);
		if (data != NULL && fread(data, 1, total, f) == total)
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
	fclose(f);
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
	if (!DirMade) { mkdir(TEXCACHE_DIR, 0777); DirMade = true; }
	char path[96], tmp[100];
	CachePath(path, sizeof(path), w.Key);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (f == NULL) return;
	bool ok = fwrite(&w.Header, sizeof(w.Header), 1, f) == 1 && fwrite(w.Data, 1, w.Size, f) == w.Size;
	ok = (fclose(f) == 0) && ok;
	if (ok) { remove(path); ok = rename(tmp, path) == 0; }
	if (!ok) remove(tmp);
	else CacheWrites++;
}

static int WriterEntry(SceSize, void *)
{
	for (;;)
	{
		sceKernelWaitSema(WriteSema, 1, NULL);
		sceKernelLockMutex(WriteMutex, 1, NULL);
		if (WriteQueue.Size() == 0) { sceKernelUnlockMutex(WriteMutex, 1); continue; }
		FVtcWrite w = WriteQueue[0];
		WriteQueue.Delete(0);
		sceKernelUnlockMutex(WriteMutex, 1);
		WriteFile(w);
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

bool Vita_TexCacheCompressUpload(const unsigned char *rgba, int w, int h, bool mipmaps)
{
	if (!vita_texcache || !IsPow2(w) || !IsPow2(h) || rgba == NULL) return false;

	bool dxt5 = false;
	{
		const unsigned *px = (const unsigned *)rgba;
		const size_t n = (size_t)w * h;
		for (size_t i = 0; i < n; i++) if ((px[i] >> 24) != 0xff) { dxt5 = true; break; }
	}
	int levels = 1;
	if (mipmaps && vita_texcachemips) { int lw = w, lh = h; while (lw > 4 && lh > 4) { lw /= 2; lh /= 2; levels++; } }

	unsigned total = 0;
	{ int lw = w, lh = h; for (int l = 0; l < levels; l++) { total += LevelSize(lw, lh, dxt5); lw = lw > 4 ? lw / 2 : 4; lh = lh > 4 ? lh / 2 : 4; } }
	unsigned char *out = (unsigned char *)malloc(total);
	unsigned char *mip = levels > 1 ? (unsigned char *)malloc((size_t)(w / 2) * (h / 2) * 4 * 2) : NULL;
	if (out == NULL || (levels > 1 && mip == NULL)) { free(out); free(mip); return false; }

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

	UploadLevels(out, w, h, dxt5, levels);

	if (Vita_TexCachePending.Key != 0)
	{
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

void Vita_TexCacheStats(unsigned &hits, unsigned &writes)
{
	hits = CacheHits; writes = CacheWrites;
}
