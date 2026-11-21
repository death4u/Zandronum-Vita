// [Vita] Learned preloading (0044).
//
// Sprites, model skins and sounds a mod uses during play are noted per game
// ("<mod>+<mod>" as in the controls layouts) in
// ux0:data/zandronum/learned/<game>.txt. On the next level start they are
// loaded during the loading screen, so the first casing, footstep, gore
// sprite or ricochet of a session doesn't stall the game. Works the same for
// any mod; it starts helping from the second level/session.

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "doomtype.h"
#include "doomstat.h"
#include "c_cvars.h"
#include "s_sound.h"
#include "textures/textures.h"
#include "w_wad.h"
#include "g_level.h"
#include "vita/vita_system.h"
#include <psp2/kernel/processmgr.h>

CVAR(Bool, vita_learnpreload, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

FString Vita_GameName();

static TArray<BYTE> SeenTex, SeenSnd;	// marks from the current level, by index
struct FLearned { FString Name; int Type; };	// Type -1 = sound, else FTexture::UseType
static TArray<FLearned> Learned;
static bool Loaded, Dirty;

static FString LearnPath()
{
	FString g = Vita_GameName();
	if (g.IsEmpty()) g = "default";
	for (unsigned i = 0; i < g.Len(); i++)
	{
		char c = g[i];
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') g.LockBuffer()[i] = '_', g.UnlockBuffer();
	}
	return FString("ux0:data/zandronum/learned/") + g + ".txt";
}

static bool Have(const char *name, int type)
{
	for (unsigned i = 0; i < Learned.Size(); i++)
		if (Learned[i].Type == type && !Learned[i].Name.CompareNoCase(name)) return true;
	return false;
}

static void Load()
{
	if (Loaded) return;
	Loaded = true;
	FILE *f = fopen(LearnPath(), "r");
	if (f == NULL) return;
	char line[256];
	while (fgets(line, sizeof(line), f))
	{
		int type; char name[200];
		if (sscanf(line, "%d %199s", &type, name) == 2 && !Have(name, type))
		{
			FLearned l = { name, type };
			Learned.Push(l);
		}
	}
	fclose(f);
}

// Called by the texture code when it creates a texture mid-level.
void Vita_LearnTexture(FTexture *tex)
{
	if (!vita_learnpreload || tex == NULL || gamestate != GS_LEVEL) return;
	if (tex->GetWidth() < 16 || tex->GetHeight() < 16) return;	// tiny ones are cheap
	if (tex->UseType == FTexture::TEX_Wall || tex->UseType == FTexture::TEX_Flat || tex->UseType == FTexture::TEX_MiscPatch || tex->UseType == FTexture::TEX_FontChar) return;
	const int idx = tex->GetID().GetIndex();
	if (idx > 0 && (unsigned)idx < SeenTex.Size()) SeenTex[idx] = 1;
}

// Called by S_LoadSound when a sound is loaded mid-level (any thread).
void Vita_LearnSound(int sfxindex)
{
	if (!vita_learnpreload || gamestate != GS_LEVEL) return;
	if (sfxindex > 0 && (unsigned)sfxindex < SeenSnd.Size()) SeenSnd[sfxindex] = 1;
}

// Level change: what the last level loaded goes into the list (main thread).
void Vita_LearnSave()
{
	if (!vita_learnpreload) return;
	Load();
	int added = 0;
	for (unsigned i = 1; i < SeenTex.Size() && Learned.Size() < 3000; i++)
	{
		if (!SeenTex[i]) continue;
		FTexture *tex = TexMan.ByIndex(i);
		if (tex == NULL || tex->Name[0] == 0 || Have(tex->Name, tex->UseType)) continue;
		FLearned l = { tex->Name, tex->UseType };
		Learned.Push(l); added++;
	}
	for (unsigned i = 1; i < SeenSnd.Size() && i < S_sfx.Size() && Learned.Size() < 3000; i++)
	{
		if (!SeenSnd[i] || S_sfx[i].name.IsEmpty() || Have(S_sfx[i].name, -1)) continue;
		FLearned l = { S_sfx[i].name, -1 };
		Learned.Push(l); added++;
	}
	memset(&SeenTex[0], 0, SeenTex.Size());
	if (SeenSnd.Size()) memset(&SeenSnd[0], 0, SeenSnd.Size());
	if (added == 0 && !Dirty) return;
	mkdir("ux0:data/zandronum/learned", 0777);
	FILE *f = fopen(LearnPath(), "w");
	if (f == NULL) return;
	for (unsigned i = 0; i < Learned.Size(); i++) fprintf(f, "%d %s\n", Learned[i].Type, Learned[i].Name.GetChars());
	fclose(f);
	Dirty = false;
	Vita_BootLog("learned preload: %d new sprites/skins/sounds noted (%u in total)", added, Learned.Size());
}

// Level start, during the loading screen: sounds are only marked here (the
// sound precache loads them, in the background); textures are loaded until
// the time limit.
void Vita_LearnMarkSounds()
{
	SeenSnd.Resize(S_sfx.Size());
	if (SeenSnd.Size()) memset(&SeenSnd[0], 0, SeenSnd.Size());
	if (!vita_learnpreload || gamestate == GS_TITLELEVEL || !stricmp(level.mapname, "TITLEMAP")) return;
	Load();
	int n = 0;
	for (unsigned i = 0; i < Learned.Size(); i++)
	{
		if (Learned[i].Type != -1) continue;
		const int id = S_FindSound(Learned[i].Name);
		if (id <= 0) continue;
		sfxinfo_t *sfx = &S_sfx[id];
		if (sfx->link != sfxinfo_t::NO_LINK) sfx = &S_sfx[sfx->link];
		if (sfx->lumpnum >= 0 && Wads.LumpLength(sfx->lumpnum) > 256 * 1024) continue;	// long sounds: still on demand
		sfx->bUsed = true;
		n++;
	}
	if (n) Vita_BootLog("learned preload: %d sounds marked", n);
}

void Vita_LearnPreloadTextures(void (*precache)(FTexture *))
{
	SeenTex.Resize(TexMan.NumTextures());
	if (SeenTex.Size()) memset(&SeenTex[0], 0, SeenTex.Size());
	if (!vita_learnpreload || !stricmp(level.mapname, "TITLEMAP")) return;
	Load();
	const unsigned long long t0 = sceKernelGetProcessTimeWide(), end = t0 + 3000000ull;
	int n = 0, left = 0;
	for (unsigned i = 0; i < Learned.Size(); i++)
	{
		if (Learned[i].Type < 0) continue;
		if (sceKernelGetProcessTimeWide() > end) { left++; continue; }
		FTextureID id = TexMan.CheckForTexture(Learned[i].Name, Learned[i].Type, FTextureManager::TEXMAN_TryAny);
		FTexture *tex = id.isValid() ? TexMan[id] : NULL;
		if (tex == NULL) continue;
		precache(tex);
		n++;
	}
	Vita_BootLog("learned preload: %d sprites/skins in %llu ms (%d left for when first drawn: time limit)", n,
		(sceKernelGetProcessTimeWide() - t0) / 1000, left);
}
