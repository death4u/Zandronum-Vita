// [Vita] Batching of walls and flats.
//
// Each wall or flat used to be its own vitaGL draw call (thousands per frame
// in big maps, the bulk of the 3D time). While a draw list is drawn, surfaces
// are collected into one triangle list with a colour per vertex (so different
// light levels share a batch) and drawn with one glDrawArrays whenever the
// texture, the fog or the blend equation changes. This is used for the
// single-pass rendering and for all passes of the dynamic light rendering.
// RenderWall, DrawSubsectorLights and the edge-splitting code emit their
// vertices through the wrappers below, which either pass them straight to GL
// or, while collecting, add them to the batch.
#ifndef GL_VITAWALLBATCH_H
#define GL_VITAWALLBATCH_H
#ifdef __vita__

#include "tarray.h"
#include "gl/renderer/gl_colormap.h"
class FMaterial;

// One bucket per state (texture, fog, blend equation). Within a draw list
// the order of opaque surfaces doesn't matter (depth buffer), so all
// surfaces with the same state are drawn with one call when the list ends
// (0036 on the Vita: up to 1800 draw calls a frame, mostly because the fog
// density changes with every sector's light level).
struct FVitaBucket
{
	FMaterial *Tex;			// bound when drawn (NULL: caller's texture state)
	int CmColormap, Flags;
	bool FogOn;
	PalEntry FogColor;
	float FogDensity;
	int BlendEq;
	TArray<float> Xyz, Uv;	// triangles
	TArray<unsigned> Rgba;	// one colour per vertex
};

struct FVitaWallBatch
{
	bool Collecting;		// vertex calls go to the batch
	bool Open;				// buckets hold surfaces
	FVitaBucket *Cur;		// bucket of the surface being added
	// key of the surface being added (set by the caller)
	FMaterial *NextTex;
	int NextCm, NextFlags;
	bool UseCurColor;		// take the colour from the last glColor call
	unsigned NextColor;
	TArray<float> FanXyz, FanUv;
	float U, V;
	unsigned Color;			// colour of the surface being added
};
extern FVitaWallBatch VitaWalls;
extern unsigned Vita_CurColor;	// last glColor (linker wrappers in vita_perf.cpp)
void Vita_FlushWalls();
// Sets the key of the next surface: texture to bind (NULL: none), its
// colormap and clamp flags, and its colour (UseCurColor: last glColor).
void Vita_BatchSurface(FMaterial *tex, int cm, int flags, bool usecurcolor, float r = 1, float g = 1, float b = 1);
// Starts or continues a run with the surface's key and the fog and blend
// equation currently in gl_RenderState.
void Vita_BatchCheck();

static inline void VitaGL_Begin(GLenum mode)
{
	if (VitaWalls.Collecting) { Vita_BatchCheck(); VitaWalls.FanXyz.Clear(); VitaWalls.FanUv.Clear(); }
	else glBegin(mode);
}
static inline void VitaGL_TexCoord2f(float u, float v)
{
	if (VitaWalls.Collecting) { VitaWalls.U = u; VitaWalls.V = v; }
	else glTexCoord2f(u, v);
}
static inline void VitaGL_Vertex3f(float x, float y, float z)
{
	if (VitaWalls.Collecting)
	{
		VitaWalls.FanXyz.Push(x); VitaWalls.FanXyz.Push(y); VitaWalls.FanXyz.Push(z);
		VitaWalls.FanUv.Push(VitaWalls.U); VitaWalls.FanUv.Push(VitaWalls.V);
	}
	else glVertex3f(x, y, z);
}
static inline void VitaGL_End()
{
	if (!VitaWalls.Collecting) { glEnd(); return; }
	// triangle fan -> triangles
	unsigned n = VitaWalls.FanXyz.Size() / 3;
	for (unsigned k = 1; k + 1 < n; k++)
	{
		unsigned idx[3] = { 0, k, k + 1 };
		for (int j = 0; j < 3; j++)
		{
			FVitaBucket *b = VitaWalls.Cur;
			b->Xyz.Push(VitaWalls.FanXyz[idx[j] * 3]);
			b->Xyz.Push(VitaWalls.FanXyz[idx[j] * 3 + 1]);
			b->Xyz.Push(VitaWalls.FanXyz[idx[j] * 3 + 2]);
			b->Uv.Push(VitaWalls.FanUv[idx[j] * 2]);
			b->Uv.Push(VitaWalls.FanUv[idx[j] * 2 + 1]);
			b->Rgba.Push(VitaWalls.Color);
		}
	}
}

#define glBegin VitaGL_Begin
#define glTexCoord2f VitaGL_TexCoord2f
#define glVertex3f VitaGL_Vertex3f
#define glEnd VitaGL_End

#else
#define Vita_FlushWalls()
#endif
#endif
