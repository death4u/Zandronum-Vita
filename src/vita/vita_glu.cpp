// [Vita] The GLU polygon tesselator the GL renderer needs (vitaGL already
// has gluScaleImage), implemented on top of libtess2 (SGI's tesselator).

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <GL/glu.h>
#include "libtess2/tesselator.h"

// ---------------------------------------------------------------------------
// Tesselator: collects contours, runs libtess2 with GLU's default
// (odd winding) rule and reports triangles through the GLU callbacks.
// ---------------------------------------------------------------------------

struct GLUtesselator
{
	TESStesselator *tess;
	void *polydata;
	float *verts; void **vdata; int nverts, capverts;
	int contourStart;
	void (*beginData)(GLenum, void *);
	void (*vertexData)(void *, void *);
	void (*endData)(void *);
	void (*errorData)(GLenum, void *);
	void (*begin)(GLenum);
	void (*vertex)(void *);
	void (*end)(void);
	void (*error)(GLenum);
	// all original vertices of the polygon, to map new (intersection) vertices back
	float *allverts; void **alldata; int nall, capall;
};

extern "C" GLUtesselator *gluNewTess(void)
{
	GLUtesselator *t = (GLUtesselator *)calloc(1, sizeof(GLUtesselator));
	return t;
}

extern "C" void gluDeleteTess(GLUtesselator *t)
{
	if (!t) return;
	if (t->tess) tessDeleteTess(t->tess);
	free(t->verts); free(t->vdata); free(t->allverts); free(t->alldata);
	free(t);
}

extern "C" void gluTessCallback(GLUtesselator *t, GLenum which, _GLUfuncptr fn)
{
	switch (which)
	{
	case GLU_TESS_BEGIN_DATA:  t->beginData = (void (*)(GLenum, void *))fn; break;
	case GLU_TESS_VERTEX_DATA: t->vertexData = (void (*)(void *, void *))fn; break;
	case GLU_TESS_END_DATA:    t->endData = (void (*)(void *))fn; break;
	case GLU_TESS_ERROR_DATA:  t->errorData = (void (*)(GLenum, void *))fn; break;
	case GLU_TESS_BEGIN:       t->begin = (void (*)(GLenum))fn; break;
	case GLU_TESS_VERTEX:      t->vertex = (void (*)(void *))fn; break;
	case GLU_TESS_END:         t->end = (void (*)(void))fn; break;
	case GLU_TESS_ERROR:       t->error = (void (*)(GLenum))fn; break;
	default: break;	// combine: new vertices are mapped to the nearest input vertex
	}
}

extern "C" void gluTessBeginPolygon(GLUtesselator *t, GLvoid *data)
{
	if (t->tess) tessDeleteTess(t->tess);
	t->tess = tessNewTess(NULL);
	t->polydata = data;
	t->nall = 0;
}

extern "C" void gluTessBeginContour(GLUtesselator *t)
{
	t->nverts = 0;
}

extern "C" void gluTessVertex(GLUtesselator *t, GLdouble *loc, GLvoid *data)
{
	if (t->nverts == t->capverts)
	{
		t->capverts = t->capverts ? t->capverts * 2 : 64;
		t->verts = (float *)realloc(t->verts, sizeof(float) * 3 * t->capverts);
		t->vdata = (void **)realloc(t->vdata, sizeof(void *) * t->capverts);
	}
	t->verts[t->nverts * 3 + 0] = (float)loc[0];
	t->verts[t->nverts * 3 + 1] = (float)loc[1];
	t->verts[t->nverts * 3 + 2] = (float)loc[2];
	t->vdata[t->nverts] = data;
	t->nverts++;

	if (t->nall == t->capall)
	{
		t->capall = t->capall ? t->capall * 2 : 64;
		t->allverts = (float *)realloc(t->allverts, sizeof(float) * 3 * t->capall);
		t->alldata = (void **)realloc(t->alldata, sizeof(void *) * t->capall);
	}
	memcpy(t->allverts + t->nall * 3, t->verts + (t->nverts - 1) * 3, sizeof(float) * 3);
	t->alldata[t->nall] = data;
	t->nall++;
}

extern "C" void gluTessEndContour(GLUtesselator *t)
{
	if (t->tess && t->nverts > 0)
		tessAddContour(t->tess, 3, t->verts, sizeof(float) * 3, t->nverts);
}

static void *NearestData(GLUtesselator *t, const float *v)
{
	int best = 0; float bestd = 1e30f;
	for (int i = 0; i < t->nall; ++i)
	{
		const float *p = t->allverts + i * 3;
		float dx = p[0] - v[0], dy = p[1] - v[1], dz = p[2] - v[2];
		float d = dx * dx + dy * dy + dz * dz;
		if (d < bestd) { bestd = d; best = i; }
	}
	return t->nall ? t->alldata[best] : NULL;
}

extern "C" void gluTessEndPolygon(GLUtesselator *t)
{
	if (!t->tess) return;
	if (!tessTesselate(t->tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 3, NULL))
	{
		if (t->errorData) t->errorData(100151 /*GLU_TESS_ERROR*/, t->polydata);
		else if (t->error) t->error(100151);
		return;
	}
	const int nelems = tessGetElementCount(t->tess);
	const TESSindex *elems = tessGetElements(t->tess);
	const TESSreal *verts = tessGetVertices(t->tess);
	const TESSindex *vidx = tessGetVertexIndices(t->tess);
	if (nelems <= 0) return;

	if (t->beginData) t->beginData(GL_TRIANGLES, t->polydata);
	else if (t->begin) t->begin(GL_TRIANGLES);
	for (int i = 0; i < nelems; ++i)
	{
		for (int j = 0; j < 3; ++j)
		{
			TESSindex v = elems[i * 3 + j];
			if (v == TESS_UNDEF) continue;
			void *data = (vidx[v] != TESS_UNDEF && vidx[v] < t->nall) ? t->alldata[vidx[v]] : NearestData(t, verts + v * 3);
			if (t->vertexData) t->vertexData(data, t->polydata);
			else if (t->vertex) t->vertex(data);
		}
	}
	if (t->endData) t->endData(t->polydata);
	else if (t->end) t->end();
}
