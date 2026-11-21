/*
** gl_material.cpp
** 
**---------------------------------------------------------------------------
** Copyright 2004-2009 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
** 4. When not used as part of GZDoom or a GZDoom derivative, this code will be
**    covered by the terms of the GNU Lesser General Public License as published
**    by the Free Software Foundation; either version 2.1 of the License, or (at
**    your option) any later version.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include "gl/system/gl_system.h"
#include "w_wad.h"
#include "m_png.h"
#include "sbar.h"
#include "gi.h"
#include "cmdlib.h"
#include "c_dispatch.h"
#include "stats.h"
#include "r_utility.h"
#include "templates.h"
#include "sc_man.h"
#include "colormatcher.h"

//#include "gl/gl_intern.h"

#include "gl/system/gl_interface.h"
#include "gl/system/gl_framebuffer.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/data/gl_data.h"
#include "gl/textures/gl_texture.h"
#include "gl/textures/gl_translate.h"
#include "doomstat.h"
#include "gl/textures/gl_bitmap.h"
#ifdef __vita__
#include "vita/vita_texcache.h"
#ifdef __vita__
#include "vita/vita_perf.h"
#include "vita/vita_system.h"
#endif
#endif
#include "gl/textures/gl_material.h"
#include "gl/shaders/gl_shader.h"

EXTERN_CVAR(Bool, gl_render_precise)
EXTERN_CVAR(Int, gl_lightmode)
EXTERN_CVAR(Bool, gl_precache)
EXTERN_CVAR(Bool, gl_texture_usehires)

//===========================================================================
//
// The GL texture maintenance class
//
//===========================================================================

//===========================================================================
//
// Constructor
//
//===========================================================================
FGLTexture::FGLTexture(FTexture * tx, bool expandpatches)
{
	assert(tx->gl_info.SystemTexture == NULL);
	tex = tx;

	glpatch=NULL;
	for(int i=0;i<5;i++) gltexture[i]=NULL;
	HiresLump=-1;
	hirestexture = NULL;
	currentwarp = 0;
#ifdef __vita__
	VitaWarpSrc = NULL;
	VitaWarpW = VitaWarpH = VitaWarpCm = VitaWarpTrans = 0;
#endif
	bHasColorkey = false;
	bIsTransparent = -1;
	bExpand = expandpatches;
	tex->gl_info.SystemTexture = this;
}

//===========================================================================
//
// Destructor
//
//===========================================================================

FGLTexture::~FGLTexture()
{
	Clean(true);
	if (hirestexture) delete hirestexture;
}

//==========================================================================
//
// Checks for the presence of a hires texture replacement and loads it
//
//==========================================================================
unsigned char *FGLTexture::LoadHiresTexture(FTexture *tex, int *width, int *height, int cm)
{
	if (HiresLump==-1) 
	{
		bHasColorkey = false;
		HiresLump = CheckDDPK3(tex);
		if (HiresLump < 0) HiresLump = CheckExternalFile(tex, bHasColorkey);

		if (HiresLump >=0) 
		{
			hirestexture = FTexture::CreateTexture(HiresLump, FTexture::TEX_Any);
		}
	}
	if (hirestexture != NULL)
	{
		int w=hirestexture->GetWidth();
		int h=hirestexture->GetHeight();

		unsigned char * buffer=new unsigned char[w*(h+1)*4];
		memset(buffer, 0, w * (h+1) * 4);

		FGLBitmap bmp(buffer, w*4, w, h);
		bmp.SetTranslationInfo(cm);

		
		int trans = hirestexture->CopyTrueColorPixels(&bmp, 0, 0);
		hirestexture->CheckTrans(buffer, w*h, trans);
		bIsTransparent = hirestexture->gl_info.mIsTransparent;

		if (bHasColorkey)
		{
			// This is a crappy Doomsday color keyed image
			// We have to remove the key manually. :(
			DWORD * dwdata=(DWORD*)buffer;
			for (int i=(w*h);i>0;i--)
			{
				if (dwdata[i]==0xffffff00 || dwdata[i]==0xffff00ff) dwdata[i]=0;
			}
		}
		*width = w;
		*height = h;
		return buffer;
	}
	return NULL;
}

//===========================================================================
// 
//	Deletes all allocated resources
//
//===========================================================================

void FGLTexture::Clean(bool all)
{
#ifdef __vita__
	if (all && VitaWarpSrc != NULL) { delete[] VitaWarpSrc; VitaWarpSrc = NULL; }
#endif
	for(int i=0;i<5;i++)
	{
		if (gltexture[i]) 
		{
			if (!all) gltexture[i]->Clean(false);
			else
			{
				delete gltexture[i];
				gltexture[i]=NULL;
			}
		}
	}
	if (glpatch) 
	{
		if (!all) glpatch->Clean(false);
		else
		{
			delete glpatch;
			glpatch=NULL;
		}
	}
}

//===========================================================================
//
// FGLTex::WarpBuffer
//
//===========================================================================

BYTE *FGLTexture::WarpBuffer(BYTE *buffer, int Width, int Height, int warp)
{
	if (Width > 256 || Height > 256) return buffer;

	DWORD *in = (DWORD*)buffer;
	DWORD *out = (DWORD*)new BYTE[4*Width*Height];
	float Speed = static_cast<FWarpTexture*>(tex)->GetSpeed();

	static_cast<FWarpTexture*>(tex)->GenTime = r_FrameTime;

	static DWORD linebuffer[256];	// anything larger will bring down performance so it is excluded above.
	DWORD timebase = DWORD(r_FrameTime*Speed*23/28);
	int xsize = Width;
	int ysize = Height;
	int xmask = xsize - 1;
	int ymask = ysize - 1;
	int ds_xbits;
	int i,x;

	if (warp == 1)
	{
		for(ds_xbits=-1,i=Width; i; i>>=1, ds_xbits++);

		for (x = xsize-1; x >= 0; x--)
		{
			int yt, yf = (finesine[(timebase+(x+17)*128)&FINEMASK]>>13) & ymask;
			const DWORD *source = in + x;
			DWORD *dest = out + x;
			for (yt = ysize; yt; yt--, yf = (yf+1)&ymask, dest += xsize)
			{
				*dest = *(source+(yf<<ds_xbits));
			}
		}
		timebase = DWORD(r_FrameTime*Speed*32/28);
		int y;
		for (y = ysize-1; y >= 0; y--)
		{
			int xt, xf = (finesine[(timebase+y*128)&FINEMASK]>>13) & xmask;
			DWORD *source = out + (y<<ds_xbits);
			DWORD *dest = linebuffer;
			for (xt = xsize; xt; xt--, xf = (xf+1)&xmask)
			{
				*dest++ = *(source+xf);
			}
			memcpy (out+y*xsize, linebuffer, xsize*sizeof(DWORD));
		}
	}
	else
	{
		int ybits;
		for(ybits=-1,i=ysize; i; i>>=1, ybits++);

		DWORD timebase = (r_FrameTime * Speed * 40 / 28);
		for (x = xsize-1; x >= 0; x--)
		{
			for (int y = ysize-1; y >= 0; y--)
			{
				int xt = (x + 128
					+ ((finesine[(y*128 + timebase*5 + 900) & 8191]*2)>>FRACBITS)
					+ ((finesine[(x*256 + timebase*4 + 300) & 8191]*2)>>FRACBITS)) & xmask;
				int yt = (y + 128
					+ ((finesine[(y*128 + timebase*3 + 700) & 8191]*2)>>FRACBITS)
					+ ((finesine[(x*256 + timebase*4 + 1200) & 8191]*2)>>FRACBITS)) & ymask;
				const DWORD *source = in + (xt << ybits) + yt;
				DWORD *dest = out + (x << ybits) + y;
				*dest = *source;
			}
		}
	}
	delete [] buffer;
	return (BYTE*)out;
}

//===========================================================================
// 
//	Initializes the buffer for the texture data
//
//===========================================================================

unsigned char * FGLTexture::CreateTexBuffer(int cm, int translation, int & w, int & h, bool expand, FTexture *hirescheck, int warp)
{
	unsigned char * buffer;
	int W, H;


	// Textures that are already scaled in the texture lump will not get replaced
	// by hires textures
	if (gl_texture_usehires && hirescheck != NULL)
	{
		buffer = LoadHiresTexture (hirescheck, &w, &h, cm);
		if (buffer)
		{
			return buffer;
		}
	}

	W = w = tex->GetWidth() + expand*2;
	H = h = tex->GetHeight() + expand*2;


	buffer=new unsigned char[W*(H+1)*4];
	memset(buffer, 0, W * (H+1) * 4);

	FGLBitmap bmp(buffer, W*4, W, H);
	bmp.SetTranslationInfo(cm, translation);

	if (tex->bComplex)
	{
		FBitmap imgCreate;

		// The texture contains special processing so it must be composited using the
		// base bitmap class and then be converted as a whole.
		if (imgCreate.Create(W, H))
		{
			memset(imgCreate.GetPixels(), 0, W * H * 4);
			int trans = tex->CopyTrueColorPixels(&imgCreate, expand, expand);
			bmp.CopyPixelDataRGB(0, 0, imgCreate.GetPixels(), W, H, 4, W * 4, 0, CF_BGRA);
			tex->CheckTrans(buffer, W*H, trans);
			bIsTransparent = tex->gl_info.mIsTransparent;
		}
	}
	else if (translation<=0)
	{
		int trans = tex->CopyTrueColorPixels(&bmp, expand, expand);
		tex->CheckTrans(buffer, W*H, trans);
		bIsTransparent = tex->gl_info.mIsTransparent;
	}
	else
	{
		// When using translations everything must be mapped to the base palette.
		// Since FTexture's method is doing exactly that by calling GetPixels let's use that here
		// to do all the dirty work for us. ;)
		tex->FTexture::CopyTrueColorPixels(&bmp, expand, expand);
		bIsTransparent = 0;
	}

	if (warp != 0)
	{
		buffer = WarpBuffer(buffer, W, H, warp);
	}
	// [BB] The hqnx upsampling (not the scaleN one) destroys partial transparency, don't upsamle textures using it.
	// Also don't upsample warped textures.
	else //if (bIsTransparent != 1)
	{
		// [BB] Potentially upsample the buffer.
		buffer = gl_CreateUpsampledTextureBuffer ( tex, buffer, W, H, w, h, bIsTransparent || cm == CM_SHADE );
	}
	currentwarp = warp;
	currentwarptime = gl_frameMS;

	return buffer;
}


//===========================================================================
// 
//	Create hardware texture for world use
//
//===========================================================================

FHardwareTexture *FGLTexture::CreateTexture(int clampmode)
{
	if (tex->UseType==FTexture::TEX_Null) return NULL;		// Cannot register a NULL texture
	if (!gltexture[clampmode]) 
	{
		gltexture[clampmode] = new FHardwareTexture(tex->GetWidth(), tex->GetHeight(), true, true, false, tex->gl_info.bNoCompress);
	}
	return gltexture[clampmode]; 
}

//===========================================================================
// 
//	Create Hardware texture for patch use
//
//===========================================================================

bool FGLTexture::CreatePatch()
{
	if (tex->UseType==FTexture::TEX_Null) return false;		// Cannot register a NULL texture
	if (!glpatch) 
	{
		glpatch=new FHardwareTexture(tex->GetWidth() + bExpand, tex->GetHeight() + bExpand, false, false, tex->gl_info.bNoFilter, tex->gl_info.bNoCompress);
	}
	if (glpatch) return true; 	
	return false;
}


#ifdef __vita__
//===========================================================================
//
// [Vita] Warped textures. The stock code deleted the texture and rebuilt it
// from the image file every frame (PNG decode + warp + DXT compression:
// ~70 ms for HaloDoom's 256x192 water on Rebellion). Here the unwarped
// picture is decoded once, re-warped at most 35 times a second (the game's
// tic rate, as in the software renderer) and uploaded uncompressed into the
// same texture. Same warp maths, so it looks the same.
//
//===========================================================================
CVAR(Bool, vita_fastwarp, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

const FHardwareTexture *FGLTexture::Vita_BindWarp(FHardwareTexture *hwtex, int texunit, int cm, int clampmode, int translation, int warp)
{
	if (!vita_fastwarp) return NULL;
	const unsigned long long t0 = Vita_PerfNow();
	if (VitaWarpSrc == NULL || VitaWarpCm != cm || VitaWarpTrans != translation)
	{
		if (VitaWarpSrc != NULL) { delete[] VitaWarpSrc; VitaWarpSrc = NULL; }
		int w = 0, h = 0;
		unsigned char *src = NULL;
		try { src = CreateTexBuffer(cm, translation, w, h, false, NULL, 0); }
		catch (std::bad_alloc &) { return NULL; }
		if (src == NULL) return NULL;
		if (w != tex->GetWidth() || h != tex->GetHeight() || w > 256 || h > 256)
		{
			delete[] src;	// resized or too big for the warp code: stock path
			return NULL;
		}
		VitaWarpSrc = src; VitaWarpW = w; VitaWarpH = h; VitaWarpCm = cm; VitaWarpTrans = translation;
		currentwarptime = gl_frameMS - 1000;	// force the first upload
	}
	const bool have = hwtex->Bind(texunit, cm, translation);
	if (have && currentwarp == warp && (unsigned)(gl_frameMS - currentwarptime) < 28u)
		return hwtex;	// this animation step is already uploaded
	const int w = VitaWarpW, h = VitaWarpH;
	unsigned char *buf = new unsigned char[w * (h + 1) * 4];
	memcpy(buf, VitaWarpSrc, w * h * 4);
	memset(buf + w * h * 4, 0, w * 4);
	buf = WarpBuffer(buf, w, h, warp);
	Vita_TexUncompressed = true;	// re-uploaded constantly: never compressed or cached
	Vita_TexCachePending.LoadKey = 0;
	Vita_TexCachePending.Key = 0;
	const bool ok = hwtex->CreateTexture(buf, w, h, true, texunit, cm, translation);
	Vita_TexUncompressed = false;
	delete[] buf;
	if (!ok) return NULL;
	if (!have)
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (clampmode & GLT_CLAMPX)? GL_CLAMP_TO_EDGE : GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (clampmode & GLT_CLAMPY)? GL_CLAMP_TO_EDGE : GL_REPEAT);
	}
	currentwarp = warp;
	currentwarptime = gl_frameMS;
	const unsigned long long dt = Vita_PerfNow() - t0;
	Vita_PerfAcc[VPERF_WARP] += dt; Vita_PerfCalls[VPERF_WARP]++;
	if (dt > 15000) Vita_BootLog("slow warp texture %s %dx%d: %llu ms", tex->Name, w, h, dt / 1000);
	return hwtex;
}
#endif

//===========================================================================
// 
//	Binds a texture to the renderer
//
//===========================================================================

#ifdef __vita__
#include <new>
#include "vita/vita_perf.h"
#include "vita/vita_system.h"
// Logs textures that took long to decode or upload (to see what causes
// in-game stalls).
static void Vita_LogSlowTexture(FTexture *tex, int w, int h, int trans, unsigned long long decode, unsigned long long upload)
{
	Vita_PerfAcc[VPERF_TEXDECODE] += decode;
	if (decode + upload > 15000)
		{ char nm[9]; memcpy(nm, tex->Name, 8); nm[8] = 0; Vita_BootLog("slow texture %s %dx%d trans %d: decode %llu ms, upload %llu ms", nm, w, h, trans, decode / 1000, upload / 1000); }
}
#endif

#ifdef __vita__
// [Vita] Bot colour skins are made on the third core (vita_texcache.cpp).
static unsigned char *Vita_BuildTranslated(void *ctx, int cmhires, int translation, unsigned long long key, int &w, int &h)
{
	FGLTexture *gt = (FGLTexture *)ctx;
	const int cm = cmhires & 0xffff;
	const bool hires = (cmhires >> 16) != 0;
	const PalEntry *pal = GLTranslationPalette::GetPalette(translation);
	// the translation slot may have been given other colours meanwhile
	if (pal == NULL || Vita_TexCacheKey(gt->tex, cm, 0, hires, pal, 256 * sizeof(PalEntry)) != key) return NULL;
	return gt->CreateTexBuffer(cm, translation, w, h, false, hires ? gt->tex : NULL, 0);
}
extern unsigned long long Vita_PrecacheDeadline;

// [Vita] Untranslated textures that weren't loaded in advance (model skins,
// sprites, graphics of things spawned mid-game) are read from the cache or
// decoded and compressed on the texbuild thread too. Before 0044 only bot
// colour skins went there; a 1024x1024 model skin took 0.8 s on the main
// thread. Ctx3 = 1 for sprites (patch layout), 0 for textures.
static unsigned char *Vita_BuildPlain(void *ctx, int cmhires, int patch, unsigned long long key, int &w, int &h)
{
	FGLTexture *gt = (FGLTexture *)ctx;
	const int cm = cmhires & 0xffff;
	const bool hires = (cmhires >> 16) != 0;
	unsigned char *buf = gt->CreateTexBuffer(cm, 0, w, h, patch ? gt->Vita_Expand() : false, hires ? gt->tex : NULL, 0);
	if (buf == NULL) return NULL;
	gt->tex->ProcessData(buf, w, h, patch != 0);
	if (Vita_JobMeta != NULL)
	{
		Vita_JobMeta->Masked = gt->tex->bMasked;
		Vita_JobMeta->IsTransparent = gt->tex->gl_info.mIsTransparent;
		Vita_JobMeta->GLIsTransparent = gt->bIsTransparent;
	}
	return buf;
}

CVAR(Bool, vita_asynctextures, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vita_asyncall, false, 0)	// test only: every texture through the background path

// Which textures may arrive late: not level walls/flats (loaded in advance),
// not HUD/font graphics (uncompressed, loaded in advance), not multi-patch
// textures (built from shared patches), at least 64x64 (small ones are cheap).
static bool Vita_AsyncCandidate(FTexture *tex)
{
	if (!vita_asynctextures || gamestate != GS_LEVEL || Vita_PrecacheDeadline != 0) return false;
	if (vita_asyncall && !tex->bMultiPatch && !tex->bWarped && !tex->bHasCanvas && tex->UseType != FTexture::TEX_MiscPatch && tex->UseType != FTexture::TEX_FontChar) return true;	// test: walls/flats too
	if (tex->bMultiPatch || tex->bHasCanvas || tex->bWarped) return false;
	if (tex->GetWidth() < 64 || tex->GetHeight() < 64) return false;
	switch (tex->UseType)
	{
	case FTexture::TEX_Wall: case FTexture::TEX_Flat: case FTexture::TEX_MiscPatch: case FTexture::TEX_FontChar:
		return false;
	default:
		return true;
	}
}

// Drawn while a texture is still being built: transparent for sprites, a
// neutral grey for model skins and other textures.
static void Vita_BindPlaceholder(int texunit, bool transparent)
{
	static unsigned ids[2];
	unsigned &id = ids[transparent ? 1 : 0];
	if (id == 0)
	{
		unsigned px[8 * 8];
		for (int i = 0; i < 64; i++) px[i] = transparent ? 0 : 0xff808080;
		glGenTextures(1, &id);
		glBindTexture(GL_TEXTURE_2D, id);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	if (texunit != 0) glActiveTexture(GL_TEXTURE0 + texunit);
	glBindTexture(GL_TEXTURE_2D, id);
	if (texunit != 0) glActiveTexture(GL_TEXTURE0);
	FHardwareTexture::lastbound[texunit] = id;
}
#endif

const FHardwareTexture *FGLTexture::Bind(int texunit, int cm, int clampmode, int translation, FTexture *hirescheck, int warp)
{
	int usebright = false;

	if (translation <= 0) translation = -translation;
	else translation = GLTranslationPalette::GetInternalTranslation(translation);

	FHardwareTexture *hwtex;
	
	if (gltexture[4] != NULL && clampmode < 4 && gltexture[clampmode] == NULL)
	{
		hwtex = gltexture[clampmode] = gltexture[4];
		gltexture[4] = NULL;

		if (hwtex->Bind(texunit, cm, translation))
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (clampmode & GLT_CLAMPX)? GL_CLAMP_TO_EDGE : GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (clampmode & GLT_CLAMPY)? GL_CLAMP_TO_EDGE : GL_REPEAT);
		}
	}
	else
	{
		hwtex = CreateTexture(clampmode);
	}

	if (hwtex)
	{
#ifdef __vita__
		if (warp != 0 && !tex->bHasCanvas)
		{
			const FHardwareTexture *r = Vita_BindWarp(hwtex, texunit, cm, clampmode, translation, warp);
			if (r != NULL) return r;
		}
#endif
		if ((warp != 0 || currentwarp != warp) && currentwarptime != gl_frameMS)
		{
			// must recreate the texture
			Clean(true);
			hwtex = CreateTexture(clampmode);
		}

		// Texture has become invalid - this is only for special textures, not the regular warping, which is handled above
		else if ((warp == 0 && !tex->bHasCanvas && !tex->bWarped) && tex->CheckModified())
		{
			Clean(true);
			hwtex = CreateTexture(clampmode);
		}

		// Bind it to the system.
		if (!hwtex->Bind(texunit, cm, translation))
		{
			
			int w=0, h=0;

			// Create this texture
			unsigned char * buffer = NULL;
			
#ifdef __vita__
			if (Vita_PrecacheDeadline == 0) { extern void Vita_LearnTexture(FTexture *); Vita_LearnTexture(tex); }
			const unsigned long long vt0 = Vita_PerfNow();
			const PalEntry *vpal = translation == 0 ? NULL : GLTranslationPalette::GetPalette(translation);
			const unsigned long long vkey = warp == 0 && (translation == 0 || vpal != NULL) ? Vita_TexCacheKey(tex, cm, 0, hirescheck != NULL, vpal, 256 * sizeof(PalEntry)) : 0;
			if (vkey != 0 && translation != 0 && gamestate == GS_LEVEL && Vita_PrecacheDeadline == 0
				&& tex->GetWidth() >= 64 && tex->GetHeight() >= 64 && (hirescheck == NULL || hirescheck == tex))
			{
				FVitaTexCacheMeta meta;
				meta.Width = meta.Height = 0;
				meta.Masked = tex->bMasked;
				meta.IsTransparent = tex->gl_info.mIsTransparent;
				meta.GLIsTransparent = 0;
				const int st = Vita_TexCacheAsync(vkey, Vita_BuildTranslated, this, cm | ((hirescheck != NULL) << 16), translation, meta);
				if (st == VTC_ASYNC_WAIT)
				{
					// not ready yet: draw it without the colours meanwhile
					return Bind(texunit, cm, clampmode, 0, hirescheck, warp);
				}
			}
			if (vkey != 0 && translation == 0 && (hirescheck == NULL || hirescheck == tex) && Vita_AsyncCandidate(tex))
			{
				FVitaTexCacheMeta meta;
				meta.Width = meta.Height = 0;
				meta.Masked = tex->bMasked;
				meta.IsTransparent = tex->gl_info.mIsTransparent;
				meta.GLIsTransparent = bIsTransparent;
				if (Vita_TexCacheAsync(vkey, Vita_BuildPlain, this, cm | ((hirescheck != NULL) << 16), 0, meta) == VTC_ASYNC_WAIT)
				{
					Vita_BindPlaceholder(texunit, false);
					return hwtex;
				}
			}
			if (vkey != 0)
			{
				Vita_TexCachePending.LoadKey = vkey;
				Vita_TexCachePending.Loaded = false;
				hwtex->CreateTexture(NULL, 0, 0, true, texunit, cm, translation);
				if (Vita_TexCachePending.Loaded)
				{
					tex->bMasked = !!Vita_TexCachePending.Meta.Masked;
					tex->gl_info.mIsTransparent = Vita_TexCachePending.Meta.IsTransparent;
					bIsTransparent = Vita_TexCachePending.Meta.GLIsTransparent;
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (clampmode & GLT_CLAMPX)? GL_CLAMP_TO_EDGE : GL_REPEAT);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (clampmode & GLT_CLAMPY)? GL_CLAMP_TO_EDGE : GL_REPEAT);
					Vita_LogSlowTexture(tex, Vita_TexCachePending.Meta.Width, Vita_TexCachePending.Meta.Height, translation, 0, Vita_PerfNow() - vt0);
					return hwtex;
				}
			}
#endif
			if (!tex->bHasCanvas)
			{
#ifdef __vita__
				// Out of memory for a big texture: draw without it rather
				// than end the game.
				try { buffer = CreateTexBuffer(cm, translation, w, h, false, hirescheck, warp); }
				catch (std::bad_alloc &) { Vita_BootLog("texture skipped: out of memory"); return NULL; }
#else
				buffer = CreateTexBuffer(cm, translation, w, h, false, hirescheck, warp);
#endif
				tex->ProcessData(buffer, w, h, false);
			}
#ifdef __vita__
			if (vkey != 0)	// (a cached masked texture just isn't split into its solid areas: a draw-time optimisation)
			{
				Vita_TexCachePending.Key = vkey;
				Vita_TexCachePending.Meta.Masked = tex->bMasked;
				Vita_TexCachePending.Meta.IsTransparent = tex->gl_info.mIsTransparent;
				Vita_TexCachePending.Meta.GLIsTransparent = bIsTransparent;
			}
			const unsigned long long vt1 = Vita_PerfNow();
			const bool vok = hwtex->CreateTexture(buffer, w, h, true, texunit, cm, translation);
			Vita_LogSlowTexture(tex, w, h, translation, vt1 - vt0, Vita_PerfNow() - vt1);
			if (!vok)
#else
			if (!hwtex->CreateTexture(buffer, w, h, true, texunit, cm, translation)) 
#endif
			{
				// could not create texture
				delete[] buffer;
				return NULL;
			}
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (clampmode & GLT_CLAMPX)? GL_CLAMP_TO_EDGE : GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (clampmode & GLT_CLAMPY)? GL_CLAMP_TO_EDGE : GL_REPEAT);
			delete[] buffer;
		}

		if (tex->bHasCanvas) static_cast<FCanvasTexture*>(tex)->NeedUpdate();
		return hwtex; 
	}
	return NULL;
}

//===========================================================================
// 
//	Binds a sprite to the renderer
//
//===========================================================================
const FHardwareTexture * FGLTexture::BindPatch(int texunit, int cm, int translation, int warp)
{
	bool usebright = false;
	int transparm = translation;

	if (translation <= 0) translation = -translation;
	else translation = GLTranslationPalette::GetInternalTranslation(translation);

	if (CreatePatch())
	{
		if (warp != 0 || currentwarp != warp)
		{
			// must recreate the texture
			Clean(true);
			CreatePatch();
		}

		// Texture has become invalid - this is only for special textures, not the regular warping, which is handled above
		else if ((warp == 0 && !tex->bHasCanvas && !tex->bWarped) && tex->CheckModified())
		{
			Clean(true);
			CreatePatch();
		}


		// Bind it to the system. 
		if (!glpatch->Bind(texunit, cm, translation))
		{
			int w, h;

			// Create this texture
#ifdef __vita__
			if (Vita_PrecacheDeadline == 0) { extern void Vita_LearnTexture(FTexture *); Vita_LearnTexture(tex); }
			const unsigned long long vt0 = Vita_PerfNow();
			const PalEntry *vpal = translation == 0 ? NULL : GLTranslationPalette::GetPalette(translation);
			const unsigned long long vkey = warp == 0 && (translation == 0 || vpal != NULL) ? Vita_TexCacheKey(tex, cm, bExpand ? 2 : 1, false, vpal, 256 * sizeof(PalEntry)) : 0;
			if (vkey != 0 && translation == 0 && Vita_AsyncCandidate(tex))
			{
				FVitaTexCacheMeta meta;
				meta.Width = meta.Height = 0;
				meta.Masked = tex->bMasked;
				meta.IsTransparent = tex->gl_info.mIsTransparent;
				meta.GLIsTransparent = bIsTransparent;
				if (Vita_TexCacheAsync(vkey, Vita_BuildPlain, this, cm, 1, meta) == VTC_ASYNC_WAIT)
				{
					Vita_BindPlaceholder(texunit, true);
					return glpatch;
				}
			}
			if (vkey != 0)
			{
				Vita_TexCachePending.LoadKey = vkey;
				Vita_TexCachePending.Loaded = false;
				glpatch->CreateTexture(NULL, 0, 0, false, texunit, cm, translation);
				if (Vita_TexCachePending.Loaded)
				{
					tex->bMasked = !!Vita_TexCachePending.Meta.Masked;
					tex->gl_info.mIsTransparent = Vita_TexCachePending.Meta.IsTransparent;
					bIsTransparent = Vita_TexCachePending.Meta.GLIsTransparent;
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
					Vita_LogSlowTexture(tex, Vita_TexCachePending.Meta.Width, Vita_TexCachePending.Meta.Height, translation, 0, Vita_PerfNow() - vt0);
					return glpatch;
				}
			}
#endif
#ifdef __vita__
			unsigned char * buffer;
			try { buffer = CreateTexBuffer(cm, translation, w, h, bExpand, NULL, warp); }
			catch (std::bad_alloc &) { Vita_BootLog("sprite skipped: out of memory"); return NULL; }
#else
			unsigned char * buffer = CreateTexBuffer(cm, translation, w, h, bExpand, NULL, warp);
#endif
#ifdef __vita__
			const unsigned long long vt1 = Vita_PerfNow();
#endif
			tex->ProcessData(buffer, w, h, true);
#ifdef __vita__
			if (vkey != 0)
			{
				Vita_TexCachePending.Key = vkey;
				Vita_TexCachePending.Meta.Masked = tex->bMasked;
				Vita_TexCachePending.Meta.IsTransparent = tex->gl_info.mIsTransparent;
				Vita_TexCachePending.Meta.GLIsTransparent = bIsTransparent;
			}
			const bool vok = glpatch->CreateTexture(buffer, w, h, false, texunit, cm, translation);
			Vita_LogSlowTexture(tex, w, h, translation, vt1 - vt0, Vita_PerfNow() - vt1);
			if (!vok)
#else
			if (!glpatch->CreateTexture(buffer, w, h, false, texunit, cm, translation)) 
#endif
			{
				// could not create texture
				delete[] buffer;
				return NULL;
			}
			delete[] buffer;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		return glpatch; 	
	}
	return NULL;
}


//===========================================================================
//
//
//
//===========================================================================

fixed_t FTexCoordInfo::RowOffset(fixed_t rowoffset) const
{
	if (mTempScaleY == FRACUNIT)
	{
		if (mScaleY==FRACUNIT || mWorldPanning) return rowoffset;
		else return FixedDiv(rowoffset, mScaleY);
	}
	else
	{
		if (mWorldPanning) return FixedDiv(rowoffset, mTempScaleY);
		else return FixedDiv(rowoffset, mScaleY);
	}
}

//===========================================================================
//
//
//
//===========================================================================

fixed_t FTexCoordInfo::TextureOffset(fixed_t textureoffset) const
{
	if (mTempScaleX == FRACUNIT)
	{
		if (mScaleX==FRACUNIT || mWorldPanning) return textureoffset;
		else return FixedDiv(textureoffset, mScaleX);
	}
	else
	{
		if (mWorldPanning) return FixedDiv(textureoffset, mTempScaleX);
		else return FixedDiv(textureoffset, mScaleX);
	}
}

//===========================================================================
//
// Returns the size for which texture offset coordinates are used.
//
//===========================================================================

fixed_t FTexCoordInfo::TextureAdjustWidth() const
{
	if (mWorldPanning) 
	{
		if (mTempScaleX == FRACUNIT) return mRenderWidth;
		else return FixedDiv(mWidth, mTempScaleX);
	}
	else return mWidth;
}



//===========================================================================
//
//
//
//===========================================================================
FGLTexture * FMaterial::ValidateSysTexture(FTexture * tex, bool expand)
{
	if (tex	&& tex->UseType!=FTexture::TEX_Null)
	{
		FGLTexture *gltex = tex->gl_info.SystemTexture;
		if (gltex == NULL) 
		{
			gltex = new FGLTexture(tex, expand);
		}
		return gltex;
	}
	return NULL;
}

//===========================================================================
//
// Constructor
//
//===========================================================================
TArray<FMaterial *> FMaterial::mMaterials;
int FMaterial::mMaxBound;

FMaterial::FMaterial(FTexture * tx, bool forceexpand)
{
	assert(tx->gl_info.Material == NULL);

	bool expanded = tx->UseType == FTexture::TEX_Sprite || 
					tx->UseType == FTexture::TEX_SkinSprite || 
					tx->UseType == FTexture::TEX_Decal ||
					forceexpand;

	mShaderIndex = 0;

	// TODO: apply custom shader object here
	/* if (tx->CustomShaderDefinition)
	{
	}
	else
	*/
	if (tx->bWarped)
	{
		mShaderIndex = tx->bWarped;
		expanded = false;
		tx->gl_info.shaderspeed = static_cast<FWarpTexture*>(tx)->GetSpeed();
	}
	else if (tx->bHasCanvas)
	{
		expanded = false;
	}
	else if (gl.shadermodel > 2) 
	{
		if (tx->gl_info.shaderindex >= FIRST_USER_SHADER)
		{
			mShaderIndex = tx->gl_info.shaderindex;
			expanded = false;
		}
		else
		{
			tx->CreateDefaultBrightmap();
			if (tx->gl_info.Brightmap != NULL)
			{
				ValidateSysTexture(tx->gl_info.Brightmap, expanded);
				FTextureLayer layer = {tx->gl_info.Brightmap, false};
				mTextureLayers.Push(layer);
				mShaderIndex = 3;
			}
		}
	}


	for (int i=GLUSE_PATCH; i<=GLUSE_TEXTURE; i++)
	{
		Width[i] = tx->GetWidth();
		Height[i] = tx->GetHeight();
		LeftOffset[i] = tx->LeftOffset;
		TopOffset[i] = tx->TopOffset;
		RenderWidth[i] = tx->GetScaledWidth();
		RenderHeight[i] = tx->GetScaledHeight();
	}
	Width[GLUSE_SPRITE] = Width[GLUSE_PATCH];
	Height[GLUSE_SPRITE] = Height[GLUSE_PATCH];
	LeftOffset[GLUSE_SPRITE] = LeftOffset[GLUSE_PATCH];
	TopOffset[GLUSE_SPRITE] = TopOffset[GLUSE_PATCH];
	SpriteU[0] = SpriteV[0] = 0;
	spriteright = SpriteU[1] = Width[GLUSE_PATCH] / (float)FHardwareTexture::GetTexDimension(Width[GLUSE_PATCH]);
	spritebottom = SpriteV[1] = Height[GLUSE_PATCH] / (float)FHardwareTexture::GetTexDimension(Height[GLUSE_PATCH]);

	mTextureLayers.ShrinkToFit();
	mMaxBound = -1;
	mMaterials.Push(this);
	tx->gl_info.Material = this;
	if (tx->bHasCanvas) tx->gl_info.mIsTransparent = 0;
	tex = tx;

	tx->gl_info.mExpanded = expanded;
	FTexture *basetex = tx->GetRedirect(gl.shadermodel < 4);
	if (!expanded && !basetex->gl_info.mExpanded && basetex->UseType != FTexture::TEX_Sprite)
	{
		// check if the texture is just a simple redirect to a patch
		// If so we should use the patch for texture creation to
		// avoid eventual redundancies. 
		// This may only be done if both textures use the same expansion mode
		// Redirects to sprites are not permitted because sprites get expanded, however, this won't have been set
		// if the sprite hadn't been used yet.
		mBaseLayer = ValidateSysTexture(basetex, false);
	}
	else if (!expanded)
	{
		// if we got a non-expanded texture that redirects to an expanded one
		mBaseLayer = ValidateSysTexture(tx, false);
	}
	else
	{
		// a little adjustment to make sprites look better with texture filtering:
		// create a 1 pixel wide empty frame around them.
		RenderWidth[GLUSE_PATCH]+=2;
		RenderHeight[GLUSE_PATCH]+=2;
		Width[GLUSE_PATCH]+=2;
		Height[GLUSE_PATCH]+=2;
		LeftOffset[GLUSE_PATCH]+=1;
		TopOffset[GLUSE_PATCH]+=1;
		Width[GLUSE_SPRITE] += 2;
		Height[GLUSE_SPRITE] += 2;
		LeftOffset[GLUSE_SPRITE] += 1;
		TopOffset[GLUSE_SPRITE] += 1;
		spriteright = SpriteU[1] = Width[GLUSE_PATCH] / (float)FHardwareTexture::GetTexDimension(Width[GLUSE_PATCH]);
		spritebottom = SpriteV[1] = Height[GLUSE_PATCH] / (float)FHardwareTexture::GetTexDimension(Height[GLUSE_PATCH]);

		mBaseLayer = ValidateSysTexture(tx, true);

		if (gl.flags & RFL_NPOT_TEXTURE)	// trimming only works if non-power-of-2 textures are supported
		{
			int trim[4];

			if (TrimBorders(trim))
			{
				Width[GLUSE_SPRITE] = trim[2] + 2;
				Height[GLUSE_SPRITE] = trim[3] + 2;
				LeftOffset[GLUSE_SPRITE] -= trim[0];
				TopOffset[GLUSE_SPRITE] -= trim[1];

				SpriteU[0] = SpriteU[1] * (trim[0] / (float)Width[GLUSE_PATCH]);
				SpriteV[0] = SpriteV[1] * (trim[1] / (float)Height[GLUSE_PATCH]);
				SpriteU[1] *= (trim[0]+trim[2]+2) / (float)Width[GLUSE_PATCH]; 
				SpriteV[1] *= (trim[1]+trim[3]+2) / (float)Height[GLUSE_PATCH]; 
			}
		}
	}
}

//===========================================================================
//
// Destructor
//
//===========================================================================

FMaterial::~FMaterial()
{
	for(unsigned i=0;i<mMaterials.Size();i++)
	{
		if (mMaterials[i]==this) 
		{
			mMaterials.Delete(i);
			break;
		}
	}

}


//===========================================================================
// 
//	Finds gaps in the texture which can be skipped by the renderer
//  This was mainly added to speed up one area in E4M6 of 007LTSD
//
//===========================================================================

bool FMaterial::TrimBorders(int *rect)
{
	PalEntry col;
	int w;
	int h;

	unsigned char *buffer = CreateTexBuffer(CM_DEFAULT, 0, w, h);

	if (buffer == NULL) 
	{
		return false;
	}
	if (w != Width[GLUSE_TEXTURE] || h != Height[GLUSE_TEXTURE])
	{
		// external Hires replacements cannot be trimmed.
		delete [] buffer;
		return false;
	}

	int size = w*h;

	int first, last;

	for(first = 0; first < size; first++)
	{
		if (buffer[first*4+3] != 0) break;
	}
	if (first >= size)
	{
		rect[0] = 0;
		rect[1] = 0;
		rect[2] = 1;
		rect[3] = 1;
		delete [] buffer;
		return true;
	}

	for(last = size-1; last >= first; last--)
	{
		if (buffer[last*4+3] != 0) break;
	}

	rect[1] = first / w;
	rect[3] = 1 + last/w - rect[1];

	rect[0] = 0;
	rect[2] = w;

	unsigned char *bufferoff = buffer + (rect[1] * w * 4);
	h = rect[3];

	for(int x = 0; x < w; x++)
	{
		for(int y = 0; y < h; y++)
		{
			if (bufferoff[(x+y*w)*4+3] != 0) goto outl;
		}
		rect[0]++;
	}
outl:
	rect[2] -= rect[0];

	for(int x = w-1; rect[2] > 1; x--)
	{
		for(int y = 0; y < h; y++)
		{
			if (bufferoff[(x+y*w)*4+3] != 0) 
			{
				delete [] buffer;
				return true;
			}
		}
		rect[2]--;
	}
	delete [] buffer;
	return true;
}


//===========================================================================
// 
//	Binds a texture to the renderer
//
//===========================================================================

void FMaterial::Bind(int cm, int clampmode, int translation, int overrideshader)
{
	int usebright = false;
	int shaderindex = overrideshader > 0? overrideshader : mShaderIndex;
	int maxbound = 0;
	bool allowhires = tex->xScale == FRACUNIT && tex->yScale == FRACUNIT;

	int softwarewarp = gl_RenderState.SetupShader(tex->bHasCanvas, shaderindex, cm, tex->gl_info.shaderspeed);

	if (tex->bHasCanvas || tex->bWarped) clampmode = 0;
	else if (clampmode != -1) clampmode &= 3;
	else clampmode = 4;

	const FHardwareTexture *gltexture = mBaseLayer->Bind(0, cm, clampmode, translation, allowhires? tex:NULL, softwarewarp);
	if (gltexture != NULL && shaderindex > 0 && overrideshader == 0)
	{
		for(unsigned i=0;i<mTextureLayers.Size();i++)
		{
			FTexture *layer;
			if (mTextureLayers[i].animated)
			{
				FTextureID id = mTextureLayers[i].texture->GetID();
				layer = TexMan(id);
				ValidateSysTexture(layer, false);
			}
			else
			{
				layer = mTextureLayers[i].texture;
			}
			layer->gl_info.SystemTexture->Bind(i+1, CM_DEFAULT, clampmode, 0, NULL, false);
			maxbound = i+1;
		}
	}
	// unbind everything from the last texture that's still active
	for(int i=maxbound+1; i<=mMaxBound;i++)
	{
		FHardwareTexture::Unbind(i);
		mMaxBound = maxbound;
	}
}


//===========================================================================
// 
//	Binds a texture to the renderer
//
//===========================================================================

void FMaterial::BindPatch(int cm, int translation, int overrideshader)
{
	int usebright = false;
	int shaderindex = overrideshader > 0? overrideshader : mShaderIndex;
	int maxbound = 0;

	int softwarewarp = gl_RenderState.SetupShader(tex->bHasCanvas, shaderindex, cm, tex->gl_info.shaderspeed);

	const FHardwareTexture *glpatch = mBaseLayer->BindPatch(0, cm, translation, softwarewarp);
	// The only multitexture effect usable on sprites is the brightmap.
	if (glpatch != NULL && shaderindex == 3)
	{
		mTextureLayers[0].texture->gl_info.SystemTexture->BindPatch(1, CM_DEFAULT, 0, 0);
		maxbound = 1;
	}
	// unbind everything from the last texture that's still active
	for(int i=maxbound+1; i<=mMaxBound;i++)
	{
		FHardwareTexture::Unbind(i);
		mMaxBound = maxbound;
	}
}


//===========================================================================
//
//
//
//===========================================================================
void FMaterial::Precache()
{
	if (tex->UseType==FTexture::TEX_Sprite) 
	{
		BindPatch(CM_DEFAULT, 0);
	}
	else 
	{
		int cached = 0;
		for(int i=0;i<4;i++)
		{
			if (mBaseLayer->gltexture[i] != 0)
			{
				Bind (CM_DEFAULT, i, 0);
				cached++;
			}
			if (cached == 0) Bind(CM_DEFAULT, -1, 0);
		}
	}
}

//===========================================================================
//
// This function is needed here to temporarily manipulate the texture
// for per-wall scaling so that the coordinate functions return proper
// results. Doing this here is much easier than having the calling code
// make these calculations.
//
//===========================================================================

void FMaterial::GetTexCoordInfo(FTexCoordInfo *tci, fixed_t x, fixed_t y) const
{
	if (x == FRACUNIT)
	{
		tci->mRenderWidth = RenderWidth[GLUSE_TEXTURE];
		tci->mScaleX = tex->xScale;
		tci->mTempScaleX = FRACUNIT;
	}
	else
	{
		fixed_t scale_x = FixedMul(x, tex->xScale);
		int foo = (Width[GLUSE_TEXTURE] << 17) / scale_x; 
		tci->mRenderWidth = (foo >> 1) + (foo & 1); 
		tci->mScaleX = scale_x;
		tci->mTempScaleX = x;
	}

	if (y == FRACUNIT)
	{
		tci->mRenderHeight = RenderHeight[GLUSE_TEXTURE];
		tci->mScaleY = tex->yScale;
		tci->mTempScaleY = FRACUNIT;
	}
	else
	{
		fixed_t scale_y = FixedMul(y, tex->yScale);
		int foo = (Height[GLUSE_TEXTURE] << 17) / scale_y; 
		tci->mRenderHeight = (foo >> 1) + (foo & 1); 
		tci->mScaleY = scale_y;
		tci->mTempScaleY = y;
	}
	if (tex->bHasCanvas) 
	{
		tci->mScaleY = -tci->mScaleY;
		tci->mRenderHeight = -tci->mRenderHeight;
	}
	tci->mWorldPanning = tex->bWorldPanning;
	tci->mWidth = Width[GLUSE_TEXTURE];
}

//===========================================================================
//
//
//
//===========================================================================

int FMaterial::GetAreas(FloatRect **pAreas) const
{
	if (mShaderIndex == 0)	// texture splitting can only be done if there's no attached effects
	{
		FTexture *tex = mBaseLayer->tex;
		*pAreas = tex->gl_info.areas;
		return tex->gl_info.areacount;
	}
	else
	{
		return 0;
	}
}

//===========================================================================
//
//
//
//===========================================================================

void FMaterial::BindToFrameBuffer()
{
	if (mBaseLayer->gltexture[0] == NULL)
	{
		// must create the hardware texture first
		mBaseLayer->Bind(0, CM_DEFAULT, 0, 0, NULL, 0);
		FHardwareTexture::Unbind(0);
	}
	mBaseLayer->gltexture[0]->BindToFrameBuffer();
}

//===========================================================================
//
// GetRect
//
//===========================================================================

void FMaterial::GetRect(FloatRect * r, ETexUse i) const
{
	r->left = -GetScaledLeftOffsetFloat(i);
	r->top = -GetScaledTopOffsetFloat(i);
	r->width = GetScaledWidthFloat(i);
	r->height = GetScaledHeightFloat(i);
}


//==========================================================================
//
// Gets a texture from the texture manager and checks its validity for
// GL rendering. 
//
//==========================================================================

FMaterial * FMaterial::ValidateTexture(FTexture * tex)
{
	if (tex	&& tex->UseType!=FTexture::TEX_Null)
	{
		FMaterial *gltex = tex->gl_info.Material;
		if (gltex == NULL) 
		{
			//@sync-tex
			gltex = new FMaterial(tex, false);
		}
		return gltex;
	}
	return NULL;
}

FMaterial * FMaterial::ValidateTexture(FTextureID no, bool translate)
{
	return ValidateTexture(translate? TexMan(no) : TexMan[no]);
}


//==========================================================================
//
// Flushes all hardware dependent data
//
//==========================================================================

#ifdef __vita__
// [Vita] HUD graphics and font characters stay loaded across level changes:
// building them again (each text colour separately) made the first HUD
// messages of every level take up to 0.6 s.
static bool Vita_KeepOnFlush(FTexture *tex)
{
	if (tex == NULL) return false;
	if (tex->UseType != FTexture::TEX_FontChar && tex->UseType != FTexture::TEX_MiscPatch) return false;
	return tex->GetWidth() * tex->GetHeight() <= 256 * 256;
}
#endif

void FMaterial::FlushAll()
{
	for(int i=mMaterials.Size()-1;i>=0;i--)
	{
#ifdef __vita__
		if (Vita_KeepOnFlush(mMaterials[i]->tex)) continue;
#endif
		mMaterials[i]->Clean(true);
	}
	// This is for shader layers. All shader layers must be managed by the texture manager
	// so this will catch everything.
	for(int i=TexMan.NumTextures()-1;i>=0;i--)
	{
		FGLTexture *gltex = TexMan.ByIndex(i)->gl_info.SystemTexture;
#ifdef __vita__
		if (Vita_KeepOnFlush(TexMan.ByIndex(i))) continue;
#endif
		if (gltex != NULL) gltex->Clean(true);
	}
}

//==========================================================================
//
// Prints some texture info
//
//==========================================================================

int FGLTexture::Dump(int i)
{
	int cnt = 0;
	int lump = tex->GetSourceLump();
	Printf(PRINT_LOG, "Texture '%s' (Index %d, Lump %d, Name '%s'):\n", tex->Name, i, lump, Wads.GetLumpFullName(lump));
	if (hirestexture) Printf(PRINT_LOG, "\tHirestexture\n");
	if (glpatch) Printf(PRINT_LOG, "\tPatch\n"),cnt++;
	if (gltexture[0]) Printf(PRINT_LOG, "\tTexture (x:no,  y:no )\n"),cnt++;
	if (gltexture[1]) Printf(PRINT_LOG, "\tTexture (x:yes, y:no )\n"),cnt++;
	if (gltexture[2]) Printf(PRINT_LOG, "\tTexture (x:no,  y:yes)\n"),cnt++;
	if (gltexture[3]) Printf(PRINT_LOG, "\tTexture (x:yes, y:yes)\n"),cnt++;
	if (gltexture[4]) Printf(PRINT_LOG, "\tTexture precache\n"),cnt++;
	return cnt;
}

CCMD(textureinfo)
{
	int cnth = 0, cntt = 0, pix = 0;
	for(int i=0; i<TexMan.NumTextures(); i++)
	{
		FTexture *tex = TexMan.ByIndex(i);
		FGLTexture *systex = tex->gl_info.SystemTexture;
		if (systex != NULL) 
		{
			int cnt = systex->Dump(i);
			cnth+=cnt;
			cntt++;
			pix += cnt * tex->GetWidth() * tex->GetHeight();
		}
	}
	Printf(PRINT_LOG, "%d system textures, %d hardware textures, %d pixels\n", cntt, cnth, pix);
}


#ifdef __vita__
#include "v_font.h"
#include "textures/textures.h"
#include "vita/vita_system.h"
#include <psp2/kernel/processmgr.h>
static TMap<FTexture *, bool> Vita_HudDone;
struct FVitaHudPre { unsigned long long deadline; int done, total; bool timeout; };
static void Vita_PreHudOne(FTexture *tex, void *data)
{
	FVitaHudPre *p = (FVitaHudPre *)data;
	p->total++;
	if (Vita_HudDone.CheckKey(tex) != NULL) return;
	if (sceKernelGetProcessTimeWide() > p->deadline) { p->timeout = true; return; }
	if (tex->GetWidth() * tex->GetHeight() > 512 * 512 || tex->UseType == FTexture::TEX_Null) return;
	FMaterial *mat = FMaterial::ValidateTexture(tex);
	if (mat == NULL) return;
	mat->BindPatch(CM_DEFAULT, 0);
	Vita_HudDone[tex] = true;
	p->done++;
	extern void Vita_LoadProgress(const char *, int, int, bool);
	if ((p->done & 15) == 0) Vita_LoadProgress("HUD", p->done, p->done + 1, false);
}

void Vita_PrecacheHud(FTextureManager *tm)
{
	// fonts first, then HUD pictures up to 256x256 (uploaded uncompressed,
	// no cache files), 2 s at most per level start
	FVitaHudPre p = { sceKernelGetProcessTimeWide() + 2000000ull, 0, 0, false };
	const unsigned long long t0 = sceKernelGetProcessTimeWide();
	FFont::Vita_ForEachChar(Vita_PreHudOne, &p);
	// coloured text: the colours used before (2 s more at most)
	{
		struct FCol { unsigned long long deadline; int done; bool timeout; } c = { sceKernelGetProcessTimeWide() + 2000000ull, 0, false };
		static TMap<QWORD, bool> doneCol;
		FFont::Vita_ForEachColoredChar([](FTexture *pic, int translation, void *data) {
			FCol *c = (FCol *)data;
			const QWORD key = ((QWORD)(size_t)pic << 16) ^ (QWORD)(unsigned)translation;
			if (doneCol.CheckKey(key) != NULL) return;
			if (sceKernelGetProcessTimeWide() > c->deadline) { c->timeout = true; return; }
			FMaterial *mat = FMaterial::ValidateTexture(pic);
			if (mat == NULL) return;
			mat->BindPatch(CM_DEFAULT, translation);
			doneCol[key] = true;
			c->done++;
		}, &c);
		Vita_BootLog("precached %d coloured font characters%s", c.done, c.timeout ? " (time limit)" : "");
	}
	for (int i = 0; i < tm->NumTextures(); i++)
	{
		FTexture *tex = tm->ByIndex(i);
		if (tex != NULL && tex->UseType == FTexture::TEX_MiscPatch && tex->GetWidth() * tex->GetHeight() <= 256 * 256) Vita_PreHudOne(tex, &p);
	}
	Vita_BootLog("precached %d HUD graphics and font characters in %llu ms (%d known%s)", p.done,
		(sceKernelGetProcessTimeWide() - t0) / 1000, p.total, p.timeout ? ", rest left for later: time limit" : "");
}
#endif
