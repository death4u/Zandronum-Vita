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
		static char junk[4096];
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

bool FMD3Model::LoadStream(const char * path, FileReaderBase *stream, int length)
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
				vert_t buf[512];
				long total = (long)nv * nFrames;
				s->vertices = new MD3Vertex[total];
				for (long v = 0; v < total && ok; )
				{
					long n = total - v > 512 ? 512 : total - v;
					if (!(ok = in.ReadAt(at + v * 8, buf, n * 8))) break;
					for (long q = 0; q < n; q++)
					{
						s->vertices[v + q].x = LittleShort(buf[q].x);
						s->vertices[v + q].y = LittleShort(buf[q].y);
						s->vertices[v + q].z = LittleShort(buf[q].z);
					}
					v += n;
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

	// Skins last: loading them may read the archive.
	unsigned name = 0;
	for (int i = 0; i < nSurfaces; i++)
	{
		MD3Surface *s = &sf[i];
		s->skins = new FTexture *[s->numSkins];
		for (int j = 0; j < s->numSkins; j++, name++)
		{
			s->skins[j] = LoadSkin("", shaderNames[name]);
			if (s->skins[j] == NULL)
				s->skins[j] = LoadSkin(path, shaderNames[name]);
		}
	}

	numFrames = nFrames;
	numTags = LittleLong(hdr.Num_Tags);
	numSurfaces = nSurfaces;
	frames = fr;
	surfaces = sf;
	return true;
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
		RenderTriangles(surf, MD3Expand(&surf->vertices[frameno * surf->numVertices].x, NULL, 0, surf->numVertices));
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

		RenderTriangles(surf, MD3Expand(&surf->vertices[frameno * surf->numVertices].x,
			&surf->vertices[frameno2 * surf->numVertices].x, inter, surf->numVertices));
	}
}

FMD3Model::~FMD3Model()
{
	if (frames) delete [] frames;
	if (surfaces) delete [] surfaces;
	frames = NULL;
	surfaces = NULL;
}
