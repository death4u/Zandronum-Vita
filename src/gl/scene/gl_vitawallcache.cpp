// [Vita] 0047: wall processing kept between frames (option 6, part 1).
//
// GLWall::Process (texture coordinates, heights, splitting, list choice) ran
// for every visible wall in every frame. Its result only changes when the
// sectors, sides, lines or global render settings it reads change, so the
// walls it hands to PutWall are recorded per seg and replayed (PutWall again,
// so list choice, dynamic lights and translucency sorting stay per frame)
// while a signature of those inputs stays the same. Segs whose walls depend
// on per-frame data (sky/portal/mirror walls, fake-flat sectors, 3D floors,
// polyobjects, horizons) are never cached. vita_wallcache 0 turns it off.

#include "gl/system/gl_system.h"
#include "p_local.h"
#include "p_lnspec.h"
#include "a_sharedglobal.h"
#include "g_level.h"
#include "templates.h"
#include "r_defs.h"
#include "r_sky.h"
#include "c_cvars.h"
#include "gl/system/gl_cvars.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/data/gl_data.h"
#include "gl/dynlights/gl_glow.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/scene/gl_portal.h"
#include "gl/textures/gl_material.h"
#include "gl/utility/gl_geometric.h"

// 0048: off (user: glitches). Renamed so saved configs don't keep it on.
CVAR(Bool, vita_keepwalls, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(Bool, gl_seamless)
EXTERN_CVAR(Bool, gl_mirrors)

struct FVitaWallItem { GLWall Wall; bool Translucent; };
struct FVitaMissing { side_t *Side; subsector_t *Sub; fixed_t Height; bool Upper; };
struct FVitaSegCache
{
	unsigned Sig;
	bool Valid, Never;
	TArray<FVitaWallItem> Walls;
	TArray<FVitaMissing> Missing;
};

static TArray<FVitaSegCache *> SegCache;	// by seg index
static int SegCacheSegs = -1;
static FVitaSegCache *Rec;					// being recorded
static bool RecBad;
unsigned Vita_WallCacheHits, Vita_WallCacheMisses;

void Vita_WallCacheClear()
{
	for (unsigned i = 0; i < SegCache.Size(); i++) delete SegCache[i];
	SegCache.Clear();
	SegCacheSegs = -1;
	Rec = NULL;
}

static inline unsigned Mix(unsigned h, unsigned v) { return (h ^ v) * 16777619u; }
static inline unsigned MixP(unsigned h, const void *p) { return Mix(h, (unsigned)(uintptr_t)p); }

static unsigned SectorSig(unsigned h, const sector_t *s)
{
	h = Mix(h, s->floorplane.a); h = Mix(h, s->floorplane.b); h = Mix(h, s->floorplane.c); h = Mix(h, s->floorplane.d);
	h = Mix(h, s->ceilingplane.a); h = Mix(h, s->ceilingplane.b); h = Mix(h, s->ceilingplane.c); h = Mix(h, s->ceilingplane.d);
	h = Mix(h, s->GetPlaneTexZ(sector_t::floor)); h = Mix(h, s->GetPlaneTexZ(sector_t::ceiling));
	h = Mix(h, s->GetTexture(sector_t::floor).GetIndex()); h = Mix(h, s->GetTexture(sector_t::ceiling).GetIndex());
	h = MixP(h, TexMan(s->GetTexture(sector_t::floor))); h = MixP(h, TexMan(s->GetTexture(sector_t::ceiling)));
	h = Mix(h, (unsigned)s->lightlevel); h = MixP(h, s->ColorMap);
	return h;
}

static bool SectorCacheable(const sector_t *s)
{
	if (s == NULL) return true;
	if (s != &sectors[s->sectornum]) return false;				// fake flat copy (camera dependent)
	if (s->heightsec != NULL) return false;
	if (s->e->XFloor.ffloors.Size() || s->e->XFloor.lightlist.Size() > 1) return false;	// 3D floors
	if (s->portals[sector_t::floor] != NULL || s->portals[sector_t::ceiling] != NULL) return false;
	return true;
}

// Returns true if the seg's walls were replayed from the cache.
bool Vita_WallCacheTry(seg_t *seg, sector_t *front, sector_t *back, subsector_t *sub, unsigned &sigout)
{
	sigout = 0;
	if (!vita_keepwalls || seg->sidedef == NULL || seg->linedef == NULL) return false;
	if (seg->sidedef->Flags & WALLF_POLYOBJ) return false;
	if (seg->linedef->special == Line_Horizon || seg->linedef->special == Line_Mirror) return false;
	if (!SectorCacheable(front) || !SectorCacheable(back)) return false;
	if (SegCacheSegs != numsegs)
	{
		Vita_WallCacheClear();
		SegCache.Resize(numsegs);
		for (int i = 0; i < numsegs; i++) SegCache[i] = NULL;
		SegCacheSegs = numsegs;
	}
	const unsigned idx = (unsigned)(seg - segs);
	if (idx >= SegCache.Size()) return false;

	// signature of everything Process reads
	unsigned h = 2166136261u;
	h = MixP(h, front); h = MixP(h, back); h = MixP(h, sub);
	h = SectorSig(h, front);
	if (back != NULL) h = SectorSig(h, back);
	const side_t *sd = seg->sidedef;
	for (int p = 0; p < 3; p++)
	{
		h = MixP(h, TexMan(sd->GetTexture(p)));
		h = Mix(h, sd->GetTextureXOffset(p)); h = Mix(h, sd->GetTextureYOffset(p));
		h = Mix(h, sd->GetTextureXScale(p)); h = Mix(h, sd->GetTextureYScale(p));
	}
	h = Mix(h, (unsigned)(int)sd->Light); h = Mix(h, sd->Flags);
	const line_t *ld = seg->linedef;
	h = Mix(h, ld->flags); h = Mix(h, (unsigned)ld->special); h = Mix(h, ld->Alpha);
	// global render settings
	h = Mix(h, (unsigned)gl_fixedcolormap); h = Mix(h, (unsigned)glset.lightmode);
	h = Mix(h, level.flags & LEVEL_HASFADETABLE ? 1 : 0); h = Mix(h, (unsigned)i_compatflags);
	h = Mix(h, (gl_seamless ? 1 : 0) | (gl_mirrors ? 2 : 0)); h = Mix(h, (unsigned)skyflatnum.GetIndex());
	if (h == 0) h = 1;
	sigout = h;

	FVitaSegCache *c = SegCache[idx];
	if (c == NULL || !c->Valid || c->Sig != h)
	{
		if (c != NULL && c->Never && c->Sig == h) return false;	// known uncacheable
		return false;
	}
	if (c->Never) return false;
	// replay
	if (gl_seamless)
	{
		vertex_t *v1 = seg->sidedef == seg->linedef->sidedef[0] ? seg->linedef->v1 : seg->linedef->v2;
		vertex_t *v2 = seg->sidedef == seg->linedef->sidedef[0] ? seg->linedef->v2 : seg->linedef->v1;
		if (v1->dirty) gl_RecalcVertexHeights(v1);
		if (v2->dirty) gl_RecalcVertexHeights(v2);
	}
	for (unsigned i = 0; i < c->Walls.Size(); i++)
	{
		GLWall w = c->Walls[i].Wall;
		w.Vita_Put(c->Walls[i].Translucent);
	}
	for (unsigned i = 0; i < c->Missing.Size(); i++)
	{
		const FVitaMissing &m = c->Missing[i];
		if (m.Upper) gl_drawinfo->AddUpperMissingTexture(m.Side, m.Sub, m.Height);
		else gl_drawinfo->AddLowerMissingTexture(m.Side, m.Sub, m.Height);
	}
	Vita_WallCacheHits++;
	return true;
}

void Vita_WallCacheRecordBegin(seg_t *seg, unsigned sig)
{
	Rec = NULL;
	if (sig == 0) return;
	const unsigned idx = (unsigned)(seg - segs);
	if (idx >= SegCache.Size()) return;
	FVitaSegCache *c = SegCache[idx];
	if (c == NULL) c = SegCache[idx] = new FVitaSegCache;
	c->Valid = false; c->Never = false; c->Sig = sig;
	c->Walls.Clear(); c->Missing.Clear();
	Rec = c;
	RecBad = false;
	Vita_WallCacheMisses++;
}

void Vita_WallCacheRecordEnd()
{
	if (Rec == NULL) return;
	if (RecBad) { Rec->Never = true; Rec->Walls.Clear(); Rec->Missing.Clear(); }
	else Rec->Valid = true;
	Rec = NULL;
}

// From GLWall::PutWall (before it changes anything).
void Vita_WallCacheNotePut(const GLWall *w, bool translucent, int type)
{
	if (Rec == NULL) return;
	// sky, portal, horizon and mirror walls point at per-frame data
	switch (type)
	{
	case RENDERWALL_SKY: case RENDERWALL_SKYBOX: case RENDERWALL_HORIZON: case RENDERWALL_SECTORSTACK:
	case RENDERWALL_PLANEMIRROR: case RENDERWALL_MIRROR: case RENDERWALL_MIRRORSURFACE: case RENDERWALL_COLORLAYER:
		RecBad = true;
		return;
	default: break;
	}
	FVitaWallItem it;
	it.Wall = *w;
	it.Translucent = translucent;
	Rec->Walls.Push(it);
}

void Vita_WallCacheNoteMissing(side_t *side, subsector_t *sub, fixed_t height, bool upper)
{
	if (Rec == NULL) return;
	FVitaMissing m = { side, sub, height, upper };
	Rec->Missing.Push(m);
}
