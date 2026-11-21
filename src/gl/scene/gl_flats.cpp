/*
** gl_flat.cpp
** Flat rendering
**
**---------------------------------------------------------------------------
** Copyright 2000-2005 Christoph Oelckers
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
** 5. Full disclosure of the entire project's source code, except for third
**    party libraries is mandatory. (NOTE: This clause is non-negotiable!)
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
#include "a_sharedglobal.h"
#include "r_defs.h"
#include "r_sky.h"
#include "r_utility.h"
#include "g_level.h"
#include "doomstat.h"
#include "d_player.h"

#include "gl/system/gl_interface.h"
#include "gl/system/gl_cvars.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/data/gl_data.h"
#include "gl/data/gl_vertexbuffer.h"
#include "gl/dynlights/gl_dynlight.h"
#include "gl/dynlights/gl_glow.h"
#include "gl/dynlights/gl_lightbuffer.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/shaders/gl_shader.h"
#include "gl/textures/gl_material.h"
#include "gl/utility/gl_clock.h"
#include "gl/utility/gl_convert.h"
#include "gl/scene/gl_vitawallbatch.h"
#include "gl/utility/gl_templates.h"

//==========================================================================
//
// Sets the texture matrix according to the plane's texture positioning
// information
//
//==========================================================================

bool gl_SetPlaneTextureRotation(const GLSectorPlane * secplane, FMaterial * gltexture)
{
	// only manipulate the texture matrix if needed.
	if (secplane->xoffs != 0 || secplane->yoffs != 0 ||
		secplane->xscale != FRACUNIT || secplane->yscale != FRACUNIT ||
		secplane->angle != 0 || 
		gltexture->TextureWidth(GLUSE_TEXTURE) != 64 ||
		gltexture->TextureHeight(GLUSE_TEXTURE) != 64)
	{
		float uoffs=FIXED2FLOAT(secplane->xoffs)/gltexture->TextureWidth(GLUSE_TEXTURE);
		float voffs=FIXED2FLOAT(secplane->yoffs)/gltexture->TextureHeight(GLUSE_TEXTURE);

		float xscale1=FIXED2FLOAT(secplane->xscale);
		float yscale1=FIXED2FLOAT(secplane->yscale);
		if (gltexture->tex->bHasCanvas)
		{
			yscale1 = 0 - yscale1;
		}
		float angle=-ANGLE_TO_FLOAT(secplane->angle);

		float xscale2=64.f/gltexture->TextureWidth(GLUSE_TEXTURE);
		float yscale2=64.f/gltexture->TextureHeight(GLUSE_TEXTURE);

		glMatrixMode(GL_TEXTURE);
		glPushMatrix();
		glScalef(xscale1 ,yscale1,1.0f);
		glTranslatef(uoffs,voffs,0.0f);
		glScalef(xscale2 ,yscale2,1.0f);
		glRotatef(angle,0.0f,0.0f,1.0f);
		return true;
	}
	return false;
}


#ifdef __vita__
// [Vita] The texture matrix of gl_SetPlaneTextureRotation as a 2D affine
// transform, so flats can be put into the shared batch with their final UVs.
struct FVitaUVXform { float a, b, c, d, e, f; };	// u' = a*u + b*v + c, v' = d*u + e*v + f
static FVitaUVXform Vita_PlaneUV(const GLSectorPlane * secplane, FMaterial * gltexture)
{
	FVitaUVXform x = { 1, 0, 0, 0, 1, 0 };
	if (secplane->xoffs != 0 || secplane->yoffs != 0 ||
		secplane->xscale != FRACUNIT || secplane->yscale != FRACUNIT ||
		secplane->angle != 0 ||
		gltexture->TextureWidth(GLUSE_TEXTURE) != 64 ||
		gltexture->TextureHeight(GLUSE_TEXTURE) != 64)
	{
		float uoffs=FIXED2FLOAT(secplane->xoffs)/gltexture->TextureWidth(GLUSE_TEXTURE);
		float voffs=FIXED2FLOAT(secplane->yoffs)/gltexture->TextureHeight(GLUSE_TEXTURE);
		float xscale1=FIXED2FLOAT(secplane->xscale);
		float yscale1=FIXED2FLOAT(secplane->yscale);
		if (gltexture->tex->bHasCanvas) yscale1 = 0 - yscale1;
		float angle=-ANGLE_TO_FLOAT(secplane->angle) * (float)(M_PI / 180.0);
		float xscale2=64.f/gltexture->TextureWidth(GLUSE_TEXTURE);
		float yscale2=64.f/gltexture->TextureHeight(GLUSE_TEXTURE);
		float cs = cosf(angle), sn = sinf(angle);
		// S1 * T * S2 * R
		x.a = xscale1 * xscale2 * cs;  x.b = -xscale1 * xscale2 * sn; x.c = xscale1 * uoffs;
		x.d = yscale1 * yscale2 * sn;  x.e = yscale1 * yscale2 * cs;  x.f = yscale1 * voffs;
	}
	return x;
}
#endif

//==========================================================================
//
// Flats 
//
//==========================================================================

void GLFlat::DrawSubsectorLights(subsector_t * sub, int pass)
{
	Plane p;
	Vector nearPt, up, right, t1;
	float scale;
	unsigned int k;
	seg_t *v;

	FLightNode * node = sub->lighthead[pass==GLPASS_LIGHT_ADDITIVE];
#ifdef __vita__
	FLightNode *vhead = node;
	extern bool Vita_LightAllowed(FLightNode *head, ADynamicLight *light);
	if (!VitaWalls.Collecting)
#endif
	gl_RenderState.Apply();
	while (node)
	{
		ADynamicLight * light = node->lightsource;
		
#ifdef __vita__
		if ((light->flags2&MF2_DORMANT) || !Vita_LightAllowed(vhead, light))
#else
		if (light->flags2&MF2_DORMANT)
#endif
		{
			node=node->nextLight;
			continue;
		}
		iter_dlightf++;

		// we must do the side check here because gl_SetupLight needs the correct plane orientation
		// which we don't have for Legacy-style 3D-floors
		fixed_t planeh = plane.plane.ZatPoint(light->x, light->y);
		if (gl_lights_checkside && ((planeh<light->z && ceiling) || (planeh>light->z && !ceiling)))
		{
			node=node->nextLight;
			continue;
		}

		p.Set(plane.plane);
		if (!gl_SetupLight(p, light, nearPt, up, right, scale, Colormap.colormap, false, foggy)) 
		{
			node=node->nextLight;
			continue;
		}
		draw_dlightf++;

		// Render the light
		glBegin(GL_TRIANGLE_FAN);
		for(k = 0, v = sub->firstline; k < sub->numlines; k++, v++)
		{
			vertex_t *vt = v->v1;
			float zc = plane.plane.ZatPoint(vt->fx, vt->fy) + dz;

			t1.Set(vt->fx, zc, vt->fy);
			Vector nearToVert = t1 - nearPt;
			glTexCoord2f( (nearToVert.Dot(right) * scale) + 0.5f, (nearToVert.Dot(up) * scale) + 0.5f);

			glVertex3f(vt->fx, zc, vt->fy);
		}

		glEnd();
		node = node->nextLight;
	}
}


//==========================================================================
//
// Flats 
//
//==========================================================================
extern FDynLightData lightdata;

bool GLFlat::SetupSubsectorLights(bool lightsapplied, subsector_t * sub)
{
	Plane p;

	lightdata.Clear();
	for(int i=0;i<2;i++)
	{
		FLightNode * node = sub->lighthead[i];
		while (node)
		{
			ADynamicLight * light = node->lightsource;
			
			if (light->flags2&MF2_DORMANT)
			{
				node=node->nextLight;
				continue;
			}
			iter_dlightf++;

			// we must do the side check here because gl_SetupLight needs the correct plane orientation
			// which we don't have for Legacy-style 3D-floors
			fixed_t planeh = plane.plane.ZatPoint(light->x, light->y);
			if (gl_lights_checkside && ((planeh<light->z && ceiling) || (planeh>light->z && !ceiling)))
			{
				node=node->nextLight;
				continue;
			}

			p.Set(plane.plane);
			gl_GetLight(p, light, Colormap.colormap, false, false, lightdata);
			node = node->nextLight;
		}
	}

	int numlights[3];

	lightdata.Combine(numlights, gl.MaxLights());
	if (numlights[2] > 0)
	{
		draw_dlightf+=numlights[2]/2;
		gl_RenderState.EnableLight(true);
		gl_RenderState.SetLights(numlights, &lightdata.arrays[0][0]);
		gl_RenderState.Apply();
		return true;
	}
	if (lightsapplied) 
	{
		gl_RenderState.EnableLight(false);
		gl_RenderState.Apply();
	}
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

void GLFlat::DrawSubsector(subsector_t * sub)
{
	glBegin(GL_TRIANGLE_FAN);

	for(unsigned int k=0; k<sub->numlines; k++)
	{
		vertex_t *vt = sub->firstline[k].v1;
		glTexCoord2f(vt->fx/64.f, -vt->fy/64.f);
		float zc = plane.plane.ZatPoint(vt->fx, vt->fy) + dz;
		glVertex3f(vt->fx, zc, vt->fy);
	}
	glEnd();

	flatvertices += sub->numlines;
	flatprimitives++;
}

#ifdef __vita__
// Appends a subsector's polygon to the shared wall/flat batch.
static void Vita_AddSubsectorToBatch(GLFlat *flat, subsector_t * sub, const FVitaUVXform &x)
{
	unsigned n = sub->numlines;
	if (n < 3) return;
	vertex_t *v0 = sub->firstline[0].v1;
	if ((sub->lighthead[0] != NULL || sub->lighthead[1] != NULL) && Vita_VertexLightsOn())
	{
		// lit: per-vertex light, big triangles split
		float pts[3][3], uvs[3][2];
		bool planeok = false;
		for (unsigned k = 1; k + 1 < n; k++)
		{
			vertex_t *vs[3] = { v0, sub->firstline[k].v1, sub->firstline[k+1].v1 };
			for (int j = 0; j < 3; j++)
			{
				vertex_t *vt = vs[j];
				pts[j][0] = vt->fx; pts[j][1] = flat->plane.plane.ZatPoint(vt->fx, vt->fy) + flat->dz; pts[j][2] = vt->fy;
				float u = vt->fx/64.f, v = -vt->fy/64.f;
				uvs[j][0] = x.a * u + x.b * v + x.c; uvs[j][1] = x.d * u + x.e * v + x.f;
			}
			if (!planeok)
			{
				// a flat's plane: a point and two directions along it
				float q1[3] = { pts[0][0] + 64.f, pts[0][1], pts[0][2] }, q2[3] = { pts[0][0], pts[0][1], pts[0][2] + 64.f };
				q1[1] = flat->plane.plane.ZatPoint(q1[0], q1[2]) + flat->dz;
				q2[1] = flat->plane.plane.ZatPoint(q2[0], q2[2]) + flat->dz;
				if (!Vita_SetupPolyLights(sub->lighthead[0], sub->lighthead[1], pts[0], q1, q2)) goto unlit;
				planeok = true;
			}
			Vita_EmitLitTriangle(pts[0], pts[1], pts[2], uvs[0], uvs[1], uvs[2]);
		}
		Vita_EndPolyLights();
		flatvertices += n;
		return;
	}
unlit:
	{
		const unsigned tris = n - 2;
		FVitaBucket *b = VitaWalls.Cur;
		float *xyz = &b->Xyz[b->Xyz.Reserve(tris * 9)];
		float *uv = &b->Uv[b->Uv.Reserve(tris * 6)];
		unsigned *rgba = &b->Rgba[b->Rgba.Reserve(tris * 3)];
		const unsigned color = VitaWalls.Color;
		const float z0 = flat->plane.plane.ZatPoint(v0->fx, v0->fy) + flat->dz;
		const float u0 = v0->fx/64.f, w0 = -v0->fy/64.f;
		const float uu0 = x.a * u0 + x.b * w0 + x.c, vv0 = x.d * u0 + x.e * w0 + x.f;
		vertex_t *prev = sub->firstline[1].v1;
		float zp = flat->plane.plane.ZatPoint(prev->fx, prev->fy) + flat->dz;
		float up_ = x.a * (prev->fx/64.f) + x.b * (-prev->fy/64.f) + x.c, vp_ = x.d * (prev->fx/64.f) + x.e * (-prev->fy/64.f) + x.f;
		for (unsigned k = 1; k + 1 < n; k++)
		{
			vertex_t *cur = sub->firstline[k+1].v1;
			const float zc = flat->plane.plane.ZatPoint(cur->fx, cur->fy) + flat->dz;
			const float uc = x.a * (cur->fx/64.f) + x.b * (-cur->fy/64.f) + x.c, vc = x.d * (cur->fx/64.f) + x.e * (-cur->fy/64.f) + x.f;
			xyz[0] = v0->fx; xyz[1] = z0; xyz[2] = v0->fy;
			xyz[3] = prev->fx; xyz[4] = zp; xyz[5] = prev->fy;
			xyz[6] = cur->fx; xyz[7] = zc; xyz[8] = cur->fy;
			xyz += 9;
			uv[0] = uu0; uv[1] = vv0; uv[2] = up_; uv[3] = vp_; uv[4] = uc; uv[5] = vc;
			uv += 6;
			rgba[0] = rgba[1] = rgba[2] = color;
			rgba += 3;
			prev = cur; zp = zc; up_ = uc; vp_ = vc;
		}
	}
	flatvertices += n;
}

// Appends a subsector's polygon as a triangle list.
void GLFlat::AddSubsectorTriangles(subsector_t * sub, TArray<float> &xyz, TArray<float> &uv)
{
	unsigned n = sub->numlines;
	if (n < 3) return;
	vertex_t *v0 = sub->firstline[0].v1;
	float z0 = plane.plane.ZatPoint(v0->fx, v0->fy) + dz;
	for (unsigned k = 1; k + 1 < n; k++)
	{
		vertex_t *va = sub->firstline[k].v1, *vb = sub->firstline[k+1].v1;
		vertex_t *vs[3] = { v0, va, vb };
		for (int j = 0; j < 3; j++)
		{
			vertex_t *vt = vs[j];
			float zc = j == 0 ? z0 : plane.plane.ZatPoint(vt->fx, vt->fy) + dz;
			xyz.Push(vt->fx); xyz.Push(zc); xyz.Push(vt->fy);
			uv.Push(vt->fx/64.f); uv.Push(-vt->fy/64.f);
		}
	}
	flatvertices += n;
}
#endif

//==========================================================================
//
//
//
//==========================================================================

void GLFlat::DrawSubsectors(int pass, bool istrans)
{
#ifdef __vita__
	{ extern unsigned Vita_GLCount[8]; Vita_GLCount[7]++; }
#endif
	bool lightsapplied = false;

	gl_RenderState.Apply();
	if (sub)
	{
		// This represents a single subsector
		if (pass == GLPASS_ALL) lightsapplied = SetupSubsectorLights(lightsapplied, sub);
		DrawSubsector(sub);
	}
	else
	{
		if (vboindex >= 0)
		{
			//glColor3f( 1.f,.5f,.5f);
			int index = vboindex;
			for (int i=0; i<sector->subsectorcount; i++)
			{
				subsector_t * sub = sector->subsectors[i];
				// This is just a quick hack to make translucent 3D floors and portals work.
				if (gl_drawinfo->ss_renderflags[sub-subsectors]&renderflags || istrans)
				{
					if (pass == GLPASS_ALL) lightsapplied = SetupSubsectorLights(lightsapplied, sub);
					glDrawArrays(GL_TRIANGLE_FAN, index, sub->numlines);
					flatvertices += sub->numlines;
					flatprimitives++;
				}
				index += sub->numlines;
			}
		}
#ifdef __vita__
		else if (pass != GLPASS_ALL)
		{
			// [Vita] One draw call per flat instead of one glBegin/glEnd per
			// subsector: every immediate-mode batch costs vitaGL a lot of CPU.
			static TArray<float> xyz, uv;
			xyz.Clear();
			uv.Clear();
			for (int i=0; i<sector->subsectorcount; i++)
			{
				subsector_t * sub = sector->subsectors[i];
				if (gl_drawinfo->ss_renderflags[sub-subsectors]&renderflags || istrans)
					AddSubsectorTriangles(sub, xyz, uv);
			}
			if (!(renderflags&SSRF_RENDER3DPLANES))
			{
				gl_subsectorrendernode * node = (renderflags&SSRF_RENDERFLOOR)?
					gl_drawinfo->GetOtherFloorPlanes(sector->sectornum) :
					gl_drawinfo->GetOtherCeilingPlanes(sector->sectornum);
				for (; node; node = node->next)
					AddSubsectorTriangles(node->sub, xyz, uv);
			}
			if (xyz.Size() > 0)
			{
				glVertexPointer(3, GL_FLOAT, 0, &xyz[0]);
				glTexCoordPointer(2, GL_FLOAT, 0, &uv[0]);
				glEnableClientState(GL_VERTEX_ARRAY);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glDrawArrays(GL_TRIANGLES, 0, xyz.Size() / 3);
				glDisableClientState(GL_VERTEX_ARRAY);
				glDisableClientState(GL_TEXTURE_COORD_ARRAY);
				flatprimitives++;
			}
			gl_RenderState.EnableLight(false);
			return;
		}
#endif
		else
		{
			//glColor3f( .5f,1.f,.5f); // these are for testing the VBO stuff.
			// Draw the subsectors belonging to this sector
			for (int i=0; i<sector->subsectorcount; i++)
			{
				subsector_t * sub = sector->subsectors[i];
				if (gl_drawinfo->ss_renderflags[sub-subsectors]&renderflags || istrans)
				{
					if (pass == GLPASS_ALL) lightsapplied = SetupSubsectorLights(lightsapplied, sub);
					DrawSubsector(sub);
				}
			}
		}

		// Draw the subsectors assigned to it due to missing textures
		if (!(renderflags&SSRF_RENDER3DPLANES))
		{
			gl_subsectorrendernode * node = (renderflags&SSRF_RENDERFLOOR)?
				gl_drawinfo->GetOtherFloorPlanes(sector->sectornum) :
				gl_drawinfo->GetOtherCeilingPlanes(sector->sectornum);

			while (node)
			{
				if (pass == GLPASS_ALL) lightsapplied = SetupSubsectorLights(lightsapplied, node->sub);
				DrawSubsector(node->sub);
				node = node->next;
			}
		}
	}
	gl_RenderState.EnableLight(false);
}


//==========================================================================
//
//
//
//==========================================================================
void GLFlat::Draw(int pass)
{
	int i;
	int rel = getExtraLight();

#ifdef _DEBUG
	if (sector->sectornum == 2)
	{
		int a = 0;
	}
#endif


#ifdef __vita__
	{
		EXTERN_CVAR(Bool, gl_vita_batchwalls)
		if (pass == GLPASS_PLAIN && gl_vita_batchwalls && vboindex < 0 && sub == NULL && glset.lightmode != 8)
		{
			float r, g, b;
			gl_SetColor(lightlevel, rel, &Colormap, &r, &g, &b);
			gl_SetFog(lightlevel, rel, &Colormap, false);
			Vita_BatchSurface(gltexture, Colormap.colormap, 0, false, r, g, b);
			Vita_BatchCheck();
			FVitaUVXform x = Vita_PlaneUV(&plane, gltexture);
			for (int i=0; i<sector->subsectorcount; i++)
			{
				subsector_t * ss = sector->subsectors[i];
				if (gl_drawinfo->ss_renderflags[ss-subsectors]&renderflags)
					Vita_AddSubsectorToBatch(this, ss, x);
			}
			if (!(renderflags&SSRF_RENDER3DPLANES))
			{
				gl_subsectorrendernode * node = (renderflags&SSRF_RENDERFLOOR)?
					gl_drawinfo->GetOtherFloorPlanes(sector->sectornum) :
					gl_drawinfo->GetOtherCeilingPlanes(sector->sectornum);
				for (; node; node = node->next)
					Vita_AddSubsectorToBatch(this, node->sub, x);
			}
			return;
		}
		// dynamic light passes (vita_multipasslights)
		if (gl_vita_batchwalls && vboindex < 0 && sub == NULL && glset.lightmode != 8 && !foggy
			&& (pass == GLPASS_BASE || pass == GLPASS_TEXTURE || pass == GLPASS_LIGHT || pass == GLPASS_LIGHT_ADDITIVE))
		{
			FVitaUVXform x = { 1, 0, 0, 0, 1, 0 };
			if (pass == GLPASS_BASE)
			{
				gl_SetColor(lightlevel, rel, &Colormap,1.0f);
				gl_SetFog(lightlevel, rel, &Colormap, false);
				Vita_BatchSurface(NULL, 0, 0, true);
			}
			else if (pass == GLPASS_TEXTURE)
			{
				Vita_BatchSurface(gltexture, Colormap.colormap, 0, true);
				x = Vita_PlaneUV(&plane, gltexture);
			}
			else
			{
				gl_SetFog((255+lightlevel)>>1, 0, &Colormap, false);
				EXTERN_CVAR(Bool, vita_lighttexpass)
				if (vita_lighttexpass && pass == GLPASS_LIGHT && gltexture != NULL)
				{
					Vita_BatchSurface(gltexture, Colormap.colormap, 0, true);
					FVitaUVXform fx = Vita_PlaneUV(&plane, gltexture);
					VitaWalls.LightPassMode = 2;
					VitaWalls.FlatA = fx.a; VitaWalls.FlatB = fx.b; VitaWalls.FlatC = fx.c;
					VitaWalls.FlatD = fx.d; VitaWalls.FlatE = fx.e; VitaWalls.FlatF = fx.f;
				}
				else Vita_BatchSurface(NULL, 0, 0, true);
				VitaWalls.Collecting = true;
				for (int i=0; i<sector->subsectorcount; i++)
				{
					subsector_t * ss = sector->subsectors[i];
					if (gl_drawinfo->ss_renderflags[ss-subsectors]&renderflags)
						DrawSubsectorLights(ss, pass);
				}
				if (!(renderflags&SSRF_RENDER3DPLANES))
				{
					gl_subsectorrendernode * node = (renderflags&SSRF_RENDERFLOOR)?
						gl_drawinfo->GetOtherFloorPlanes(sector->sectornum) :
						gl_drawinfo->GetOtherCeilingPlanes(sector->sectornum);
					for (; node; node = node->next)
						DrawSubsectorLights(node->sub, pass);
				}
				VitaWalls.Collecting = false;
				VitaWalls.LightPassMode = 0;
				return;
			}
			Vita_BatchCheck();
			for (int i=0; i<sector->subsectorcount; i++)
			{
				subsector_t * ss = sector->subsectors[i];
				if (gl_drawinfo->ss_renderflags[ss-subsectors]&renderflags)
					Vita_AddSubsectorToBatch(this, ss, x);
			}
			if (!(renderflags&SSRF_RENDER3DPLANES))
			{
				gl_subsectorrendernode * node = (renderflags&SSRF_RENDERFLOOR)?
					gl_drawinfo->GetOtherFloorPlanes(sector->sectornum) :
					gl_drawinfo->GetOtherCeilingPlanes(sector->sectornum);
				for (; node; node = node->next)
					Vita_AddSubsectorToBatch(this, node->sub, x);
			}
			return;
		}
		Vita_FlushWalls();
	}
#endif
	switch (pass)
	{
	case GLPASS_BASE:
		gl_SetColor(lightlevel, rel, &Colormap,1.0f);
		if (!foggy) gl_SetFog(lightlevel, rel, &Colormap, false);
		DrawSubsectors(pass, false);
		break;

	case GLPASS_PLAIN:			// Single-pass rendering
	case GLPASS_ALL:
	case GLPASS_BASE_MASKED:
		gl_SetColor(lightlevel, rel, &Colormap,1.0f);
		if (!foggy || pass != GLPASS_BASE_MASKED) gl_SetFog(lightlevel, rel, &Colormap, false);
		// fall through
	case GLPASS_TEXTURE:
	{
		gltexture->Bind(Colormap.colormap);
		bool pushed = gl_SetPlaneTextureRotation(&plane, gltexture);
		DrawSubsectors(pass, false);
		if (pushed) 
		{
			glPopMatrix();
			glMatrixMode(GL_MODELVIEW);
		}
		break;
	}

	case GLPASS_LIGHT:
	case GLPASS_LIGHT_ADDITIVE:

		if (!foggy)	gl_SetFog((255+lightlevel)>>1, 0, &Colormap, false);
		else gl_SetFog(lightlevel, 0, &Colormap, true);	

		if (sub)
		{
			DrawSubsectorLights(sub, pass);
		}
		else
		{
			// Draw the subsectors belonging to this sector
			for (i=0; i<sector->subsectorcount; i++)
			{
				subsector_t * sub = sector->subsectors[i];

				if (gl_drawinfo->ss_renderflags[sub-subsectors]&renderflags)
				{
					DrawSubsectorLights(sub, pass);
				}
			}

			// Draw the subsectors assigned to it due to missing textures
			if (!(renderflags&SSRF_RENDER3DPLANES))
			{
				gl_subsectorrendernode * node = (renderflags&SSRF_RENDERFLOOR)?
					gl_drawinfo->GetOtherFloorPlanes(sector->sectornum) :
					gl_drawinfo->GetOtherCeilingPlanes(sector->sectornum);

				while (node)
				{
					DrawSubsectorLights(node->sub, pass);
					node = node->next;
				}
			}
		}
		break;

	case GLPASS_TRANSLUCENT:
		if (renderstyle==STYLE_Add) gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE);
		gl_SetColor(lightlevel, rel, &Colormap, alpha);
		gl_SetFog(lightlevel, rel, &Colormap, false);
		gl_RenderState.AlphaFunc(GL_GEQUAL,gl_mask_threshold*(alpha));
		if (!gltexture)	
		{
			gl_RenderState.EnableTexture(false);
			DrawSubsectors(pass, true);
			gl_RenderState.EnableTexture(true);
		}
		else 
		{
			if (foggy) gl_RenderState.EnableBrightmap(false);
			gltexture->Bind(Colormap.colormap);
			bool pushed = gl_SetPlaneTextureRotation(&plane, gltexture);
			DrawSubsectors(pass, true);
			gl_RenderState.EnableBrightmap(true);
			if (pushed)
			{
				glPopMatrix();
				glMatrixMode(GL_MODELVIEW);
			}
		}
		if (renderstyle==STYLE_Add) gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	}
}


//==========================================================================
//
// GLFlat::PutFlat
//
// Checks texture, lighting and translucency settings and puts this
// plane in the appropriate render list.
//
//==========================================================================
inline void GLFlat::PutFlat(bool fog)
{
	int list;

	if (gl_fixedcolormap) 
	{
		Colormap.GetFixedColormap();
	}
	if (renderstyle!=STYLE_Translucent || alpha < 1.f - FLT_EPSILON || fog)
	{
		int list = (renderflags&SSRF_RENDER3DPLANES) ? GLDL_TRANSLUCENT : GLDL_TRANSLUCENTBORDER;
		gl_drawinfo->drawlists[list].AddFlat (this);
	}
	else if (gltexture != NULL)
	{
		static DrawListType list_indices[2][2][2]={
			{ { GLDL_PLAIN, GLDL_FOG      }, { GLDL_MASKED,      GLDL_FOGMASKED      } },
			{ { GLDL_LIGHT, GLDL_LIGHTFOG }, { GLDL_LIGHTMASKED, GLDL_LIGHTFOGMASKED } }
		};

		bool light = gl_forcemultipass && GL_MULTIPASS_LIGHTS;
		bool masked = gltexture->isMasked() && ((renderflags&SSRF_RENDER3DPLANES) || stack);

		if (!gl_fixedcolormap)
		{
			foggy = gl_CheckFog(&Colormap, lightlevel) || level.flags&LEVEL_HASFADETABLE;

			if (GL_MULTIPASS_LIGHTS && gl_lights && !gl_dynlight_shader && GLRenderer->mLightCount)	// Are lights touching this sector?
			{
				for(int i=0;i<sector->subsectorcount;i++) if (sector->subsectors[i]->lighthead[0] != NULL)
				{
					light=true;
				}
			}
		}
		else foggy = false;

		list = list_indices[light][masked][foggy];
		if (list == GLDL_LIGHT && gltexture->tex->gl_info.Brightmap && gl_BrightmapsActive()) list = GLDL_LIGHTBRIGHT;

		gl_drawinfo->drawlists[list].AddFlat (this);
	}
}

//==========================================================================
//
// This draws one flat 
// The passed sector does not indicate the area which is rendered. 
// It is only used as source for the plane data.
// The whichplane boolean indicates if the flat is a floor(false) or a ceiling(true)
//
//==========================================================================

void GLFlat::Process(sector_t * model, int whichplane, bool fog)
{
	plane.GetFromSector(model, whichplane);

	if (!fog)
	{
		if (plane.texture==skyflatnum) return;

		gltexture=FMaterial::ValidateTexture(plane.texture, true);
		if (!gltexture) return;
		if (gltexture->tex->isFullbright()) 
		{
			Colormap.LightColor.r = Colormap.LightColor.g = Colormap.LightColor.b = 0xff;
			lightlevel=255;
		}
	}
	else 
	{
		gltexture = NULL;
		lightlevel = abs(lightlevel);
	}

	// get height from vplane
	if (whichplane == sector_t::floor && sector->transdoor) dz = -1;
	else dz = 0;

	z = plane.plane.ZatPoint(0.f, 0.f);
	
	PutFlat(fog);
	rendered_flats++;
}

//==========================================================================
//
// Sets 3D floor info. Common code for all 4 cases 
//
//==========================================================================

void GLFlat::SetFrom3DFloor(F3DFloor *rover, bool top, bool underside)
{
	F3DFloor::planeref & plane = top? rover->top : rover->bottom;

	// FF_FOG requires an inverted logic where to get the light from
	lightlist_t *light = P_GetPlaneLight(sector, plane.plane, underside);
	lightlevel = *light->p_lightlevel;
	
	if (rover->flags & FF_FOG) Colormap.LightColor = (light->extra_colormap)->Fade;
	else Colormap.CopyLightColor(light->extra_colormap);

	alpha = rover->alpha/255.0f;
	renderstyle = rover->flags&FF_ADDITIVETRANS? STYLE_Add : STYLE_Translucent;
	if (plane.model->VBOHeightcheck(plane.isceiling))
	{
		vboindex = plane.vindex;
	}
	else
	{
		vboindex = -1;
	}
}

//==========================================================================
//
// Process a sector's flats for rendering
// This function is only called once per sector.
// Subsequent subsectors are just quickly added to the ss_renderflags array
//
//==========================================================================

void GLFlat::ProcessSector(sector_t * frontsector)
{
	lightlist_t * light;

#ifdef _DEBUG
	if (frontsector->sectornum==0)
	{
		int a = 0;
	}
#endif

	// Get the real sector for this one.
	sector=&sectors[frontsector->sectornum];	
	extsector_t::xfloor &x = sector->e->XFloor;
	this->sub=NULL;

	byte &srf = gl_drawinfo->sectorrenderflags[sector->sectornum];

	//
	//
	//
	// do floors
	//
	//
	//
	if (frontsector->floorplane.ZatPoint(FIXED2FLOAT(viewx), FIXED2FLOAT(viewy)) <= FIXED2FLOAT(viewz))
	{
		// process the original floor first.

		srf |= SSRF_RENDERFLOOR;

		lightlevel = gl_ClampLight(frontsector->GetFloorLight());
		Colormap=frontsector->ColorMap;
		if ((stack = (frontsector->portals[sector_t::floor] != NULL)))
		{
			gl_drawinfo->AddFloorStack(sector);
			alpha = frontsector->GetAlpha(sector_t::floor)/65536.0f;
		}
		else
		{
			alpha = 1.0f-frontsector->GetReflect(sector_t::floor);
		}
		if (frontsector->VBOHeightcheck(sector_t::floor))
		{
			vboindex = frontsector->vboindex[sector_t::floor];
		}
		else
		{
			vboindex = -1;
		}

		ceiling=false;
		renderflags=SSRF_RENDERFLOOR;

		if (x.ffloors.Size())
		{
			light = P_GetPlaneLight(sector, &frontsector->floorplane, false);
			if ((!(sector->GetFlags(sector_t::floor)&PLANEF_ABSLIGHTING) || light!=&x.lightlist[0])	
				&& (light->p_lightlevel != &frontsector->lightlevel))
			{
				lightlevel = *light->p_lightlevel;
			}

			Colormap.CopyLightColor(light->extra_colormap);
		}
		renderstyle = STYLE_Translucent;
		if (alpha!=0.0f) Process(frontsector, false, false);
	}
	
	//
	//
	//
	// do ceilings
	//
	//
	//
	if (frontsector->ceilingplane.ZatPoint(FIXED2FLOAT(viewx), FIXED2FLOAT(viewy)) >= FIXED2FLOAT(viewz))
	{
		// process the original ceiling first.

		srf |= SSRF_RENDERCEILING;

		lightlevel = gl_ClampLight(frontsector->GetCeilingLight());
		Colormap=frontsector->ColorMap;
		if ((stack = (frontsector->portals[sector_t::ceiling] != NULL))) 
		{
			gl_drawinfo->AddCeilingStack(sector);
			alpha = frontsector->GetAlpha(sector_t::ceiling)/65536.0f;
		}
		else
		{
			alpha = 1.0f-frontsector->GetReflect(sector_t::ceiling);
		}

		if (frontsector->VBOHeightcheck(sector_t::ceiling))
		{
			vboindex = frontsector->vboindex[sector_t::ceiling];
		}
		else
		{
			vboindex = -1;
		}

		ceiling=true;
		renderflags=SSRF_RENDERCEILING;

		if (x.ffloors.Size())
		{
			light = P_GetPlaneLight(sector, &sector->ceilingplane, true);

			if ((!(sector->GetFlags(sector_t::ceiling)&PLANEF_ABSLIGHTING))
				&& (light->p_lightlevel != &frontsector->lightlevel))
			{
				lightlevel = *light->p_lightlevel;
			}
			Colormap.CopyLightColor(light->extra_colormap);
		}
		renderstyle = STYLE_Translucent;
		if (alpha!=0.0f) Process(frontsector, true, false);
	}

	//
	//
	//
	// do 3D floors
	//
	//
	//

	stack=false;
	if (x.ffloors.Size())
	{
		player_t * player=players[consoleplayer].camera->player;

		renderflags=SSRF_RENDER3DPLANES;
		srf |= SSRF_RENDER3DPLANES;
		// 3d-floors must not overlap!
		fixed_t lastceilingheight=sector->CenterCeiling();	// render only in the range of the
		fixed_t lastfloorheight=sector->CenterFloor();		// current sector part (if applicable)
		F3DFloor * rover;	
		int k;
		
		// floors are ordered now top to bottom so scanning the list for the best match
		// is no longer necessary.

		ceiling=true;
		for(k=0;k<(int)x.ffloors.Size();k++)
		{
			rover=x.ffloors[k];
			
			if ((rover->flags&(FF_EXISTS|FF_RENDERPLANES|FF_THISINSIDE))==(FF_EXISTS|FF_RENDERPLANES))
			{
				if (rover->flags&FF_FOG && gl_fixedcolormap) continue;
				if (rover->flags&(FF_INVERTPLANES|FF_BOTHPLANES))
				{
					fixed_t ff_top=rover->top.plane->ZatPoint(CenterSpot(sector));
					if (ff_top<lastceilingheight)
					{
						if (FIXED2FLOAT(viewz) <= rover->top.plane->ZatPoint(FIXED2FLOAT(viewx), FIXED2FLOAT(viewy)))
						{
							SetFrom3DFloor(rover, true, !!(rover->flags&FF_FOG));
							Colormap.FadeColor=frontsector->ColorMap->Fade;
							Process(rover->top.model, rover->top.isceiling, !!(rover->flags&FF_FOG));
						}
						lastceilingheight=ff_top;
					}
				}
				if (!(rover->flags&FF_INVERTPLANES))
				{
					fixed_t ff_bottom=rover->bottom.plane->ZatPoint(CenterSpot(sector));
					if (ff_bottom<lastceilingheight)
					{
						if (FIXED2FLOAT(viewz)<=rover->bottom.plane->ZatPoint(FIXED2FLOAT(viewx), FIXED2FLOAT(viewy)))
						{
							SetFrom3DFloor(rover, false, !(rover->flags&FF_FOG));
							Colormap.FadeColor=frontsector->ColorMap->Fade;
							Process(rover->bottom.model, rover->bottom.isceiling, !!(rover->flags&FF_FOG));
						}
						lastceilingheight=ff_bottom;
						if (rover->alpha<255) lastceilingheight++;
					}
				}
			}
		}
			  
		ceiling=false;
		for(k=x.ffloors.Size()-1;k>=0;k--)
		{
			rover=x.ffloors[k];
			
			if ((rover->flags&(FF_EXISTS|FF_RENDERPLANES|FF_THISINSIDE))==(FF_EXISTS|FF_RENDERPLANES))
			{
				if (rover->flags&FF_FOG && gl_fixedcolormap) continue;
				if (rover->flags&(FF_INVERTPLANES|FF_BOTHPLANES))
				{
					fixed_t ff_bottom=rover->bottom.plane->ZatPoint(CenterSpot(sector));
					if (ff_bottom>lastfloorheight || (rover->flags&FF_FIX))
					{
						if (FIXED2FLOAT(viewz) >= rover->bottom.plane->ZatPoint(FIXED2FLOAT(viewx), FIXED2FLOAT(viewy)))
						{
							SetFrom3DFloor(rover, false, !(rover->flags&FF_FOG));
							Colormap.FadeColor=frontsector->ColorMap->Fade;

							if (rover->flags&FF_FIX)
							{
								lightlevel = gl_ClampLight(rover->model->lightlevel);
								Colormap = rover->GetColormap();
							}

							Process(rover->bottom.model, rover->bottom.isceiling, !!(rover->flags&FF_FOG));
						}
						lastfloorheight=ff_bottom;
					}
				}
				if (!(rover->flags&FF_INVERTPLANES))
				{
					fixed_t ff_top=rover->top.plane->ZatPoint(CenterSpot(sector));
					if (ff_top>lastfloorheight)
					{
						if (FIXED2FLOAT(viewz) >= rover->top.plane->ZatPoint(FIXED2FLOAT(viewx), FIXED2FLOAT(viewy)))
						{
							SetFrom3DFloor(rover, true, !!(rover->flags&FF_FOG));
							Colormap.FadeColor=frontsector->ColorMap->Fade;
							Process(rover->top.model, rover->top.isceiling, !!(rover->flags&FF_FOG));
						}
						lastfloorheight=ff_top;
						if (rover->alpha<255) lastfloorheight--;
					}
				}
			}
		}
	}
}

