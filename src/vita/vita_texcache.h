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
unsigned long long Vita_TexCacheKey(FTexture *tex, int cm, int kind, bool hires);
bool Vita_TexCacheLoad(unsigned long long key, FVitaTexCacheMeta &meta, bool mipmaps);
bool Vita_TexCacheCompressUpload(const unsigned char *rgba, int w, int h, bool mipmaps);
void Vita_TexCacheStats(unsigned &hits, unsigned &writes);

#endif
