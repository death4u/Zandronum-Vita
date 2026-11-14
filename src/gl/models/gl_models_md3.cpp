/*
** gl_models_md3.cpp
**
**---------------------------------------------------------------------------
** Copyright 2006 Christoph Oelckers
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
#include "cmdlib.h"
#include "sc_man.h"
#include "m_crc32.h"

#include "gl/renderer/gl_renderstate.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/models/gl_models.h"
#include "gl/textures/gl_material.h"
#include "gl/shaders/gl_shader.h"

#define MAX_QPATH 64




#ifdef __vita__
// [Vita] Packs one frame of a surface (x, y, z int16 triplets) to 11:11:10 bits.
static void MD3PackFrame(FMD3Model::MD3Surface *s, int frame, const short *xyz)
{
	int nv = s->numVertices;
	float mn[3] = { 32767, 32767, 32767 }, mx[3] = { -32768, -32768, -32768 };
	for (int i = 0; i < nv; i++)
		for (int c = 0; c < 3; c++)
		{
			float v = xyz[i * 3 + c];
			if (v < mn[c]) mn[c] = v;
			if (v > mx[c]) mx[c] = v;
		}
	static const int levels[3] = { 2047, 2047, 1023 };
	float inv[3];
	for (int c = 0; c < 3; c++)
	{
		if (nv == 0) { mn[c] = 0; mx[c] = 0; }
		float range = mx[c] - mn[c];
		s->pmin[frame * 3 + c] = mn[c];
		s->pscale[frame * 3 + c] = range > 0 ? range / levels[c] : 0;
		inv[c] = range > 0 ? levels[c] / range : 0;
	}
	unsigned *out = s->packed + frame * nv;
	for (int i = 0; i < nv; i++)
	{
		unsigned q[3];
		for (int c = 0; c < 3; c++)
		{
			int v = (int)((xyz[i * 3 + c] - mn[c]) * inv[c] + 0.5f);
			q[c] = (unsigned)clamp(v, 0, levels[c]);
		}
		out[i] = q[0] | (q[1] << 11) | (q[2] << 22);
	}
}

static void MD3AllocPacked(FMD3Model::MD3Surface *s, int frames)
{
	s->packed = new unsigned[s->numVertices * frames];
	s->pmin = new float[frames * 3];
	s->pscale = new float[frames * 3];
}
#endif

bool FMD3Model::Load(const char * path, int, const char * buffer, int length)
{
	#pragma pack(4)
	struct md3_header_t
	{
		DWORD Magic;
		DWORD Version;
		char Name[MAX_QPATH];
		DWORD Flags;
		DWORD Num_Frames;
		DWORD Num_Tags;
		DWORD Num_Surfaces;
		DWORD Num_Skins;
		DWORD Ofs_Frames;
		DWORD Ofs_Tags;
		DWORD Ofs_Surfaces;
		DWORD Ofs_Eof;
	};

	struct md3_surface_t
	{
		DWORD Magic;
		char Name[MAX_QPATH];
		DWORD Flags;
		DWORD Num_Frames;
		DWORD Num_Shaders;
		DWORD Num_Verts;
		DWORD Num_Triangles;
		DWORD Ofs_Triangles;
		DWORD Ofs_Shaders;
		DWORD Ofs_Texcoord;
		DWORD Ofs_XYZNormal;
		DWORD Ofs_End;
	};

	struct md3_triangle_t
	{
		DWORD vt_index[3];
	};

	struct md3_shader_t
	{
		char Name[MAX_QPATH];
		DWORD index;
	};

	struct md3_texcoord_t
	{
		float s,t;
	};

	struct md3_vertex_t
	{
		short x,y,z,n;
	};

	struct md3_frame_t
	{
		float min_Bounds[3];
		float max_Bounds[3];
		float localorigin[3];
		float radius;
		char Name[16];
	};
	#pragma pack()

	md3_header_t * hdr=(md3_header_t *)buffer;

	numFrames = LittleLong(hdr->Num_Frames);
	numTags = LittleLong(hdr->Num_Tags);
	numSurfaces = LittleLong(hdr->Num_Surfaces);

	md3_frame_t * frm = (md3_frame_t*)(buffer + LittleLong(hdr->Ofs_Frames));

	frames = new MD3Frame[numFrames];
	for(int i=0;i<numFrames;i++)
	{
		strncpy(frames[i].Name, frm[i].Name, 16);
		for(int j=0;j<3;j++) frames[i].origin[j] = frm[i].localorigin[j];
	}

	md3_surface_t * surf = (md3_surface_t*)(buffer + LittleLong(hdr->Ofs_Surfaces));

	surfaces = new MD3Surface[numSurfaces];
	for(int i=0;i<numSurfaces;i++)
	{
		MD3Surface * s = &surfaces[i];
		md3_surface_t * ss = surf;

		surf = (md3_surface_t *)(((char*)surf) + LittleLong(surf->Ofs_End));

		s->numSkins = LittleLong(ss->Num_Shaders);
		s->numTriangles = LittleLong(ss->Num_Triangles);
		s->numVertices = LittleLong(ss->Num_Verts);

		// copy triangle indices
		md3_triangle_t * tris = (md3_triangle_t*)(((char*)ss)+LittleLong(ss->Ofs_Triangles));
		s->tris = new MD3Triangle[s->numTriangles];

		for(int i=0;i<s->numTriangles;i++) for (int j=0;j<3;j++)
		{
			s->tris[i].VertIndex[j]=LittleLong(tris[i].vt_index[j]);
		}
#ifdef __vita__
		if (s->numVertices <= 65535)
		{
			s->indices16 = new unsigned short[s->numTriangles * 3];
			for (int i = 0; i < s->numTriangles; i++) for (int j = 0; j < 3; j++)
				s->indices16[i * 3 + j] = (unsigned short)s->tris[i].VertIndex[j];
		}
#endif

		// copy shaders (skins)
		md3_shader_t * shader = (md3_shader_t*)(((char*)ss)+LittleLong(ss->Ofs_Shaders));
		s->skins = new FTexture *[s->numSkins];

		for(int i=0;i<s->numSkins;i++)
		{
			// [BB] According to the MD3 spec, Name is supposed to include the full path.
			s->skins[i] = LoadSkin("", shader[i].Name);
			// [BB] Fall back and check if Name is relative.
			if ( s->skins[i] == NULL )
				s->skins[i] = LoadSkin(path, shader[i].Name);
		}

		// Load texture coordinates
		md3_texcoord_t * tc = (md3_texcoord_t*)(((char*)ss)+LittleLong(ss->Ofs_Texcoord));
		s->texcoords = new MD3TexCoord[s->numVertices];

		for(int i=0;i<s->numVertices;i++)
		{
			s->texcoords[i].s = tc[i].s;
			s->texcoords[i].t = tc[i].t;
		}

		// Load vertices and texture coordinates
		md3_vertex_t * vt = (md3_vertex_t*)(((char*)ss)+LittleLong(ss->Ofs_XYZNormal));
		s->vertices = new MD3Vertex[s->numVertices * numFrames];

		for(int i=0;i<s->numVertices * numFrames;i++)
		{
			s->vertices[i].x = LittleShort(vt[i].x);
			s->vertices[i].y = LittleShort(vt[i].y);
			s->vertices[i].z = LittleShort(vt[i].z);
		}
#ifdef __vita__
		MD3AllocPacked(s, numFrames);
		for (int f = 0; f < numFrames; f++)
			MD3PackFrame(s, f, &s->vertices[f * s->numVertices].x);
		delete[] s->vertices;
		s->vertices = NULL;
#endif
	}
	return true;
}

#ifdef __vita__
//===========================================================================
//
// [Vita] Loads an MD3 straight from a forward-only lump stream, so the file
// is never held in memory whole. HaloDoom's third-person Spartan models are
// 18-25 MB each; reading one into a buffer (twice: lump cache and copy)
// needed 40+ MB at once, and a bot switching weapons mid-game ran the heap
// out. Returns false (model untouched) if the file's sections aren't in
// ascending order; the caller then falls back to Load().
//
//===========================================================================

namespace {
struct MD3Stream
{
	FileReaderBase *r;
	long pos;
	bool Skip(long to)
	{
		char junk[4096];
		if (to < pos) return false;
		while (pos < to)
		{
			long n = to - pos > (long)sizeof(junk) ? (long)sizeof(junk) : to - pos;
			if (r->Read(junk, n) != n) return false;
			pos += n;
		}
		return true;
	}
	bool ReadAt(long at, void *buf, long n)
	{
		if (!Skip(at)) return false;
		if (r->Read(buf, n) != n) return false;
		pos += n;
		return true;
	}
};
}

bool FMD3Model::LoadStream(const char * path, FileReaderBase *stream, int length, bool deferSkins)
{
	#pragma pack(4)
	struct hdr_t { DWORD Magic, Version; char Name[MAX_QPATH]; DWORD Flags, Num_Frames, Num_Tags, Num_Surfaces, Num_Skins, Ofs_Frames, Ofs_Tags, Ofs_Surfaces, Ofs_Eof; };
	struct surf_t { DWORD Magic; char Name[MAX_QPATH]; DWORD Flags, Num_Frames, Num_Shaders, Num_Verts, Num_Triangles, Ofs_Triangles, Ofs_Shaders, Ofs_Texcoord, Ofs_XYZNormal, Ofs_End; };
	struct shader_t { char Name[MAX_QPATH]; DWORD index; };
	struct frame_t { float min_Bounds[3], max_Bounds[3], localorigin[3]; float radius; char Name[16]; };
	struct vert_t { short x,y,z,n; };
	#pragma pack()

	MD3Stream in = { stream, 0 };
	hdr_t hdr;
	if (!in.ReadAt(0, &hdr, sizeof(hdr))) return false;

	int nFrames = LittleLong(hdr.Num_Frames);
	int nSurfaces = LittleLong(hdr.Num_Surfaces);
	long ofsFrames = LittleLong(hdr.Ofs_Frames), ofsSurf = LittleLong(hdr.Ofs_Surfaces);
	if (nFrames <= 0 || nSurfaces < 0 || ofsSurf < ofsFrames) return false;

	MD3Frame *fr = new MD3Frame[nFrames];
	MD3Surface *sf = new MD3Surface[nSurfaces];
	TArray<FString> shaderNames;
	TArray<int> shaderCounts;
	bool ok = true;

	for (int i = 0; i < nFrames && ok; i++)
	{
		frame_t f;
		ok = in.ReadAt(ofsFrames + i * (long)sizeof(f), &f, sizeof(f));
		if (ok)
		{
			strncpy(fr[i].Name, f.Name, 16);
			for (int j = 0; j < 3; j++) fr[i].origin[j] = f.localorigin[j];
		}
	}

	long surfStart = ofsSurf;
	for (int i = 0; i < nSurfaces && ok; i++)
	{
		MD3Surface *s = &sf[i];
		s->skins = NULL;
		surf_t ss;
		if (!(ok = in.ReadAt(surfStart, &ss, sizeof(ss)))) break;
		s->numSkins = LittleLong(ss.Num_Shaders);
		s->numTriangles = LittleLong(ss.Num_Triangles);
		s->numVertices = LittleLong(ss.Num_Verts);
		int nv = s->numVertices, nt = s->numTriangles, nsh = s->numSkins;

		// The four sections, read in file order.
		enum { SEC_SHADERS, SEC_TRIS, SEC_ST, SEC_XYZ };
		long ofs[4] = { (long)LittleLong(ss.Ofs_Shaders), (long)LittleLong(ss.Ofs_Triangles),
			(long)LittleLong(ss.Ofs_Texcoord), (long)LittleLong(ss.Ofs_XYZNormal) };
		int order[4] = { 0, 1, 2, 3 };
		for (int a = 0; a < 4; a++) for (int b = a + 1; b < 4; b++)
			if (ofs[order[b]] < ofs[order[a]]) { int t = order[a]; order[a] = order[b]; order[b] = t; }

		for (int k = 0; k < 4 && ok; k++)
		{
			long at = surfStart + ofs[order[k]];
			switch (order[k])
			{
			case SEC_SHADERS:
				shaderCounts.Push(nsh);
				for (int j = 0; j < nsh && ok; j++)
				{
					shader_t sh;
					if ((ok = in.ReadAt(at + j * (long)sizeof(sh), &sh, sizeof(sh))))
					{
						sh.Name[MAX_QPATH - 1] = 0;
						shaderNames.Push(FString(sh.Name));
					}
				}
				break;

			case SEC_TRIS:
			{
				DWORD tri[3 * 256];
				if (nv <= 65535) s->indices16 = new unsigned short[nt * 3];
				else s->tris = new MD3Triangle[nt];
				for (int t = 0; t < nt && ok; )
				{
					int n = nt - t > 256 ? 256 : nt - t;
					if (!(ok = in.ReadAt(at + t * 12L, tri, n * 12L))) break;
					for (int q = 0; q < n * 3; q++)
					{
						DWORD v = LittleLong(tri[q]);
						if (s->indices16) s->indices16[t * 3 + q] = (unsigned short)v;
						else s->tris[t + q / 3].VertIndex[q % 3] = v;
					}
					t += n;
				}
				break;
			}

			case SEC_ST:
				s->texcoords = new MD3TexCoord[nv];
				ok = in.ReadAt(at, s->texcoords, nv * 8L);
				// float pairs, little-endian like the Vita
				break;

			case SEC_XYZ:
			{
				TArray<vert_t> buf;
				TArray<short> xyz;
				buf.Resize(nv > 0 ? nv : 1);
				xyz.Resize(nv * 3 > 0 ? nv * 3 : 1);
				MD3AllocPacked(s, nFrames);
				for (int f = 0; f < nFrames && ok; f++)
				{
					if (!(ok = in.ReadAt(at + (long)f * nv * 8, &buf[0], nv * 8L))) break;
					for (int q = 0; q < nv; q++)
					{
						xyz[q * 3] = LittleShort(buf[q].x);
						xyz[q * 3 + 1] = LittleShort(buf[q].y);
						xyz[q * 3 + 2] = LittleShort(buf[q].z);
					}
					MD3PackFrame(s, f, &xyz[0]);
				}
				break;
			}
			}
		}
		surfStart += LittleLong(ss.Ofs_End);
	}

	if (!ok)
	{
		delete[] fr;
		delete[] sf;
		return false;
	}

	numFrames = nFrames;
	numTags = LittleLong(hdr.Num_Tags);
	numSurfaces = nSurfaces;
	frames = fr;
	surfaces = sf;
	for (int i = 0; i < nSurfaces; i++)
	{
		sf[i].skins = new FTexture *[sf[i].numSkins];
		for (int j = 0; j < sf[i].numSkins; j++) sf[i].skins[j] = NULL;
	}

	// Skins last: loading them may read the archive. A model loaded on the
	// background thread gets them on the game thread (ResolveDeferredSkins).
	DeferredSkins = shaderNames;
	DeferredPath = path;
	if (!deferSkins)
		ResolveDeferredSkins();
	return true;
}

void FMD3Model::ResolveDeferredSkins()
{
	unsigned name = 0;
	for (int i = 0; i < numSurfaces; i++)
	{
		MD3Surface *s = &surfaces[i];
		for (int j = 0; j < s->numSkins && name < DeferredSkins.Size(); j++, name++)
		{
			s->skins[j] = LoadSkin("", DeferredSkins[name]);
			if (s->skins[j] == NULL)
				s->skins[j] = LoadSkin(DeferredPath, DeferredSkins[name]);
		}
	}
	DeferredSkins.Clear();
	DeferredPath = "";
}
#endif

int FMD3Model::FindFrame(const char * name)
{
	for (int i=0;i<numFrames;i++)
	{
		if (!stricmp(name, frames[i].Name)) return i;
	}
	return -1;
}

// Expands one frame (or a blend of two) to float xyz in GL axis order.
static TArray<float> MD3Scratch;

#ifdef __vita__
int FMD3Model::MemSize()
{
	int bytes = numFrames * sizeof(MD3Frame);
	for (int i = 0; i < numSurfaces; i++)
	{
		MD3Surface *s = &surfaces[i];
		bytes += s->numVertices * numFrames * 4 + numFrames * 24;
		bytes += s->numVertices * sizeof(MD3TexCoord);
		bytes += s->indices16 ? s->numTriangles * 6 : s->numTriangles * sizeof(MD3Triangle);
	}
	return bytes;
}

float FMD3Model::VitaCheckPacking(const char *buffer)
{
	DWORD ofsSurf = LittleLong(((DWORD *)buffer)[25]);	// header: Ofs_Surfaces at byte 100
	const char *surf = buffer + ofsSurf;
	float maxerr = 0;
	for (int i = 0; i < numSurfaces; i++)
	{
		const DWORD *sh = (const DWORD *)(surf + 72);	// Num_Frames .. Ofs_End
		DWORD ofsXyz = LittleLong(sh[7]), ofsEnd = LittleLong(sh[8]);
		const short *raw = (const short *)(surf + ofsXyz);
		MD3Surface *s = &surfaces[i];
		for (int f = 0; f < numFrames; f++)
		{
			for (int v = 0; v < s->numVertices; v++)
			{
				unsigned w = s->packed[f * s->numVertices + v];
				float d[3] = { s->pmin[f*3] + (w & 2047) * s->pscale[f*3], s->pmin[f*3+1] + ((w >> 11) & 2047) * s->pscale[f*3+1], s->pmin[f*3+2] + (w >> 22) * s->pscale[f*3+2] };
				for (int c = 0; c < 3; c++)
				{
					float e = fabsf(d[c] - LittleShort(raw[(f * s->numVertices + v) * 4 + c]));
					if (e > maxerr) maxerr = e;
				}
			}
		}
		surf += ofsEnd;
	}
	return maxerr;	// in MD3 units (1/64 map unit)
}

// Unpacks one frame (or a blend of two) to float xyz in GL axis order.
static float *MD3ExpandPacked(FMD3Model::MD3Surface *s, int f1, int f2, double inter)
{
	int nv = s->numVertices;
	MD3Scratch.Resize(nv * 3 > 0 ? nv * 3 : 3);
	float *out = &MD3Scratch[0];
	const float sc = 1.f / 64.f;
	const float sz = sc * rModelAspectMod;
	const unsigned *p1 = s->packed + f1 * nv;
	const float *m1 = s->pmin + f1 * 3, *k1 = s->pscale + f1 * 3;
	if (f2 < 0)
	{
		for (int i = 0; i < nv; i++, out += 3)
		{
			unsigned w = p1[i];
			float x = m1[0] + (w & 2047) * k1[0];
			float y = m1[1] + ((w >> 11) & 2047) * k1[1];
			float z = m1[2] + (w >> 22) * k1[2];
			out[0] = x * sc; out[1] = z * sz; out[2] = y * sc;
		}
	}
	else
	{
		const unsigned *p2 = s->packed + f2 * nv;
		const float *m2 = s->pmin + f2 * 3, *k2 = s->pscale + f2 * 3;
		const float a = float(1 - inter), b = float(inter);
		for (int i = 0; i < nv; i++, out += 3)
		{
			unsigned w = p1[i], u = p2[i];
			float x = a * (m1[0] + (w & 2047) * k1[0]) + b * (m2[0] + (u & 2047) * k2[0]);
			float y = a * (m1[1] + ((w >> 11) & 2047) * k1[1]) + b * (m2[1] + ((u >> 11) & 2047) * k2[1]);
			float z = a * (m1[2] + (w >> 22) * k1[2]) + b * (m2[2] + (u >> 22) * k2[2]);
			out[0] = x * sc; out[1] = z * sz; out[2] = y * sc;
		}
	}
	return &MD3Scratch[0];
}
#endif

static float *MD3Expand(const short *v1, const short *v2, double inter, int numVertices)
{
	MD3Scratch.Resize(numVertices * 3);
	float *out = &MD3Scratch[0];
	const float s = 1.f / 64.f;
	const float sz = s * rModelAspectMod;
	if (v2 == NULL)
	{
		for (int k = 0; k < numVertices; k++, v1 += 3, out += 3)
		{
			out[0] = v1[0] * s;
			out[1] = v1[2] * sz;
			out[2] = v1[1] * s;
		}
	}
	else
	{
		const float a = float(1 - inter), b = float(inter);
		for (int k = 0; k < numVertices; k++, v1 += 3, v2 += 3, out += 3)
		{
			out[0] = (a * v1[0] + b * v2[0]) * s;
			out[1] = (a * v1[2] + b * v2[2]) * sz;
			out[2] = (a * v1[1] + b * v2[1]) * s;
		}
	}
	return &MD3Scratch[0];
}

void FMD3Model::RenderTriangles(MD3Surface * surf, const float * xyz)
{
	gl_RenderState.Apply();
#ifdef __vita__
	// [Vita] One glDrawElements per surface instead of 2 calls per vertex:
	// HaloDoom's characters have ~6,500 triangles each.
	if (surf->indices16 != NULL)
	{
		glVertexPointer(3, GL_FLOAT, 0, xyz);
		glTexCoordPointer(2, GL_FLOAT, 0, surf->texcoords);
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glDrawElements(GL_TRIANGLES, surf->numTriangles * 3, GL_UNSIGNED_SHORT, surf->indices16);
		glDisableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		return;
	}
#endif
	glBegin(GL_TRIANGLES);
	for(int i=0; i<surf->numTriangles;i++)
	{
		for(int j=0;j<3;j++)
		{
			int x = surf->tris[i].VertIndex[j];

			glTexCoord2fv(&surf->texcoords[x].s);
			glVertex3fv(xyz + x * 3);
		}
	}
	glEnd();
}

void FMD3Model::RenderFrame(FTexture * skin, int frameno, int cm, int translation)
{
	if (frameno>=numFrames) return;

	MD3Frame * frame = &frames[frameno];

	// I can't confirm correctness of this because no model I have tested uses this information
	// glMatrixMode(GL_MODELVIEW);
	// glTranslatef(frame->origin[0], frame->origin[1], frame->origin[2]);

	for(int i=0;i<numSurfaces;i++)
	{
		MD3Surface * surf = &surfaces[i];

		// [BB] In case no skin is specified via MODELDEF, check if the MD3 has a skin for the current surface.
		// Note: Each surface may have a different skin.
		FTexture *surfaceSkin = skin;
		if (!surfaceSkin)
		{
			if (curSpriteMDLFrame->surfaceskins && curSpriteMDLFrame->surfaceskins[curMDLIndex][i])
			{
				surfaceSkin = curSpriteMDLFrame->surfaceskins[curMDLIndex][i];
			}
			else if (surf->numSkins > 0 && surf->skins[0])
			{
				surfaceSkin = surf->skins[0];
			}

			if (!surfaceSkin) return;
		}

		FMaterial * tex = FMaterial::ValidateTexture(surfaceSkin);

		tex->Bind(cm, 0, translation);
#ifdef __vita__
		RenderTriangles(surf, MD3ExpandPacked(surf, frameno, -1, 0));
#else
		RenderTriangles(surf, MD3Expand(&surf->vertices[frameno * surf->numVertices].x, NULL, 0, surf->numVertices));
#endif
	}
}

void FMD3Model::RenderFrameInterpolated(FTexture * skin, int frameno, int frameno2, double inter, int cm, int translation)
{
	if (frameno>=numFrames || frameno2>=numFrames) return;

	for(int i=0;i<numSurfaces;i++)
	{
		MD3Surface * surf = &surfaces[i];

		// [BB] In case no skin is specified via MODELDEF, check if the MD3 has a skin for the current surface.
		// Note: Each surface may have a different skin.
		FTexture *surfaceSkin = skin;
		if (!surfaceSkin)
		{
			if (curSpriteMDLFrame->surfaceskins && curSpriteMDLFrame->surfaceskins[curMDLIndex][i])
			{
				surfaceSkin = curSpriteMDLFrame->surfaceskins[curMDLIndex][i];
			}
			else if(surf->numSkins > 0 && surf->skins[0])
			{
				surfaceSkin = surf->skins[0];
			}

			if (!surfaceSkin) return;
		}

		FMaterial * tex = FMaterial::ValidateTexture(surfaceSkin);

		tex->Bind(cm, 0, translation);

#ifdef __vita__
		RenderTriangles(surf, MD3ExpandPacked(surf, frameno, frameno2, inter));
#else
		RenderTriangles(surf, MD3Expand(&surf->vertices[frameno * surf->numVertices].x,
			&surf->vertices[frameno2 * surf->numVertices].x, inter, surf->numVertices));
#endif
	}
}

FMD3Model::~FMD3Model()
{
	if (frames) delete [] frames;
	if (surfaces) delete [] surfaces;
	frames = NULL;
	surfaces = NULL;
}
