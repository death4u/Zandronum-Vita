// [Vita] Texture cache: compressed textures on the memory card (vita_texcache.cpp).
#ifndef VITA_TEXCACHE_H
#define VITA_TEXCACHE_H

class FTexture;

struct FVitaTexCacheMeta
{
	unsigned Width, Height;
	int Masked;				// FTexture::bMasked after ProcessData
	int IsTransparent;		// FTexture::gl_info.mIsTransparent
	int GLIsTransparent;	// FGLTexture::bIsTransparent
};

struct FVitaTexCachePending
{
	unsigned long long Key;		// texture being created: save it under this key
	unsigned long long LoadKey;	// texture being created: try to load it from the cache
	bool Loaded;				// LoadKey was found and uploaded
	FVitaTexCacheMeta Meta;
};
extern FVitaTexCachePending Vita_TexCachePending;

// kind: 0 = texture, 1 = patch, 2 = expanded patch
// trans: the colours of a translation (bot/player colours), NULL for none;
// translated textures are keyed by these, so they match in every session.
unsigned long long Vita_TexCacheKey(FTexture *tex, int cm, int kind, bool hires, const void *trans = NULL, unsigned transsize = 0);
// Set by Vita_TexCacheKey: the texture is uploaded uncompressed (HUD
// pictures, font characters, textures up to 64x64); LoadImage clears it.
extern bool Vita_TexUncompressed;
bool Vita_TexSmall(FTexture *tex);
bool Vita_TexCacheLoad(unsigned long long key, FVitaTexCacheMeta &meta, bool mipmaps);
bool Vita_TexCacheCompressUpload(const unsigned char *rgba, int w, int h, bool mipmaps);
void Vita_TexCacheSaveRaw(const unsigned char *rgba, int w, int h);
void Vita_TexCacheStats(unsigned &hits, unsigned &writes);

// Textures prepared on the third core. build makes the RGBA image (new[]),
// or returns NULL; it is called on that thread.
typedef unsigned char *(*FVitaBuildFn)(void *ctx, int ctx2, int ctx3, unsigned long long key, int &w, int &h);
enum { VTC_ASYNC_NO, VTC_ASYNC_WAIT, VTC_ASYNC_READY };
// READY: Vita_TexCacheLoad(key) now uploads it; WAIT: queued, draw
// something else; NO: do it here.
int Vita_TexCacheAsync(unsigned long long key, FVitaBuildFn build, void *ctx, int ctx2, int ctx3, const FVitaTexCacheMeta &meta);
void Vita_TexCacheAsyncDrain();

#endif
