// [Vita] 0047: wall vertices kept between frames (option 6, part 2).
//
// A wall's batched triangles (and each of its light passes) are recorded the
// first time they are emitted and copied straight into the batch afterwards,
// as long as a signature of the wall's geometry, texture coordinates, colour,
// fog and (for light passes) the light's position, size and colour is the
// same. Skips RenderWall's vertex work and the light projection maths.
// vita_surfcache 0 turns it off.

#include "doomtype.h"
#include "c_cvars.h"
#include "tarray.h"
#include "gl/system/gl_system.h"
#include "gl/renderer/gl_colormap.h"
#include "gl/scene/gl_vitawallbatch.h"

// 0048: off (user: glitches). Renamed so saved configs don't keep it on.
CVAR(Bool, vita_keepwallverts, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

struct FVitaSurfEntry
{
	const void *A, *B;
	unsigned Kind, Sig;
	unsigned Off, Count;	// vertices in the arena (8 floats each)
	bool HasUv2;
	bool Used;
};

enum { SURF_HASH = 1 << 16 };
static TArray<FVitaSurfEntry> Entries;
static int Hash[SURF_HASH];		// entry index + 1
static TArray<float> Arena;
static bool Recording, RecBad, RecHasUv2;
static int RecEntry;
static FVitaBucket *RecBucket;
unsigned Vita_SurfHits, Vita_SurfMisses;
unsigned Vita_SurfPendingSig;

void Vita_SurfCacheClear()
{
	Entries.Clear();
	Arena.Clear();
	memset(Hash, 0, sizeof(Hash));
	Recording = false;
}

static inline unsigned KeyHash(const void *a, const void *b, unsigned kind)
{
	unsigned h = (unsigned)(uintptr_t)a * 2654435761u ^ (unsigned)(uintptr_t)b * 40503u ^ kind * 2246822519u;
	return (h ^ (h >> 15)) & (SURF_HASH - 1);
}

static FVitaSurfEntry *Find(const void *a, const void *b, unsigned kind, unsigned &slot)
{
	unsigned h = KeyHash(a, b, kind);
	for (int probe = 0; probe < 64; probe++, h = (h + 1) & (SURF_HASH - 1))
	{
		const int i = Hash[h] - 1;
		if (i < 0) { slot = h; return NULL; }
		FVitaSurfEntry &e = Entries[i];
		if (e.A == a && e.B == b && e.Kind == kind) { slot = h; return &e; }
	}
	slot = ~0u;
	return NULL;
}

// Returns true if the surface was copied in from the cache; otherwise it
// starts recording what the caller emits next.
bool Vita_SurfTry(const void *a, const void *b, unsigned kind, unsigned sig)
{
	Recording = false;
	if (!vita_keepwallverts) return false;
	if (Entries.Size() > 40000 || Arena.Size() > 2u * 1024 * 1024) Vita_SurfCacheClear();	// at most 8 MB of vertices
	if (Arena.Max() == 0) Arena.Grow(2u * 1024 * 1024 + 65536);	// one block, no doubling reallocations
	unsigned slot;
	FVitaSurfEntry *e = Find(a, b, kind, slot);
	if (e != NULL && e->Sig == sig)
	{
		Vita_SurfHits++;
		if (e->Count == 0) return true;
		Vita_BatchCheck();
		FVitaBucket *bk = VitaWalls.Cur;
		const unsigned n = e->Count;
		float *xyz = &bk->Xyz[bk->Xyz.Reserve(n * 3)];
		float *uv = &bk->Uv[bk->Uv.Reserve(n * 2)];
		float *uv2 = e->HasUv2 ? &bk->Uv2[bk->Uv2.Reserve(n * 2)] : NULL;
		unsigned *rgba = &bk->Rgba[bk->Rgba.Reserve(n)];
		const float *src = &Arena[e->Off];
		for (unsigned i = 0; i < n; i++, src += 8)
		{
			xyz[0] = src[0]; xyz[1] = src[1]; xyz[2] = src[2]; xyz += 3;
			uv[0] = src[3]; uv[1] = src[4]; uv += 2;
			if (uv2) { uv2[0] = src[5]; uv2[1] = src[6]; uv2 += 2; }
			memcpy(rgba++, &src[7], 4);
		}
		return true;
	}
	Vita_SurfMisses++;
	if (slot == ~0u) return false;	// table area full: just draw
	if (e == NULL)
	{
		FVitaSurfEntry ne;
		ne.A = a; ne.B = b; ne.Kind = kind;
		Entries.Push(ne);
		Hash[slot] = Entries.Size();
		e = &Entries[Entries.Size() - 1];
	}
	e->Sig = 0;	// invalid until recorded
	e->Off = Arena.Size(); e->Count = 0; e->HasUv2 = false;
	RecEntry = int(e - &Entries[0]);
	Recording = true; RecBad = false; RecBucket = NULL; RecHasUv2 = false;
	Vita_SurfPendingSig = sig;
	return false;
}

// Called by the batch code after it appended count vertices to bucket b.
void Vita_SurfNote(FVitaBucket *b, unsigned first, unsigned count, bool uv2)
{
	if (!Recording || RecBad) return;
	if (RecBucket != NULL && RecBucket != b) { RecBad = true; return; }
	RecBucket = b;
	FVitaSurfEntry &e = Entries[RecEntry];
	if (e.Count > 0 && uv2 != RecHasUv2) { RecBad = true; return; }
	RecHasUv2 = uv2;
	float *dst = &Arena[Arena.Reserve(count * 8)];
	for (unsigned i = 0; i < count; i++, dst += 8)
	{
		const unsigned v = first + i;
		dst[0] = b->Xyz[v * 3]; dst[1] = b->Xyz[v * 3 + 1]; dst[2] = b->Xyz[v * 3 + 2];
		dst[3] = b->Uv[v * 2]; dst[4] = b->Uv[v * 2 + 1];
		if (uv2) { dst[5] = b->Uv2[v * 2]; dst[6] = b->Uv2[v * 2 + 1]; } else dst[5] = dst[6] = 0;
		memcpy(&dst[7], &b->Rgba[v], 4);
	}
	e.Count += count;
}
void Vita_SurfBad() { if (Recording) RecBad = true; }

void Vita_SurfEnd()
{
	if (!Recording) return;
	Recording = false;
	FVitaSurfEntry &e = Entries[RecEntry];
	if (RecBad) { e.Sig = 0; e.Count = 0; return; }
	e.HasUv2 = RecHasUv2;
	e.Sig = Vita_SurfPendingSig;
}
