// [Vita] Pad controls: tap/hold keys and a control layout per game.
//
// Every pad button has two keys: the button itself (a tap, or a press of a
// button whose hold key is unbound) and "Hold_<button>", sent once the
// button has been held for vita_holdtime ms. A button whose hold key is
// unbound reacts immediately, as before. When the controls menu waits for a
// key, a quick press binds the button and a long one its hold key.
//
// Each game (the mods ticked in the launcher, or the IWAD without mods) has
// its own layout in ux0:data/zandronum/controls/<game>.ini. It is loaded when
// the game starts and saved whenever a binding changes. A game without one
// starts from its default layout (HaloDoom has its own).

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <psp2/kernel/processmgr.h>

#include "doomdef.h"
#include "doomtype.h"
#include "d_event.h"
#include "c_bind.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "configfile.h"
#include "w_wad.h"
#include "m_crc32.h"
#include "menu/menu.h"
#include "vita/vita_system.h"

CVAR(Int, vita_holdtime, 300, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

#define VITA_PAD_BUTTONS 12
#define CONTROLS_DIR "ux0:data/zandronum/controls"

// Pad button order from SDL: Triangle, Circle, Cross, Square, L, R,
// Down, Left, Up, Right, Select, Start.
static const char *const PadNames[VITA_PAD_BUTTONS] =
{
	"Triangle", "Circle", "Cross", "Square", "LButton", "RButton",
	"PadDown", "PadLeft", "PadUp", "PadRight", "Select", "Start"
};
static char HoldNames[VITA_PAD_BUTTONS][16];

// Called from c_bind.cpp's static setup: friendly names for the pad keys
// (config files with joy1..joy24 still load, see GetKeyFromName).
void Vita_NamePadKeys(const char **keynames)
{
	for (int i = 0; i < VITA_PAD_BUTTONS; i++)
	{
		snprintf(HoldNames[i], sizeof(HoldNames[i]), "Hold_%s", PadNames[i]);
		keynames[KEY_FIRSTJOYBUTTON + i] = PadNames[i];
		keynames[KEY_FIRSTJOYBUTTON + VITA_PAD_BUTTONS + i] = HoldNames[i];
	}
}

//==========================================================================
//
// Tap / hold
//
//==========================================================================

enum { PS_IDLE, PS_PENDING, PS_HOLDING, PS_TAPPED, PS_DOWN };
struct FPadButton { int State; unsigned long long Time; };
static FPadButton Pad[VITA_PAD_BUTTONS];

static unsigned long long NowMs() { return sceKernelGetProcessTimeWide() / 1000; }

static void PostKey(int key, bool down)
{
	event_t ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = down ? EV_KeyDown : EV_KeyUp;
	ev.data1 = key;
	D_PostEvent(&ev);
}

static bool HoldBound(int button)
{
	if (menuactive == MENU_WaitKey) return true;	// binding a key: tell taps from holds
	const int hk = KEY_FIRSTJOYBUTTON + VITA_PAD_BUTTONS + button;
	return !Bindings.GetBinding(hk).IsEmpty() || !DoubleBindings.GetBinding(hk).IsEmpty();
}

// A pad button changed while the game (or the key binding menu) has the
// input. Returns true when handled here.
bool Vita_PadButton(int button, bool down)
{
	if (button < 0 || button >= VITA_PAD_BUTTONS) return false;
	FPadButton &b = Pad[button];
	const int tap = KEY_FIRSTJOYBUTTON + button, hold = tap + VITA_PAD_BUTTONS;
	if (down)
	{
		if (b.State == PS_TAPPED) PostKey(tap, false);	// release of the last tap still pending
		if (HoldBound(button)) { b.State = PS_PENDING; b.Time = NowMs(); }
		else { b.State = PS_DOWN; PostKey(tap, true); }
	}
	else
	{
		switch (b.State)
		{
		case PS_PENDING:
			// short press: the tap key, released a little later so that the
			// game sees it for at least a tic (+reload-style commands)
			PostKey(tap, true);
			b.State = PS_TAPPED;
			b.Time = NowMs() + 60;
			break;
		case PS_HOLDING:
			PostKey(hold, false);
			b.State = PS_IDLE;
			break;
		case PS_DOWN:
			PostKey(tap, false);
			b.State = PS_IDLE;
			break;
		default:
			break;
		}
	}
	return true;
}

// Every frame: holds that reached the hold time, delayed tap releases.
static void PadUpdate()
{
	const unsigned long long now = NowMs();
	for (int i = 0; i < VITA_PAD_BUTTONS; i++)
	{
		FPadButton &b = Pad[i];
		if (b.State == PS_PENDING && now - b.Time >= (unsigned)MAX(50, (int)vita_holdtime))
		{
			PostKey(KEY_FIRSTJOYBUTTON + VITA_PAD_BUTTONS + i, true);
			b.State = PS_HOLDING;
		}
		else if (b.State == PS_TAPPED && now >= b.Time)
		{
			PostKey(KEY_FIRSTJOYBUTTON + i, false);
			b.State = PS_IDLE;
		}
	}
}

// The menu took over (GUI input): let go of everything the game holds.
void Vita_PadReleaseAll()
{
	for (int i = 0; i < VITA_PAD_BUTTONS; i++)
	{
		FPadButton &b = Pad[i];
		if (b.State == PS_HOLDING) PostKey(KEY_FIRSTJOYBUTTON + VITA_PAD_BUTTONS + i, false);
		else if (b.State == PS_DOWN || b.State == PS_TAPPED) PostKey(KEY_FIRSTJOYBUTTON + i, false);
		b.State = PS_IDLE;
	}
}

//==========================================================================
//
// Layout per game
//
//==========================================================================

static FString LayoutPath;
static unsigned LayoutCrc;
static bool LayoutActive;
static unsigned long long NextCheck;

static bool IsEngineFile(const char *name)
{
	static const char *const names[] = { "zandronum.pk3", "skulltag_actors.pk3", "brightmaps.pk3", "vita-halodoom.pk3",
		"skulltag_data.pk3", "skulltag_content.pk3" };
	for (size_t i = 0; i < countof(names); i++)
		if (!stricmp(name, names[i])) return true;
	return false;
}

static FString BaseName(const char *path)
{
	const char *s = strrchr(path, '/'), *c = strrchr(path, ':');
	if (c > s) s = c;
	FString b = s ? s + 1 : path;
	long dot = b.LastIndexOf('.');
	if (dot > 0) b.Truncate(dot);
	FString out;
	for (unsigned i = 0; i < b.Len(); i++)
	{
		char ch = b[i];
		out += (isalnum((unsigned char)ch) || ch == '-' || ch == '_') ? ch : '_';
	}
	return out;
}

// "<mod>+<mod>..." for the files loaded after the IWAD, or the IWAD's name.
static FString GameName()
{
	FString mods, iwad;
	const int n = Wads.GetNumWads();
	for (int i = 0; i < n; i++)
	{
		const char *full = Wads.GetWadFullName(i);
		if (full == NULL) continue;
		const char *name = Wads.GetWadName(i);
		if (name == NULL || IsEngineFile(name) || !strnicmp(full, "app0:", 5)) continue;
		if (iwad.IsEmpty()) { iwad = BaseName(full); continue; }	// the first one after the engine files is the IWAD
		if (!mods.IsEmpty()) mods += '+';
		mods += BaseName(full);
	}
	return mods.IsEmpty() ? iwad : mods;
}

static unsigned BindingsCrc()
{
	unsigned crc = 0;
	FKeyBindings *sets[3] = { &Bindings, &DoubleBindings, &AutomapBindings };
	for (int s = 0; s < 3; s++)
		for (int k = 0; k < NUM_KEYS; k++)
		{
			const FString &b = sets[s]->GetBinding(k);
			if (!b.IsEmpty())
			{
				crc = AddCRC32(crc, (const BYTE *)&k, sizeof(k));
				crc = AddCRC32(crc, (const BYTE *)b.GetChars(), b.Len());
			}
		}
	return crc;
}

static void SaveLayout()
{
	mkdir(CONTROLS_DIR, 0777);
	FConfigFile f(LayoutPath);
	f.SetSection("Bindings", true); f.ClearCurrentSection(); Bindings.ArchiveBindings(&f);
	f.SetSection("DoubleBindings", true); f.ClearCurrentSection(); DoubleBindings.ArchiveBindings(&f);
	f.SetSection("AutomapBindings", true); f.ClearCurrentSection(); AutomapBindings.ArchiveBindings(&f);
	f.WriteConfigFile();
	LayoutCrc = BindingsCrc();
}

static bool LoadLayout()
{
	FILE *t = fopen(LayoutPath, "rb");
	if (t == NULL) return false;
	fclose(t);
	FConfigFile f(LayoutPath);
	const char *key, *value;
	if (f.SetSection("Bindings"))
	{
		Bindings.UnbindAll();
		while (f.NextInSection(key, value)) Bindings.DoBind(key, value);
	}
	if (f.SetSection("DoubleBindings"))
	{
		DoubleBindings.UnbindAll();
		while (f.NextInSection(key, value)) DoubleBindings.DoBind(key, value);
	}
	if (f.SetSection("AutomapBindings"))
	{
		AutomapBindings.UnbindAll();
		while (f.NextInSection(key, value)) AutomapBindings.DoBind(key, value);
	}
	return true;
}

// HaloDoom's layout (commands from its KEYCONF/MENUDEF).
static void HaloDoomDefaults()
{
	static const char *const binds[][2] =
	{
		{ "Triangle", "weapnext" },			// switch weapon
		{ "Circle", "+altattack" },			// melee
		{ "Cross", "+jump" },
		{ "Square", "+reloadchecker" },		// tap: reload
		{ "Hold_Square", "+pick" },			// hold: grab weapon / open (use)
		{ "LButton", "+zoom" },				// HaloDoom's "Throw Grenade"
		{ "RButton", "+attack" },
		{ "PadDown", "crouch" },			// crouch toggle
		{ "PadLeft", "puke 23" },			// switch grenade type
		{ "PadRight", "" },					// flashlight: HaloDoom has no command for one
		{ "PadUp", "zoomin" },				// zoom in/out
		{ "Select", "+scoreboardtog" },
		{ "Start", "menu_main" },
	};
	for (int i = 0; i < VITA_PAD_BUTTONS; i++)
	{
		Bindings.SetBind(KEY_FIRSTJOYBUTTON + i, "");
		Bindings.SetBind(KEY_FIRSTJOYBUTTON + VITA_PAD_BUTTONS + i, "");
		DoubleBindings.SetBind(KEY_FIRSTJOYBUTTON + i, "");
		DoubleBindings.SetBind(KEY_FIRSTJOYBUTTON + VITA_PAD_BUTTONS + i, "");
	}
	for (size_t i = 0; i < countof(binds); i++)
		if (binds[i][1][0]) Bindings.DoBind(binds[i][0], binds[i][1]);
}

// Once, when the game loop starts (after the config, the mods' KEYCONFs and
// autoexec have set their bindings).
void Vita_ControlsInit()
{
	FString game = GameName();
	if (game.IsEmpty()) game = "default";
	LayoutPath.Format(CONTROLS_DIR "/%s.ini", game.GetChars());
	LayoutActive = true;
	if (LoadLayout())
	{
		Vita_BootLog("controls: layout of \"%s\" loaded from %s", game.GetChars(), LayoutPath.GetChars());
		LayoutCrc = BindingsCrc();
		return;
	}
	const bool halodoom = Wads.CheckNumForFullName("acs/halodoomgamemodes.o") >= 0;
	if (halodoom) HaloDoomDefaults();
	SaveLayout();
	Vita_BootLog("controls: new layout for \"%s\" (%s defaults) saved to %s", game.GetChars(),
		halodoom ? "HaloDoom" : "general", LayoutPath.GetChars());
}

// Every frame: tap/hold timing; every 2 s: save the layout if it changed.
void Vita_ControlsFrame()
{
	PadUpdate();
	if (!LayoutActive) return;
	const unsigned long long now = NowMs();
	if (now < NextCheck) return;
	NextCheck = now + 2000;
	if (BindingsCrc() != LayoutCrc)
	{
		SaveLayout();
		Vita_BootLog("controls: layout saved to %s", LayoutPath.GetChars());
	}
}
