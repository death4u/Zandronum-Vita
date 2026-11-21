/*
** gl_walls_draw.cpp
** Wall rendering
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
#include "p_local.h"
#include "p_lnspec.h"
#include "a_sharedglobal.h"
#include "gl/gl_functions.h"

#include "gl/system/gl_interface.h"
#include "gl/system/gl_cvars.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/data/gl_data.h"
#include "gl/dynlights/gl_dynlight.h"
#include "gl/dynlights/gl_glow.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/scene/gl_portal.h"
#include "gl/shaders/gl_shader.h"
#include "gl/textures/gl_material.h"
#include "gl/utility/gl_clock.h"
#include "gl/utility/gl_templates.h"
#include "gl/scene/gl_vitawallbatch.h"
#include "gl/textures/gl_hwtexture.h"
#include "vita/vita_system.h"
#include "vita/vita_perf.h"

EXTERN_CVAR(Bool, gl_seamless)

//==========================================================================
//
// Sets up the texture coordinates for one light to be rendered
//
//==========================================================================
bool GLWall::PrepareLight(texcoord * tcs, ADynamicLight * light)
{
	float vtx[]={glseg.x1,zbottom[0],glseg.y1, glseg.x1,ztop[0],glseg.y1, glseg.x2,ztop[1],glseg.y2, glseg.x2,zbottom[1],glseg.y2};
	Plane p;
	Vector nearPt, up, right;
	float scale;

	p.Init(vtx,4);

	if (!p.ValidNormal()) 
	{
		return false;
	}

	if (!gl_SetupLight(p, light, nearPt, up, right, scale, Colormap.colormap, true, !!(flags&GLWF_FOGGY))) 
	{
		return false;
	}

	if (tcs != NULL)
	{
		Vector t1;
		int outcnt[4]={0,0,0,0};

		for(int i=0;i<4;i++)
		{
			t1.Set(&vtx[i*3]);
			Vector nearToVert = t1 - nearPt;
			tcs[i].u = (nearToVert.Dot(right) * scale) + 0.5f;
			tcs[i].v = (nearToVert.Dot(up) * scale) + 0.5f;

			// quick check whether the light touches this polygon
			if (tcs[i].u<0) outcnt[0]++;
			if (tcs[i].u>1) outcnt[1]++;
			if (tcs[i].v<0) outcnt[2]++;
			if (tcs[i].v>1) outcnt[3]++;

		}
		// The light doesn't touch this polygon
		if (outcnt[0]==4 || outcnt[1]==4 || outcnt[2]==4 || outcnt[3]==4) return false;
	}

	draw_dlight++;
	return true;
}

//==========================================================================
//
// Collect lights for shader
//
//==========================================================================
FDynLightData lightdata;

void GLWall::SetupLights()
{
	// [AK] Take care of gl_lights_size and ZADF_FORCE_VIDEO_DEFAULTS.
	OVERRIDE_LIGHTS_SIZE_IF_NECESSARY

	float vtx[]={glseg.x1,zbottom[0],glseg.y1, glseg.x1,ztop[0],glseg.y1, glseg.x2,ztop[1],glseg.y2, glseg.x2,zbottom[1],glseg.y2};
	Plane p;

	lightdata.Clear();
	p.Init(vtx,4);

	if (!p.ValidNormal()) 
	{
		return;
	}
	for(int i=0;i<2;i++)
	{
		FLightNode *node;
		if (seg->sidedef == NULL)
		{
			node = NULL;
		}
		else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
		{
			node = seg->sidedef->lighthead[i];
		}
		else if (sub)
		{
			// Polobject segs cannot be checked per sidedef so use the subsector instead.
			node = sub->lighthead[i];
		}
		else node = NULL;

		// Iterate through all dynamic lights which touch this wall and render them
		while (node)
		{
			if (!(node->lightsource->flags2&MF2_DORMANT))
			{
				iter_dlight++;

				Vector fn, pos;

				float x = FIXED2FLOAT(node->lightsource->x);
				float y = FIXED2FLOAT(node->lightsource->y);
				float z = FIXED2FLOAT(node->lightsource->z);
				float dist = fabsf(p.DistToPoint(x, z, y));
				float radius = (node->lightsource->GetRadius() * gl_lights_size);
				float scale = 1.0f / ((2.f * radius) - dist);

				if (radius > 0.f && dist < radius)
				{
					Vector nearPt, up, right;

					pos.Set(x,z,y);
					fn=p.Normal();
					fn.GetRightUp(right, up);

					Vector tmpVec = fn * dist;
					nearPt = pos + tmpVec;

					Vector t1;
					int outcnt[4]={0,0,0,0};
					texcoord tcs[4];

					// do a quick check whether the light touches this polygon
					for(int i=0;i<4;i++)
					{
						t1.Set(&vtx[i*3]);
						Vector nearToVert = t1 - nearPt;
						tcs[i].u = (nearToVert.Dot(right) * scale) + 0.5f;
						tcs[i].v = (nearToVert.Dot(up) * scale) + 0.5f;

						if (tcs[i].u<0) outcnt[0]++;
						if (tcs[i].u>1) outcnt[1]++;
						if (tcs[i].v<0) outcnt[2]++;
						if (tcs[i].v>1) outcnt[3]++;

					}
					if (outcnt[0]!=4 && outcnt[1]!=4 && outcnt[2]!=4 && outcnt[3]!=4) 
					{
						gl_GetLight(p, node->lightsource, Colormap.colormap, true, false, lightdata);
					}
				}
			}
			node = node->nextLight;
		}
	}
	int numlights[3];

	lightdata.Combine(numlights, gl.MaxLights());
	if (numlights[2] > 0)
	{
		draw_dlight+=numlights[2]/2;
		gl_RenderState.EnableLight(true);
		gl_RenderState.SetLights(numlights, &lightdata.arrays[0][0]);
	}
}

//==========================================================================
//
// General purpose wall rendering function
// everything goes through here
//
//==========================================================================

void GLWall::RenderWall(int textured, float * color2, ADynamicLight * light)
{
	texcoord tcs[4];
	bool glowing;
	bool split = (gl_seamless && !(textured&4) && seg->sidedef != NULL && !(seg->sidedef->Flags & WALLF_POLYOBJ));

	if (!light)
	{
		tcs[0]=lolft;
		tcs[1]=uplft;
		tcs[2]=uprgt;
		tcs[3]=lorgt;
		glowing = !!(flags&GLWF_GLOW) && (textured & 2);
	}
	else
	{
		if (!PrepareLight(tcs, light)) return;
		glowing = false;
	}

	if (glowing) gl_RenderState.SetGlowParams(topglowcolor, bottomglowcolor);

#ifdef __vita__
	if (!VitaWalls.Collecting)
#endif
	gl_RenderState.Apply();

	// the rest of the code is identical for textured rendering and lights

	glBegin(GL_TRIANGLE_FAN);

	// lower left corner
	if (glowing) glVertexAttrib2f(VATTR_GLOWDISTANCE, zceil[0] - zbottom[0], zbottom[0] - zfloor[0]);
	if (textured&1) glTexCoord2f(tcs[0].u,tcs[0].v);
	glVertex3f(glseg.x1,zbottom[0],glseg.y1);

	if (split && glseg.fracleft==0) SplitLeftEdge(tcs, glowing);

	// upper left corner
	if (glowing) glVertexAttrib2f(VATTR_GLOWDISTANCE, zceil[0] - ztop[0], ztop[0] - zfloor[0]);
	if (textured&1) glTexCoord2f(tcs[1].u,tcs[1].v);
	glVertex3f(glseg.x1,ztop[0],glseg.y1);

	if (split && !(flags & GLWF_NOSPLITUPPER)) SplitUpperEdge(tcs, glowing);

	// color for right side
	if (color2) glColor4fv(color2);

	// upper right corner
	if (glowing) glVertexAttrib2f(VATTR_GLOWDISTANCE, zceil[1] - ztop[1], ztop[1] - zfloor[1]);
	if (textured&1) glTexCoord2f(tcs[2].u,tcs[2].v);
	glVertex3f(glseg.x2,ztop[1],glseg.y2);

	if (split && glseg.fracright==1) SplitRightEdge(tcs, glowing);

	// lower right corner
	if (glowing) glVertexAttrib2f(VATTR_GLOWDISTANCE, zceil[1] - zbottom[1], zbottom[1] - zfloor[1]);
	if (textured&1) glTexCoord2f(tcs[3].u,tcs[3].v); 
	glVertex3f(glseg.x2,zbottom[1],glseg.y2);

	if (split && !(flags & GLWF_NOSPLITLOWER)) SplitLowerEdge(tcs, glowing);

	glEnd();

	vertexcount+=4;

}

//==========================================================================
//
// 
//
//==========================================================================

void GLWall::RenderFogBoundary()
{
	// [BB/EP] Take care of gl_fogmode and ZADF_FORCE_VIDEO_DEFAULTS.
	OVERRIDE_FOGMODE_IF_NECESSARY

	if (gl_fogmode && gl_fixedcolormap == 0)
	{
		// with shaders this can be done properly
		if (gl.shadermodel == 4 || (gl.shadermodel == 3 && gl_fog_shader))
		{
			int rel = rellight + getExtraLight();
			gl_SetFog(lightlevel, rel, &Colormap, false);
			gl_RenderState.SetEffect(EFF_FOGBOUNDARY);
			gl_RenderState.EnableAlphaTest(false);
			RenderWall(0, NULL);
			gl_RenderState.EnableAlphaTest(true);
			gl_RenderState.SetEffect(EFF_NONE);
		}
		else
		{
			// otherwise some approximation is needed. This won't look as good
			// as the shader version but it's an acceptable compromise.
			float fogdensity=gl_GetFogDensity(lightlevel, Colormap.FadeColor);

			float xcamera=FIXED2FLOAT(viewx);
			float ycamera=FIXED2FLOAT(viewy);

			float dist1=Dist2(xcamera,ycamera, glseg.x1,glseg.y1);
			float dist2=Dist2(xcamera,ycamera, glseg.x2,glseg.y2);


			// these values were determined by trial and error and are scale dependent!
			float fogd1=(0.95f-exp(-fogdensity*dist1/62500.f)) * 1.05f;
			float fogd2=(0.95f-exp(-fogdensity*dist2/62500.f)) * 1.05f;

			gl_ModifyColor(Colormap.FadeColor.r, Colormap.FadeColor.g, Colormap.FadeColor.b, Colormap.colormap);
			float fc[4]={Colormap.FadeColor.r/255.0f,Colormap.FadeColor.g/255.0f,Colormap.FadeColor.b/255.0f,fogd2};

			gl_RenderState.EnableTexture(false);
			gl_RenderState.EnableFog(false);
			gl_RenderState.AlphaFunc(GL_GREATER,0);
			glDepthFunc(GL_LEQUAL);
			glColor4f(fc[0],fc[1],fc[2], fogd1);
			if (glset.lightmode == 8) glVertexAttrib1f(VATTR_LIGHTLEVEL, 1.0); // Korshun.

			flags &= ~GLWF_GLOW;
			RenderWall(4,fc);

			glDepthFunc(GL_LESS);
			gl_RenderState.EnableFog(true);
			gl_RenderState.AlphaFunc(GL_GEQUAL,0.5f);
			gl_RenderState.EnableTexture(true);
		}
	}
}


//==========================================================================
//
// 
//
//==========================================================================
void GLWall::RenderMirrorSurface()
{
	if (GLRenderer->mirrortexture == NULL) return;

	// For the sphere map effect we need a normal of the mirror surface,
	Vector v(glseg.y2-glseg.y1, 0 ,-glseg.x2+glseg.x1);
	v.Normalize();
	glNormal3fv(&v[0]);

	// Use sphere mapping for this
	gl_RenderState.SetEffect(EFF_SPHEREMAP);

	gl_SetColor(lightlevel, 0, &Colormap ,0.1f);
	gl_RenderState.BlendFunc(GL_SRC_ALPHA,GL_ONE);
	gl_RenderState.AlphaFunc(GL_GREATER,0);
	glDepthFunc(GL_LEQUAL);
	gl_SetFog(lightlevel, getExtraLight(), &Colormap, true);

	FMaterial * pat=FMaterial::ValidateTexture(GLRenderer->mirrortexture);
	pat->BindPatch(Colormap.colormap, 0);

	flags &= ~GLWF_GLOW;
	//flags |= GLWF_NOSHADER;
	RenderWall(0,NULL);

	gl_RenderState.SetEffect(EFF_NONE);

	// Restore the defaults for the translucent pass
	gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	gl_RenderState.AlphaFunc(GL_GEQUAL,0.5f*gl_mask_sprite_threshold);
	glDepthFunc(GL_LESS);

	// This is drawn in the translucent pass which is done after the decal pass
	// As a result the decals have to be drawn here.
	if (seg->sidedef->AttachedDecals)
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(-1.0f, -128.0f);
		glDepthMask(false);
		DoDrawDecals();
		glDepthMask(true);
		glPolygonOffset(0.0f, 0.0f);
		glDisable(GL_POLYGON_OFFSET_FILL);
		gl_RenderState.SetTextureMode(TM_MODULATE);
		gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
}


//==========================================================================
//
// 
//
//==========================================================================

void GLWall::RenderTranslucentWall()
{
	bool transparent = gltexture? gltexture->GetTransparent() : false;
	
	// currently the only modes possible are solid, additive or translucent
	// and until that changes I won't fix this code for the new blending modes!
	bool isadditive = RenderStyle == STYLE_Add;

	if (!transparent) gl_RenderState.AlphaFunc(GL_GEQUAL,gl_mask_threshold*fabs(alpha));
	else gl_RenderState.EnableAlphaTest(false);
	if (isadditive) gl_RenderState.BlendFunc(GL_SRC_ALPHA,GL_ONE);

	int extra;
	if (gltexture) 
	{
		if (flags&GLWF_FOGGY) gl_RenderState.EnableBrightmap(false);
		gl_RenderState.EnableGlow(!!(flags & GLWF_GLOW));
		gltexture->Bind(Colormap.colormap, flags, 0);
		extra = getExtraLight();
	}
	else 
	{
		gl_RenderState.EnableTexture(false);
		extra = 0;
	}

	gl_SetColor(lightlevel, extra, &Colormap, fabsf(alpha));
	if (type!=RENDERWALL_M2SNF) gl_SetFog(lightlevel, extra, &Colormap, isadditive);
	else gl_SetFog(255, 0, NULL, false);

	RenderWall(5,NULL);

	// restore default settings
	if (isadditive) gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	if (transparent) gl_RenderState.EnableAlphaTest(true);

	if (!gltexture)	
	{
		gl_RenderState.EnableTexture(true);
	}
	gl_RenderState.EnableBrightmap(true);
	gl_RenderState.EnableGlow(false);
}

//==========================================================================
//
// 
//
//==========================================================================
#ifdef __vita__
FVitaWallBatch VitaWalls;

unsigned Vita_CurColor = 0xffffffff;

static TArray<FVitaBucket *> VitaBuckets;	// pool, reused every frame
static unsigned VitaBucketsUsed;
static int VitaBucketHash[512];			// index + 1 into VitaBuckets, 0 = empty
static unsigned VitaBucketGen[512], VitaGen = 1;

static void Vita_DrawBucket(FVitaBucket *b)
{
	const unsigned total = b->Xyz.Size() / 3;
	if (total == 0) return;
	gl_RenderState.VitaSetFog(b->FogOn, b->FogColor, b->FogDensity);
	gl_RenderState.VitaSetBlendEquation(b->BlendEq);
	if (b->Tex != NULL)
	{
		gl_RenderState.EnableGlow(false);
		b->Tex->Bind(b->CmColormap, b->Flags, 0);
	}
	gl_RenderState.Apply();
	const bool two = b->LightTex && b->Uv2.Size() == b->Uv.Size();
	if (two)
	{
		glActiveTexture(GL_TEXTURE1);
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, Vita_LightTexId);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glClientActiveTexture(GL_TEXTURE1);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glClientActiveTexture(GL_TEXTURE0);
		glActiveTexture(GL_TEXTURE0);
	}
	// vitaGL draws arrays through a 16-bit index list of 0xC000 entries:
	// bigger draws read past it (garbage, crashes), so draw in pieces.
	const unsigned piece = 0xC000 / 3 * 3 - 3;
	for (unsigned start = 0; start < total; start += piece)
	{
		const unsigned n = total - start < piece ? total - start : piece;
		glVertexPointer(3, GL_FLOAT, 0, &b->Xyz[start * 3]);
		glTexCoordPointer(2, GL_FLOAT, 0, &b->Uv[start * 2]);
		if (two)
		{
			glClientActiveTexture(GL_TEXTURE1);
			glTexCoordPointer(2, GL_FLOAT, 0, &b->Uv2[start * 2]);
			glClientActiveTexture(GL_TEXTURE0);
		}
		glColorPointer(4, GL_UNSIGNED_BYTE, 0, &b->Rgba[start]);
		glDrawArrays(GL_TRIANGLES, 0, n);
	}
	if (two)
	{
		glClientActiveTexture(GL_TEXTURE1);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glClientActiveTexture(GL_TEXTURE0);
		glActiveTexture(GL_TEXTURE1);
		glDisable(GL_TEXTURE_2D);
		glActiveTexture(GL_TEXTURE0);
	}
}

unsigned Vita_LightTexId;

// [Vita] Light passes per wall/flat: lights far from the camera are left
// out, and only the strongest few lights of a surface are drawn (0042: up to
// 4900 light passes a frame on Lockup). vita_maxsurfacelights 0 /
// vita_lightdistance 0 = no limit.
CVAR(Int, vita_maxsurfacelights, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vita_lightdistance, 1536, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
static inline float Vita_LightScore(ADynamicLight *l)
{
	// bigger and closer to the camera first
	const float dx = FIXED2FLOAT(l->x - viewx), dy = FIXED2FLOAT(l->y - viewy), dz = FIXED2FLOAT(l->z - viewz);
	return l->GetRadius() * 4.f - sqrtf(dx * dx + dy * dy + dz * dz);
}
bool Vita_LightAllowed(FLightNode *head, ADynamicLight *light)
{
	if (vita_lightdistance > 0)
	{
		const float dx = FIXED2FLOAT(light->x - viewx), dy = FIXED2FLOAT(light->y - viewy), dz = FIXED2FLOAT(light->z - viewz);
		const float maxd = (float)vita_lightdistance + light->GetRadius();
		if (dx * dx + dy * dy + dz * dz > maxd * maxd) return false;
	}
	if (vita_maxsurfacelights <= 0) return true;
	const float mine = Vita_LightScore(light);
	int better = 0;
	for (FLightNode *n = head; n != NULL; n = n->nextLight)
	{
		ADynamicLight *o = n->lightsource;
		if (o == NULL || o == light || (o->flags2 & MF2_DORMANT)) continue;
		if (vita_lightdistance > 0)
		{
			const float dx = FIXED2FLOAT(o->x - viewx), dy = FIXED2FLOAT(o->y - viewy), dz = FIXED2FLOAT(o->z - viewz);
			const float maxd = (float)vita_lightdistance + o->GetRadius();
			if (dx * dx + dy * dy + dz * dz > maxd * maxd) continue;
		}
		const float sc = Vita_LightScore(o);
		if (sc > mine || (sc == mine && o < light)) { if (++better >= vita_maxsurfacelights) return false; }
	}
	return true;
}
CVAR(Bool, vita_lighttexpass, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Light pass fan (see VitaWalls.LightPassMode): light UVs from the fan,
// surface UVs from the position.
void Vita_EmitLightPassFan()
{
	FVitaWallBatch &w = VitaWalls;
	const unsigned n = w.FanXyz.Size() / 3;
	float su[16], sv[16];
	if (n > 16 || n < 3) return;
	for (unsigned i = 0; i < n; i++)
	{
		const float x = w.FanXyz[i * 3], z = w.FanXyz[i * 3 + 1], y = w.FanXyz[i * 3 + 2];
		if (w.LightPassMode == 1)
		{
			float s = ((x - w.WallX1) * w.WallDX + (y - w.WallY1) * w.WallDY) * w.WallInvLen2;
			s = s < 0 ? 0 : s > 1 ? 1 : s;
			const float zt = w.WallZTop[0] + (w.WallZTop[1] - w.WallZTop[0]) * s;
			const float zb = w.WallZBot[0] + (w.WallZBot[1] - w.WallZBot[0]) * s;
			float t = zt != zb ? (zt - z) / (zt - zb) : 0.f;
			const float ut = w.WallUTop[0] + (w.WallUTop[1] - w.WallUTop[0]) * s, vt = w.WallVTop[0] + (w.WallVTop[1] - w.WallVTop[0]) * s;
			const float ub = w.WallUBot[0] + (w.WallUBot[1] - w.WallUBot[0]) * s, vb = w.WallVBot[0] + (w.WallVBot[1] - w.WallVBot[0]) * s;
			su[i] = ut + (ub - ut) * t;
			sv[i] = vt + (vb - vt) * t;
		}
		else
		{
			const float u = x / 64.f, v = -y / 64.f;
			su[i] = w.FlatA * u + w.FlatB * v + w.FlatC;
			sv[i] = w.FlatD * u + w.FlatE * v + w.FlatF;
		}
	}
	FVitaBucket *b = w.Cur;
	for (unsigned k = 1; k + 1 < n; k++)
	{
		const unsigned idx[3] = { 0, k, k + 1 };
		for (int j = 0; j < 3; j++)
		{
			const unsigned i = idx[j];
			b->Xyz.Push(w.FanXyz[i * 3]); b->Xyz.Push(w.FanXyz[i * 3 + 1]); b->Xyz.Push(w.FanXyz[i * 3 + 2]);
			b->Uv.Push(su[i]); b->Uv.Push(sv[i]);
			b->Uv2.Push(w.FanUv[i * 2]); b->Uv2.Push(w.FanUv[i * 2 + 1]);
			b->Rgba.Push(w.Color);
		}
	}
}

void Vita_FlushWalls()
{
	if (VitaWalls.Open && VitaBucketsUsed > 0)
	{
		VITA_PERF(VPERF_BATCHDRAW);
		bool fogon; PalEntry fc; float fd;
		gl_RenderState.VitaGetFog(fogon, fc, fd);
		const int eq = gl_RenderState.VitaGetBlendEquation();
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glEnableClientState(GL_COLOR_ARRAY);
		// buckets using the caller's texture state first, before any bind
		for (unsigned i = 0; i < VitaBucketsUsed; i++) if (VitaBuckets[i]->Tex == NULL) Vita_DrawBucket(VitaBuckets[i]);
		bool lighttex = false;
		for (unsigned i = 0; i < VitaBucketsUsed; i++) if (VitaBuckets[i]->Tex != NULL) { Vita_DrawBucket(VitaBuckets[i]); lighttex |= VitaBuckets[i]->LightTex; }
		if (lighttex)
		{
			// the light pass's other surfaces expect the light texture on unit 0
			glBindTexture(GL_TEXTURE_2D, Vita_LightTexId);
			FHardwareTexture::lastbound[0] = Vita_LightTexId;
		}
		glDisableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glDisableClientState(GL_COLOR_ARRAY);
		// the current colour is undefined after a colour array
		glColor4ub(Vita_CurColor & 0xff, (Vita_CurColor >> 8) & 0xff, (Vita_CurColor >> 16) & 0xff, Vita_CurColor >> 24);
		gl_RenderState.VitaSetFog(fogon, fc, fd);
		gl_RenderState.VitaSetBlendEquation(eq);
		for (unsigned i = 0; i < VitaBucketsUsed; i++)
		{
			VitaBuckets[i]->Xyz.Clear();
			VitaBuckets[i]->Uv2.Clear();
			VitaBuckets[i]->Uv.Clear();
			VitaBuckets[i]->Rgba.Clear();
		}
	}
	VitaBucketsUsed = 0;
	VitaGen++;
	VitaWalls.Open = false;
	VitaWalls.Cur = NULL;
}

static inline unsigned Vita_PackColor(float r, float g, float b)
{
	int ir = (int)(r * 255.f + 0.5f), ig = (int)(g * 255.f + 0.5f), ib = (int)(b * 255.f + 0.5f);
	ir = clamp(ir, 0, 255); ig = clamp(ig, 0, 255); ib = clamp(ib, 0, 255);
	return (unsigned)ir | ((unsigned)ig << 8) | ((unsigned)ib << 16) | 0xff000000u;
}

void Vita_BatchSurface(FMaterial *tex, int cm, int flags, bool usecurcolor, float r, float g, float b)
{
	VitaWalls.NextTex = tex;
	VitaWalls.NextCm = cm;
	VitaWalls.NextFlags = flags;
	VitaWalls.UseCurColor = usecurcolor;
	if (!usecurcolor) VitaWalls.NextColor = Vita_PackColor(r, g, b);
}

void Vita_BatchCheck()
{
	bool on; PalEntry fc; float fd;
	gl_RenderState.VitaGetFog(on, fc, fd);
	const int eq = gl_RenderState.VitaGetBlendEquation();
	FMaterial *tex = VitaWalls.NextTex;
	const int cm = tex != NULL ? VitaWalls.NextCm : 0, flags = tex != NULL ? VitaWalls.NextFlags : 0;
	if (VitaWalls.ForceNoFog) on = false;
	if (!on) { fc = 0; fd = 0; }
	const bool lt = VitaWalls.LightPassMode != 0;
	FVitaBucket *cur = VitaWalls.Cur;
	if (cur == NULL || cur->Tex != tex || cur->CmColormap != cm || cur->Flags != flags || cur->FogOn != on
		|| cur->FogColor != fc || cur->FogDensity != fd || cur->BlendEq != eq || cur->LightTex != lt)
	{
		unsigned h = (unsigned)(((uintptr_t)tex >> 4) * 2654435761u) ^ (unsigned)cm * 31 ^ (unsigned)flags * 17 ^ (unsigned)fc.d * 7 ^ (unsigned)(fd * 100000.f) * 13 ^ (unsigned)eq ^ (on ? 0x5555 : 0) ^ (lt ? 0xa5a5 : 0);
		h &= 511;
		cur = NULL;
		for (int probe = 0; probe < 512; probe++, h = (h + 1) & 511)
		{
			if (VitaBucketGen[h] != VitaGen) break;	// empty slot
			FVitaBucket *b = VitaBuckets[VitaBucketHash[h] - 1];
			if (b->Tex == tex && b->CmColormap == cm && b->Flags == flags && b->FogOn == on && b->FogColor == fc && b->FogDensity == fd && b->BlendEq == eq && b->LightTex == lt) { cur = b; break; }
		}
		if (cur == NULL)
		{
			if (VitaBucketsUsed >= 400)
			{
				// too many states in one list: draw what we have
				Vita_FlushWalls();
				Vita_BatchCheck();
				return;
			}
			if (VitaBucketsUsed == VitaBuckets.Size()) VitaBuckets.Push(new FVitaBucket);
			cur = VitaBuckets[VitaBucketsUsed++];
			cur->Tex = tex; cur->CmColormap = cm; cur->Flags = flags;
			cur->FogOn = on; cur->FogColor = fc; cur->FogDensity = fd; cur->BlendEq = eq; cur->LightTex = lt;
			VitaBucketGen[h] = VitaGen;
			VitaBucketHash[h] = VitaBucketsUsed;
		}
		VitaWalls.Cur = cur;
		VitaWalls.Open = true;
	}
	VitaWalls.Color = VitaWalls.UseCurColor ? Vita_CurColor : VitaWalls.NextColor;
}

CVAR(Bool, gl_vita_batchwalls, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

//==========================================================================
//
// [Vita] Dynamic lights per vertex
//
//==========================================================================
CVAR(Bool, vita_vertexlights, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vita_vertexlights_split, 96.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// split edges longer than this near a light

bool Vita_VertexLightsOn()
{
	return vita_vertexlights && gl_lights && !gl_fixedcolormap && gl_vita_batchwalls;
}

// The multipass lights drew glstuff/gllight.png (radial profile below) on
// a disc of radius r - h/2 around the point nearest the light, with the
// colour scaled by 1 - h/r (h = the light's distance from the plane).
static const float VitaLightProfile[18] = { 0.996f, 0.984f, 0.965f, 0.945f, 0.925f, 0.906f, 0.882f, 0.835f, 0.745f, 0.627f, 0.502f, 0.388f, 0.298f, 0.208f, 0.118f, 0.047f, 0.f, 0.f };
struct FVitaPolyLight { float x, y, z, h2, disc2, invdisc, r, g, b; bool sub; };
static FVitaPolyLight VitaPL[24];
static int VitaPLCount;
static float VitaPLFog, VitaPLCamX, VitaPLCamY, VitaPLCamZ;	// GL_EXP density baked into the vertex colours (0 = none)

CVAR(Bool, vita_vertexlights_debug, false, 0)
static unsigned VitaDbgCalls, VitaDbgLit, VitaDbgNodes, VitaDbgRejDist, VitaDbgRejSide;
bool Vita_SetupPolyLights(FLightNode *head0, FLightNode *head1, const float *p0, const float *p1, const float *p2)
{
	OVERRIDE_LIGHTS_SIZE_IF_NECESSARY
	VitaPLCount = 0;
	if (vita_vertexlights_debug && ++VitaDbgCalls % 2000 == 0)
		Vita_BootLog("vertexlights: %u polys, %u lit, %u light nodes, %u too far, %u wrong side", VitaDbgCalls, VitaDbgLit, VitaDbgNodes, VitaDbgRejDist, VitaDbgRejSide);
	// plane through p0 p1 p2
	float ax = p1[0] - p0[0], ay = p1[1] - p0[1], az = p1[2] - p0[2];
	float bx = p2[0] - p0[0], by = p2[1] - p0[1], bz = p2[2] - p0[2];
	float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
	float len = sqrtf(nx * nx + ny * ny + nz * nz);
	if (len < 1e-4f) return false;
	nx /= len; ny /= len; nz /= len;
	const float d = -(nx * p0[0] + ny * p0[1] + nz * p0[2]);
	// the side the camera is on is the lit side
	const float camside = nx * FIXED2FLOAT(viewx) + ny * FIXED2FLOAT(viewz) + nz * FIXED2FLOAT(viewy) + d;
	for (int list = 0; list < 2; list++)
	{
		for (FLightNode *node = list ? head1 : head0; node != NULL; node = node->nextLight)
		{
			ADynamicLight *light = node->lightsource;
			if (light == NULL || (light->flags2 & MF2_DORMANT)) continue;
			const float radius = light->GetRadius() * gl_lights_size;
			if (radius <= 0.f) continue;
			const float lx = FIXED2FLOAT(light->x), ly = FIXED2FLOAT(light->z), lz = FIXED2FLOAT(light->y);
			const float side = nx * lx + ny * ly + nz * lz + d;
			VitaDbgNodes++;
			if (fabsf(side) > radius) { VitaDbgRejDist++; continue; }
			if (gl_lights_checkside && side * camside < 0) { VitaDbgRejSide++; continue; }
			if (light->owned && light->target != NULL && !light->target->IsVisibleToPlayer()) continue;
			float cs = (gl_lights_additive || (light->flags4 & MF4_ADDITIVE) || list == 1) ? 0.2f : 1.0f;
			float r = light->GetRed() / 255.0f * cs * gl_lights_intensity;
			float g = light->GetGreen() / 255.0f * cs * gl_lights_intensity;
			float b = light->GetBlue() / 255.0f * cs * gl_lights_intensity;
			const bool sub = light->IsSubtractive();
			if (sub)
			{
				const float l = sqrtf(r * r + g * g + b * b);
				r = l - r; g = l - g; b = l - b;
			}
			if (VitaPLCount == 24) break;
			const float h = fabsf(side);
			const float fade = 1.f - h / radius;
			const float disc = radius - h * 0.5f;
			FVitaPolyLight &pl = VitaPL[VitaPLCount++];
			pl.x = lx; pl.y = ly; pl.z = lz; pl.h2 = h * h; pl.disc2 = disc * disc; pl.invdisc = 1.f / disc;
			pl.r = r * fade; pl.g = g * fade; pl.b = b * fade; pl.sub = sub;
		}
	}
	if (VitaPLCount > 0) VitaDbgLit++;
	if (VitaPLCount == 0) return false;
	// The multipass lights weren't fogged (only the sector light was): with
	// black fog, the fog is applied to the sector light here and the
	// polygon goes into a bucket without fog.
	bool fogon; PalEntry fc; float fd;
	gl_RenderState.VitaGetFog(fogon, fc, fd);
	VitaPLFog = 0.f;
	if (fogon && (fc.d & 0xffffff) == 0 && fd > 0.f)
	{
		VitaPLFog = fd / 64000.f;
		VitaPLCamX = FIXED2FLOAT(viewx); VitaPLCamY = FIXED2FLOAT(viewz); VitaPLCamZ = FIXED2FLOAT(viewy);
		VitaWalls.ForceNoFog = true;
		Vita_BatchCheck();
	}
	return true;
}

void Vita_EndPolyLights()
{
	if (VitaWalls.ForceNoFog)
	{
		VitaWalls.ForceNoFog = false;
		Vita_BatchCheck();
	}
}

static inline unsigned Vita_LitColor(const float *p)
{
	const unsigned base = VitaWalls.Color;
	float r = (base & 0xff) * (1.f / 255.f), g = ((base >> 8) & 0xff) * (1.f / 255.f), b = ((base >> 16) & 0xff) * (1.f / 255.f);
	if (VitaPLFog > 0.f)
	{
		const float cx = p[0] - VitaPLCamX, cy = p[1] - VitaPLCamY, cz = p[2] - VitaPLCamZ;
		const float f = expf(-VitaPLFog * sqrtf(cx * cx + cy * cy + cz * cz));
		r *= f; g *= f; b *= f;
	}
	for (int i = 0; i < VitaPLCount; i++)
	{
		const FVitaPolyLight &l = VitaPL[i];
		const float dx = p[0] - l.x, dy = p[1] - l.y, dz = p[2] - l.z;
		float s2 = dx * dx + dy * dy + dz * dz - l.h2;	// squared distance along the plane
		if (s2 >= l.disc2) continue;
		if (s2 < 0) s2 = 0;
		const float t = sqrtf(s2) * l.invdisc * 16.f;
		const int ti = (int)t;
		const float f = VitaLightProfile[ti] + (VitaLightProfile[ti + 1] - VitaLightProfile[ti]) * (t - ti);
		if (l.sub) { r -= l.r * f; g -= l.g * f; b -= l.b * f; }
		else { r += l.r * f; g += l.g * f; b += l.b * f; }
	}
	int ir = (int)(r * 255.f + 0.5f), ig = (int)(g * 255.f + 0.5f), ib = (int)(b * 255.f + 0.5f);
	ir = ir < 0 ? 0 : ir > 255 ? 255 : ir; ig = ig < 0 ? 0 : ig > 255 ? 255 : ig; ib = ib < 0 ? 0 : ib > 255 ? 255 : ib;
	return (unsigned)ir | ((unsigned)ig << 8) | ((unsigned)ib << 16) | (base & 0xff000000u);
}

// does the triangle's bounding box come within reach of a light?
static bool Vita_TriNearLight(const float *p0, const float *p1, const float *p2)
{
	float mn[3], mx[3];
	for (int k = 0; k < 3; k++)
	{
		mn[k] = MIN(p0[k], MIN(p1[k], p2[k]));
		mx[k] = MAX(p0[k], MAX(p1[k], p2[k]));
	}
	for (int i = 0; i < VitaPLCount; i++)
	{
		const FVitaPolyLight &l = VitaPL[i];
		const float c[3] = { l.x, l.y, l.z };
		float d2 = 0;
		for (int k = 0; k < 3; k++)
		{
			const float v = c[k] < mn[k] ? mn[k] - c[k] : c[k] > mx[k] ? c[k] - mx[k] : 0.f;
			d2 += v * v;
		}
		if (d2 < l.disc2 + l.h2) return true;
	}
	return false;
}

static void Vita_LitTri(const float *p0, const float *p1, const float *p2, const float *t0, const float *t1, const float *t2, int depth)
{
	const float split = vita_vertexlights_split < 16.f ? 16.f : (float)vita_vertexlights_split;
	if (depth < 4)
	{
		float e0 = 0, e1 = 0, e2 = 0;
		for (int k = 0; k < 3; k++)
		{
			e0 += (p1[k] - p0[k]) * (p1[k] - p0[k]);
			e1 += (p2[k] - p1[k]) * (p2[k] - p1[k]);
			e2 += (p0[k] - p2[k]) * (p0[k] - p2[k]);
		}
		const float emax = MAX(e0, MAX(e1, e2));
		if (emax > split * split && Vita_TriNearLight(p0, p1, p2))
		{
			// split in four at the edge midpoints
			float m01[3], m12[3], m20[3], u01[2], u12[2], u20[2];
			for (int k = 0; k < 3; k++) { m01[k] = (p0[k] + p1[k]) * 0.5f; m12[k] = (p1[k] + p2[k]) * 0.5f; m20[k] = (p2[k] + p0[k]) * 0.5f; }
			for (int k = 0; k < 2; k++) { u01[k] = (t0[k] + t1[k]) * 0.5f; u12[k] = (t1[k] + t2[k]) * 0.5f; u20[k] = (t2[k] + t0[k]) * 0.5f; }
			Vita_LitTri(p0, m01, m20, t0, u01, u20, depth + 1);
			Vita_LitTri(m01, p1, m12, u01, t1, u12, depth + 1);
			Vita_LitTri(m20, m12, p2, u20, u12, t2, depth + 1);
			Vita_LitTri(m01, m12, m20, u01, u12, u20, depth + 1);
			return;
		}
	}
	FVitaBucket *b = VitaWalls.Cur;
	const float *ps[3] = { p0, p1, p2 }, *ts[3] = { t0, t1, t2 };
	for (int j = 0; j < 3; j++)
	{
		b->Xyz.Push(ps[j][0]); b->Xyz.Push(ps[j][1]); b->Xyz.Push(ps[j][2]);
		b->Uv.Push(ts[j][0]); b->Uv.Push(ts[j][1]);
		b->Rgba.Push(Vita_LitColor(ps[j]));
	}
}

void Vita_EmitLitTriangle(const float *p0, const float *p1, const float *p2, const float *uv0, const float *uv1, const float *uv2)
{
	Vita_LitTri(p0, p1, p2, uv0, uv1, uv2, 0);
}
#endif

void GLWall::Draw(int pass)
{
	FLightNode * node;
	int rel;

#ifdef _DEBUG
	if (seg->linedef-lines==879)
	{
		int a = 0;
	}
#endif


#ifdef __vita__
	if (pass == GLPASS_PLAIN && gl_vita_batchwalls && !(flags & GLWF_GLOW) && glset.lightmode != 8
		&& !((flags&GLWF_SKYHACK && type==RENDERWALL_M2S) || type == RENDERWALL_COLORLAYER))
	{
		rel = rellight + getExtraLight();
		float r, g, b;
		gl_SetColor(lightlevel, rel, &Colormap, &r, &g, &b);
		if (type != RENDERWALL_M2SNF) gl_SetFog(lightlevel, rel, &Colormap, false);
		else gl_SetFog(255, 0, NULL, false);
		Vita_BatchSurface(gltexture, Colormap.colormap, flags & 3, false, r, g, b);
		VitaWalls.LightHead[0] = VitaWalls.LightHead[1] = NULL;
		if (Vita_VertexLightsOn() && seg->sidedef != NULL)
		{
			if (!(seg->sidedef->Flags & WALLF_POLYOBJ)) { VitaWalls.LightHead[0] = seg->sidedef->lighthead[0]; VitaWalls.LightHead[1] = seg->sidedef->lighthead[1]; }
			else if (sub) { VitaWalls.LightHead[0] = sub->lighthead[0]; VitaWalls.LightHead[1] = sub->lighthead[1]; }
		}
		VitaWalls.Collecting = true;
		RenderWall(3, NULL);
		VitaWalls.Collecting = false;
		VitaWalls.LightHead[0] = VitaWalls.LightHead[1] = NULL;
		return;
	}
	// Dynamic light passes (vita_multipasslights): base, light and texture
	// passes are collected the same way.
	if (gl_vita_batchwalls && glset.lightmode != 8 && !(flags & GLWF_GLOW) && !(flags&GLWF_FOGGY)
		&& !((flags&GLWF_SKYHACK && type==RENDERWALL_M2S) || type == RENDERWALL_COLORLAYER)
		&& (pass == GLPASS_BASE || pass == GLPASS_TEXTURE || pass == GLPASS_LIGHT || pass == GLPASS_LIGHT_ADDITIVE))
	{
		if (pass == GLPASS_BASE)
		{
			rel = rellight + getExtraLight();
			gl_SetColor(lightlevel, rel, &Colormap,1.0f);
			if (type!=RENDERWALL_M2SNF) gl_SetFog(lightlevel, rel, &Colormap, false);
			else gl_SetFog(255, 0, NULL, false);
			Vita_BatchSurface(NULL, 0, 0, true);
			VitaWalls.Collecting = true;
			RenderWall(2, NULL);
			VitaWalls.Collecting = false;
			return;
		}
		if (pass == GLPASS_TEXTURE)
		{
			Vita_BatchSurface(gltexture, Colormap.colormap, flags & 3, true);
			VitaWalls.Collecting = true;
			RenderWall(1, NULL);
			VitaWalls.Collecting = false;
			return;
		}
		// light passes
		gl_SetFog((255+lightlevel)>>1, 0, NULL, false);
		if (seg->sidedef == NULL) node = NULL;
		else if (!(seg->sidedef->Flags & WALLF_POLYOBJ)) node = seg->sidedef->lighthead[pass==GLPASS_LIGHT_ADDITIVE];
		else if (sub) node = sub->lighthead[pass==GLPASS_LIGHT_ADDITIVE];
		else node = NULL;
		EXTERN_CVAR(Bool, vita_lighttexpass)
		if (vita_lighttexpass && pass == GLPASS_LIGHT && gltexture != NULL)
		{
			// light x surface texture in one go (no separate texture pass)
			Vita_BatchSurface(gltexture, Colormap.colormap, flags & 3, true);
			VitaWalls.LightPassMode = 1;
			VitaWalls.WallX1 = glseg.x1; VitaWalls.WallY1 = glseg.y1;
			VitaWalls.WallDX = glseg.x2 - glseg.x1; VitaWalls.WallDY = glseg.y2 - glseg.y1;
			const float l2 = VitaWalls.WallDX * VitaWalls.WallDX + VitaWalls.WallDY * VitaWalls.WallDY;
			VitaWalls.WallInvLen2 = l2 > 0 ? 1.f / l2 : 0.f;
			VitaWalls.WallZTop[0] = ztop[0]; VitaWalls.WallZTop[1] = ztop[1];
			VitaWalls.WallZBot[0] = zbottom[0]; VitaWalls.WallZBot[1] = zbottom[1];
			VitaWalls.WallUTop[0] = uplft.u; VitaWalls.WallVTop[0] = uplft.v; VitaWalls.WallUTop[1] = uprgt.u; VitaWalls.WallVTop[1] = uprgt.v;
			VitaWalls.WallUBot[0] = lolft.u; VitaWalls.WallVBot[0] = lolft.v; VitaWalls.WallUBot[1] = lorgt.u; VitaWalls.WallVBot[1] = lorgt.v;
		}
		else Vita_BatchSurface(NULL, 0, 0, true);
		VitaWalls.Collecting = true;
		FLightNode *vhead = node;
		for (; node; node = node->nextLight)
		{
			if (!(node->lightsource->flags2&MF2_DORMANT) && Vita_LightAllowed(vhead, node->lightsource))
			{
				iter_dlight++;
				RenderWall(1, NULL, node->lightsource);
			}
		}
		VitaWalls.Collecting = false;
		VitaWalls.LightPassMode = 0;
		return;
	}
	Vita_FlushWalls();
#endif
	// This allows mid textures to be drawn on lines that might overlap a sky wall
	if ((flags&GLWF_SKYHACK && type==RENDERWALL_M2S) || type == RENDERWALL_COLORLAYER)
	{
		if (pass != GLPASS_DECALS)
		{
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(-1.0f, -128.0f);
		}
	}

	switch (pass)
	{
	case GLPASS_ALL:			// Single-pass rendering
		SetupLights();
		// fall through
	case GLPASS_PLAIN:			// Single-pass rendering
		rel = rellight + getExtraLight();
		gl_SetColor(lightlevel, rel, &Colormap,1.0f);
		if (type!=RENDERWALL_M2SNF) gl_SetFog(lightlevel, rel, &Colormap, false);
		else gl_SetFog(255, 0, NULL, false);

		gl_RenderState.EnableGlow(!!(flags & GLWF_GLOW));
		gltexture->Bind(Colormap.colormap, flags, 0);
		RenderWall(3, NULL);
		gl_RenderState.EnableGlow(false);
		gl_RenderState.EnableLight(false);
		break;

	case GLPASS_BASE:			// Base pass for non-masked polygons (all opaque geometry)
	case GLPASS_BASE_MASKED:	// Base pass for masked polygons (2sided mid-textures and transparent 3D floors)
		rel = rellight + getExtraLight();
		gl_SetColor(lightlevel, rel, &Colormap,1.0f);
		if (!(flags&GLWF_FOGGY)) 
		{
			if (type!=RENDERWALL_M2SNF) gl_SetFog(lightlevel, rel, &Colormap, false);
			else gl_SetFog(255, 0, NULL, false);
		}
		gl_RenderState.EnableGlow(!!(flags & GLWF_GLOW));
		// fall through

		if (pass != GLPASS_BASE)
		{
			gltexture->Bind(Colormap.colormap, flags, 0);
		}
		RenderWall(pass == GLPASS_BASE? 2:3, NULL);
		gl_RenderState.EnableGlow(false);
		gl_RenderState.EnableLight(false);
		break;

	case GLPASS_TEXTURE:		// modulated texture
		gltexture->Bind(Colormap.colormap, flags, 0);
		RenderWall(1, NULL);
		break;

	case GLPASS_LIGHT:
	case GLPASS_LIGHT_ADDITIVE:
		// black fog is diminishing light and should affect lights less than the rest!
		if (!(flags&GLWF_FOGGY)) gl_SetFog((255+lightlevel)>>1, 0, NULL, false);
		else gl_SetFog(lightlevel, 0, &Colormap, true);	

		if (seg->sidedef == NULL)
		{
			node = NULL;
		}
		else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
		{
			// Iterate through all dynamic lights which touch this wall and render them
			node = seg->sidedef->lighthead[pass==GLPASS_LIGHT_ADDITIVE];
		}
		else if (sub)
		{
			// To avoid constant rechecking for polyobjects use the subsector's lightlist instead
			node = sub->lighthead[pass==GLPASS_LIGHT_ADDITIVE];
		}
		else node = NULL;
		while (node)
		{
			if (!(node->lightsource->flags2&MF2_DORMANT))
			{
				iter_dlight++;
				RenderWall(1, NULL, node->lightsource);
			}
			node = node->nextLight;
		}
		break;

	case GLPASS_DECALS:
	case GLPASS_DECALS_NOFOG:
		if (seg->sidedef && seg->sidedef->AttachedDecals)
		{
			if (pass==GLPASS_DECALS) 
			{
				gl_SetFog(lightlevel, rellight + getExtraLight(), &Colormap, false);
			}
			DoDrawDecals();
		}
		break;

	case GLPASS_TRANSLUCENT:
		switch (type)
		{
		case RENDERWALL_MIRRORSURFACE:
			RenderMirrorSurface();
			break;

		case RENDERWALL_FOGBOUNDARY:
			RenderFogBoundary();
			break;

		default:
			RenderTranslucentWall();
			break;
		}
	}

	if ((flags&GLWF_SKYHACK && type==RENDERWALL_M2S) || type == RENDERWALL_COLORLAYER)
	{
		if (pass!=GLPASS_DECALS)
		{
			glDisable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(0, 0);
		}
	}
}
