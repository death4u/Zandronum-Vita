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
	bool LightTex;			// light pass: Uv = surface texture (unit 0), Uv2 = light texture (unit 1)
	TArray<float> Xyz, Uv, Uv2;	// triangles
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
	// dynamic lights of the wall being added (vita_vertexlights): its
	// polygons are split and lit per vertex
	struct FLightNode *LightHead[2];
	bool ForceNoFog;		// lit polygon: fog baked into its vertex colours
	// Light pass with the surface texture (vita_lighttexpass): the vertex
	// texture coordinates are the light's; the surface's are worked out
	// from the position. 1 = wall (Wall* below), 2 = flat (Flat*).
	int LightPassMode;
	float WallX1, WallY1, WallDX, WallDY, WallInvLen2, WallZTop[2], WallZBot[2];
	float WallUTop[2], WallVTop[2], WallUBot[2], WallVBot[2];
	float FlatA, FlatB, FlatC, FlatD, FlatE, FlatF;
};
extern unsigned Vita_LightTexId;	// GL texture of the light pass (gllight.png)
extern FVitaWallBatch VitaWalls;
extern unsigned Vita_CurColor;	// last glColor (linker wrappers in vita_perf.cpp)
void Vita_FlushWalls();
// Sets the key of the next surface: texture to bind (NULL: none), its
// colormap and clamp flags, and its colour (UseCurColor: last glColor).
void Vita_BatchSurface(FMaterial *tex, int cm, int flags, bool usecurcolor, float r = 1, float g = 1, float b = 1);
// Starts or continues a run with the surface's key and the fog and blend
// equation currently in gl_RenderState.
void Vita_BatchCheck();

// [Vita] Dynamic lights per vertex (vita_vertexlights, 0039): the lights
// touching a surface are added to its vertex colours in the normal batched
// pass (the multipass lights drew every lit surface 2 + N times). Big
// polygons near a light are split so the light keeps a round-ish shape.
bool Vita_VertexLightsOn();
// Sets up the lights of one polygon (lists of a side or subsector); p0..p2
// span its plane (GL coordinates). Returns false if none of them reach it.
bool Vita_SetupPolyLights(struct FLightNode *head0, struct FLightNode *head1, const float *p0, const float *p1, const float *p2);
// Back to the surface's own bucket after the lit triangles.
void Vita_EndPolyLights();
// Surface cache (gl_vitasurfcache.cpp, 0047)
bool Vita_SurfTry(const void *a, const void *b, unsigned kind, unsigned sig);
void Vita_SurfNote(FVitaBucket *b, unsigned first, unsigned count, bool uv2);
void Vita_SurfBad();
void Vita_SurfEnd();

// Adds a lit triangle to the current bucket (after Vita_SetupPolyLights).
void Vita_EmitLitTriangle(const float *p0, const float *p1, const float *p2, const float *uv0, const float *uv1, const float *uv2);

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
	if ((VitaWalls.LightHead[0] != NULL || VitaWalls.LightHead[1] != NULL) && n >= 3)
	{
		const float *f = &VitaWalls.FanXyz[0], *t = &VitaWalls.FanUv[0];
		// plane from the corners farthest apart (fans can have split points)
		if (Vita_SetupPolyLights(VitaWalls.LightHead[0], VitaWalls.LightHead[1], f, f + 3 * (n / 2), f + 3 * (n - 1)))
		{
			Vita_SurfBad();
			for (unsigned k = 1; k + 1 < n; k++)
				Vita_EmitLitTriangle(f, f + 3 * k, f + 3 * (k + 1), t, t + 2 * k, t + 2 * (k + 1));
			Vita_EndPolyLights();
			return;
		}
	}
	if (VitaWalls.LightPassMode != 0)
	{
		extern void Vita_EmitLightPassFan();
		Vita_EmitLightPassFan();
		return;
	}
	if (n < 3) return;
	// reserve once, then write (TArray::Push per float was a noticeable cost)
	FVitaBucket *b = VitaWalls.Cur;
	const unsigned tris = n - 2;
	const unsigned vfirst = b->Rgba.Size();
	float *xyz = &b->Xyz[b->Xyz.Reserve(tris * 9)];
	float *uv = &b->Uv[b->Uv.Reserve(tris * 6)];
	unsigned *rgba = &b->Rgba[b->Rgba.Reserve(tris * 3)];
	const float *fx = &VitaWalls.FanXyz[0], *fu = &VitaWalls.FanUv[0];
	const unsigned color = VitaWalls.Color;
	for (unsigned k = 1; k + 1 < n; k++)
	{
		const unsigned idx[3] = { 0, k, k + 1 };
		for (int j = 0; j < 3; j++)
		{
			const float *p = fx + idx[j] * 3, *t = fu + idx[j] * 2;
			xyz[0] = p[0]; xyz[1] = p[1]; xyz[2] = p[2]; xyz += 3;
			uv[0] = t[0]; uv[1] = t[1]; uv += 2;
			*rgba++ = color;
		}
	}
	Vita_SurfNote(b, vfirst, tris * 3, false);
}

#define glBegin VitaGL_Begin
#define glTexCoord2f VitaGL_TexCoord2f
#define glVertex3f VitaGL_Vertex3f
#define glEnd VitaGL_End

#else
#define Vita_FlushWalls()
#endif
#endif
