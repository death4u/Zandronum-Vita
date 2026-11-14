// [Vita] Batching of solid walls.
//
// Each wall used to be its own glBegin/glEnd, i.e. its own vitaGL draw call,
// and in big maps those calls were the bulk of the 3D time. The draw lists
// are sorted by texture and light level, so consecutive walls usually share
// all their state; GLWall::Draw (plain pass) now collects such runs and draws
// them with one glDrawArrays. RenderWall and the edge-splitting code emit
// their vertices through the wrappers below, which either pass them straight
// to GL or, while collecting, add them to the batch.
#ifndef GL_VITAWALLBATCH_H
#define GL_VITAWALLBATCH_H
#ifdef __vita__

#include "tarray.h"
#include "gl/renderer/gl_colormap.h"
class FMaterial;

struct FVitaWallBatch
{
	bool Collecting;		// vertex calls go to the batch
	bool Open;				// a run with the key below is pending
	FMaterial *Tex;
	FColormap Cm;
	int LightLevel, Rel, Flags;
	bool NoFog;
	TArray<float> Xyz, Uv;	// triangles
	TArray<float> FanXyz, FanUv;
	float U, V;
};
extern FVitaWallBatch VitaWalls;
void Vita_FlushWalls();

static inline void VitaGL_Begin(GLenum mode)
{
	if (VitaWalls.Collecting) { VitaWalls.FanXyz.Clear(); VitaWalls.FanUv.Clear(); }
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
			VitaWalls.Xyz.Push(VitaWalls.FanXyz[idx[j] * 3]);
			VitaWalls.Xyz.Push(VitaWalls.FanXyz[idx[j] * 3 + 1]);
			VitaWalls.Xyz.Push(VitaWalls.FanXyz[idx[j] * 3 + 2]);
			VitaWalls.Uv.Push(VitaWalls.FanUv[idx[j] * 2]);
			VitaWalls.Uv.Push(VitaWalls.FanUv[idx[j] * 2 + 1]);
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
