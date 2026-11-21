// [Vita] 0048: dynamic lights in a shader.
//
// With the fixed-function renderer every dynamic light on a wall or flat was
// an extra drawing pass of that surface (2,500-4,200 light passes a frame in
// HaloDoom: their vertex work was most of the 35 ms "opaque" time). Now the
// batched walls and flats are drawn once; each batch that has lights is
// drawn with a small vitaGL shader that adds up to 16 lights per pixel:
// texture x (sector light + sum of lights), lit only on the side of the
// surface the light is on, fog as vitaGL's own GL_EXP fog, alpha test as the
// renderer's. Light falloff is linear with distance (the light passes used a
// projected round texture with a similar falloff). vita_shaderlights 0 goes
// back to the light passes.

#include "gl/system/gl_system.h"
#include "c_cvars.h"
#include "tarray.h"
#include "a_sharedglobal.h"
#include "r_defs.h"
#include "gl/system/gl_cvars.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/dynlights/gl_dynlight.h"
#include "gl/scene/gl_vitawallbatch.h"
#include "vita/vita_system.h"
#include "r_main.h"
#include "gl/renderer/gl_lightdata.h"

CVAR(Bool, vita_shaderlights, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vita_shadertest, 0, 0)	// test: 1 = transpose matrices
EXTERN_CVAR(Bool, gl_vita_batchwalls)
EXTERN_CVAR(Float, gl_lights_size)
EXTERN_CVAR(Float, gl_lights_intensity)
EXTERN_CVAR(Bool, gl_lights_additive)

static const int VitaShaderCounts[3] = { 4, 8, 16 };
struct FVitaLightProg { GLuint Prog; GLint Mv, Proj, LPos, LCol, Fog, ARef; };
static FVitaLightProg Progs[3];
static int ShaderState;	// 0 not tried, 1 ready, -1 failed
unsigned Vita_ShaderDraws, Vita_ShaderLightSum;

static const char *VertSrc =
	"uniform float4x4 mv;\n"
	"uniform float4x4 proj;\n"
	"void main(float3 aPos, float2 aUv, float4 aCol, float3 aNrm,\n"
	"	float2 out vUv : TEXCOORD0, float4 out vCol : COLOR, float3 out vWorld : TEXCOORD1, float3 out vNrm : TEXCOORD2, float out vDist : TEXCOORD3,\n"
	"	float4 out gl_Position : POSITION)\n"
	"{\n"
	"	float4 eye = mul(float4(aPos, 1.0f), mv);\n"
	"	gl_Position = mul(eye, proj);\n"
	"	vDist = abs(eye.z);\n"
	"	vUv = aUv; vCol = aCol; vWorld = aPos; vNrm = aNrm;\n"
	"}\n";

static const char *FragFmt =
	"#define NL %d\n#define RED %d\n#define SUMV %d\n"
	"uniform float4 lpos[NL];\n"
	"uniform float4 lcol[NL];\n"
	"uniform float4 fogcol;\n"
	"uniform float aref;\n"
	"uniform sampler2D tex : TEXUNIT0;\n"
	"float4 main(float2 vUv : TEXCOORD0, float4 vCol : COLOR, float3 vWorld : TEXCOORD1, float3 vNrm : TEXCOORD2, float vDist : TEXCOORD3)\n"
	"{\n"
	"	if (RED) return float4(1.0f, 0.0f, 0.0f, 1.0f);\n"
	"	float4 t = tex2D(tex, vUv);\n"
	"	float3 sum = float3(0.0f, 0.0f, 0.0f);\n"
	"	for (int i = 0; i < NL; i++)\n"
	"	{\n"
	"		float3 d = lpos[i].xyz - vWorld;\n"
	"		float att = saturate(1.0f - length(d) / lpos[i].w);\n"
	"		if (dot(d, vNrm) <= 0.0f) att = 0.0f;\n"
	"		sum += lcol[i].rgb * att;\n"
	"	}\n"
	"	if (SUMV) return float4(sum, 1.0f);\n"
	"	float4 c;\n"
	"	c.a = t.a * vCol.a;\n"
	"	if (c.a < aref) discard;\n"
	"	float3 base = t.rgb * vCol.rgb;\n"
	"	if (fogcol.a > 0.0f) base = lerp(fogcol.rgb, base, saturate(exp(-fogcol.a * vDist * 1.442695f)));\n"
	"	c.rgb = saturate(base + t.rgb * sum);\n"
	"	return c;\n"
	"}\n";

static GLuint Compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint len = (GLint)strlen(src);
	glShaderSource(s, 1, &src, &len);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[512] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		Vita_BootLog("shader lights: %s shader failed to compile: %s", type == GL_CG_VERTEX_SHADER_EXT ? "vertex" : "fragment", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static bool Init()
{
	GLuint vs = Compile(GL_CG_VERTEX_SHADER_EXT, VertSrc);
	if (vs == 0) return false;
	for (int k = 0; k < 3; k++)
	{
		char src[2048];
		snprintf(src, sizeof(src), FragFmt, VitaShaderCounts[k], (vita_shadertest & 2) ? 1 : 0, (vita_shadertest & 4) ? 1 : 0);
		GLuint fs = Compile(GL_CG_FRAGMENT_SHADER_EXT, src);
		if (fs == 0) return false;
		GLuint p = glCreateProgram();
		glAttachShader(p, vs);
		glAttachShader(p, fs);
		glBindAttribLocation(p, 0, "aPos");
		glBindAttribLocation(p, 1, "aUv");
		glBindAttribLocation(p, 2, "aCol");
		glBindAttribLocation(p, 3, "aNrm");
		glLinkProgram(p);
		GLint ok = 0;
		glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok) { Vita_BootLog("shader lights: program %d failed to link", VitaShaderCounts[k]); return false; }
		FVitaLightProg &g = Progs[k];
		g.Prog = p;
		g.Mv = glGetUniformLocation(p, "mv"); g.Proj = glGetUniformLocation(p, "proj");
		g.LPos = glGetUniformLocation(p, "lpos"); g.LCol = glGetUniformLocation(p, "lcol");
		g.Fog = glGetUniformLocation(p, "fogcol"); g.ARef = glGetUniformLocation(p, "aref");
	}
	Vita_BootLog("shader lights: ready");
	return true;
}

bool Vita_ShaderLightsOn()
{
	if (!vita_shaderlights || !gl_lights || gl_fixedcolormap || !gl_vita_batchwalls) return false;
	if (ShaderState == 0) ShaderState = Init() ? 1 : -1;
	return ShaderState > 0;
}

void Vita_ShaderAddLights(FVitaBucket *b, FLightNode *h0, FLightNode *h1)
{
	FLightNode *heads[2] = { h0, h1 };
	for (int j = 0; j < 2; j++)
		for (FLightNode *n = heads[j]; n != NULL; n = n->nextLight)
		{
			ADynamicLight *l = n->lightsource;
			if (l == NULL || (l->flags2 & MF2_DORMANT)) continue;
			unsigned i;
			for (i = 0; i < b->Lights.Size(); i++) if (b->Lights[i] == l) break;
			if (i == b->Lights.Size() && b->Lights.Size() < 64) b->Lights.Push(l);
		}
}

// Normal of a polygon (GL coordinates), facing the camera.
void Vita_ShaderNormal(const float *p, unsigned n, float *out)
{
	out[0] = out[1] = out[2] = 0;
	for (unsigned k = 1; k + 1 < n; k++)
	{
		const float ax = p[k*3] - p[0], ay = p[k*3+1] - p[1], az = p[k*3+2] - p[2];
		const float bx = p[(k+1)*3] - p[0], by = p[(k+1)*3+1] - p[1], bz = p[(k+1)*3+2] - p[2];
		out[0] += ay * bz - az * by; out[1] += az * bx - ax * bz; out[2] += ax * by - ay * bx;
	}
	const float len = sqrtf(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
	if (len < 1e-6f) { out[1] = 1; return; }
	out[0] /= len; out[1] /= len; out[2] /= len;
	const float cx = FIXED2FLOAT(viewx), cy = FIXED2FLOAT(viewz), cz = FIXED2FLOAT(viewy);
	if (out[0] * (cx - p[0]) + out[1] * (cy - p[1]) + out[2] * (cz - p[2]) < 0) { out[0] = -out[0]; out[1] = -out[1]; out[2] = -out[2]; }
}

// Draws a bucket's triangles with its lights. False if it can't (FFP then).
bool Vita_ShaderDrawBucket(FVitaBucket *b, unsigned piece)
{
	const unsigned total = b->Xyz.Size() / 3;
	if (ShaderState <= 0 || b->Lights.Size() == 0 || b->Nrm.Size() != b->Xyz.Size() || total == 0) return false;
	// strongest lights for the camera first
	struct FL { float score; ADynamicLight *l; };
	FL sel[16]; int ns = 0;
	const float cx = FIXED2FLOAT(viewx), cy = FIXED2FLOAT(viewy), cz = FIXED2FLOAT(viewz);
	for (unsigned i = 0; i < b->Lights.Size(); i++)
	{
		ADynamicLight *l = b->Lights[i];
		const float r = l->GetRadius() * gl_lights_size;
		if (r <= 0) continue;
		const float dx = FIXED2FLOAT(l->x) - cx, dy = FIXED2FLOAT(l->y) - cy, dz = FIXED2FLOAT(l->z) - cz;
		const float sc = r * 4.f - sqrtf(dx*dx + dy*dy + dz*dz);
		if (ns < 16) { sel[ns].score = sc; sel[ns].l = l; ns++; }
		else
		{
			int w = 0;
			for (int k = 1; k < 16; k++) if (sel[k].score < sel[w].score) w = k;
			if (sc > sel[w].score) { sel[w].score = sc; sel[w].l = l; }
		}
	}
	if (ns == 0) return false;
	const int k = ns <= 4 ? 0 : ns <= 8 ? 1 : 2;
	const int nl = VitaShaderCounts[k];
	const FVitaLightProg &g = Progs[k];
	float lpos[16 * 4], lcol[16 * 4];
	for (int i = 0; i < nl; i++)
	{
		if (i < ns)
		{
			ADynamicLight *l = sel[i].l;
			lpos[i*4] = FIXED2FLOAT(l->x); lpos[i*4+1] = FIXED2FLOAT(l->z); lpos[i*4+2] = FIXED2FLOAT(l->y);
			lpos[i*4+3] = l->GetRadius() * gl_lights_size;
			float s = gl_lights_intensity / 255.f;
			if (gl_lights_additive || (l->flags4 & MF4_ADDITIVE)) s *= 0.2f;
			if (l->IsSubtractive()) s = -s;
			lcol[i*4] = l->GetRed() * s; lcol[i*4+1] = l->GetGreen() * s; lcol[i*4+2] = l->GetBlue() * s; lcol[i*4+3] = 0;
		}
		else
		{
			lpos[i*4] = lpos[i*4+1] = lpos[i*4+2] = 0; lpos[i*4+3] = 1;
			lcol[i*4] = lcol[i*4+1] = lcol[i*4+2] = lcol[i*4+3] = 0;
		}
	}
	float mv[16], proj[16];
	glGetFloatv(GL_MODELVIEW_MATRIX, mv);
	glGetFloatv(GL_PROJECTION_MATRIX, proj);
	if (vita_shadertest & 1)
	{
		for (int r = 0; r < 4; r++) for (int c = r + 1; c < 4; c++) { float t = mv[r*4+c]; mv[r*4+c] = mv[c*4+r]; mv[c*4+r] = t; t = proj[r*4+c]; proj[r*4+c] = proj[c*4+r]; proj[c*4+r] = t; }
	}
	float fog[4] = { 0, 0, 0, 0 };
	if (b->FogOn && !(vita_shadertest & 8)) { fog[0] = b->FogColor.r / 255.f; fog[1] = b->FogColor.g / 255.f; fog[2] = b->FogColor.b / 255.f; fog[3] = b->FogDensity / 64000.f; }
	bool atest; float athr;
	gl_RenderState.VitaGetAlpha(atest, athr);
	Vita_ShaderDraws++; Vita_ShaderLightSum += ns;
	glUseProgram(g.Prog);
	glUniformMatrix4fv(g.Mv, 1, GL_FALSE, mv);
	glUniformMatrix4fv(g.Proj, 1, GL_FALSE, proj);
	glUniform4fv(g.LPos, nl, lpos);
	glUniform4fv(g.LCol, nl, lcol);
	glUniform4fv(g.Fog, 1, fog);
	glUniform1f(g.ARef, atest ? athr : -1.f);
	glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2); glEnableVertexAttribArray(3);
	for (unsigned start = 0; start < total; start += piece)
	{
		const unsigned n = total - start < piece ? total - start : piece;
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, &b->Xyz[start * 3]);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, &b->Uv[start * 2]);
		glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, &b->Rgba[start]);
		glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 0, &b->Nrm[start * 3]);
		glDrawArrays(GL_TRIANGLES, 0, n);
	}
	glDisableVertexAttribArray(0); glDisableVertexAttribArray(1); glDisableVertexAttribArray(2); glDisableVertexAttribArray(3);
	glUseProgram(0);
	return true;
}
