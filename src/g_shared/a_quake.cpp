#include "templates.h"
#include "doomtype.h"
#include "doomstat.h"
#include "p_local.h"
#include "actor.h"
#include "m_bbox.h"
#include "m_random.h"
#include "s_sound.h"
#include "a_sharedglobal.h"
#include "statnums.h"
#include "farchive.h"
// [BB] New #includes.
#include "network.h"
#include "sv_commands.h"

static FRandom pr_quake ("Quake");

IMPLEMENT_POINTY_CLASS (DEarthquake)
 DECLARE_POINTER (m_Spot)
END_POINTERS

//==========================================================================
//
// DEarthquake :: DEarthquake private constructor
//
//==========================================================================

DEarthquake::DEarthquake()
: DThinker(STAT_EARTHQUAKE)
{
}

//==========================================================================
//
// DEarthquake :: DEarthquake public constructor
//
//==========================================================================

DEarthquake::DEarthquake (AActor *center, int intensity, int duration,
						  int damrad, int tremrad, FSoundID quakesound)
						  : DThinker(STAT_EARTHQUAKE)
{

	// [BC] If we're the server, tell clients to do the earthquake.
	if (( NETWORK_GetState( ) == NETSTATE_SERVER ) && ( center ))
		SERVERCOMMANDS_Earthquake( center, intensity, duration, tremrad, quakesound );

	m_QuakeSFX = quakesound;
	m_Spot = center;
	// Radii are specified in tile units (64 pixels)
	m_DamageRadius = damrad << (FRACBITS);
	m_TremorRadius = tremrad << (FRACBITS);
	m_Intensity = intensity;
	m_Countdown = duration;
	m_IntensityY = intensity;
	m_IntensityZ = 0;
	m_Flags = 0;
	m_CountdownStart = duration;
}

// [Vita] ZDoom 2.8's A_QuakeEx
enum { VQF_RELATIVE = 1, VQF_SCALEDOWN = 1 << 1, VQF_SCALEUP = 1 << 2, VQF_MAX = 1 << 3, VQF_FULLINTENSITY = 1 << 4, VQF_WAVE = 1 << 5 };

DEarthquake::DEarthquake (AActor *center, int intensityX, int intensityY, int intensityZ, int duration,
						  int damrad, int tremrad, FSoundID quakesound, int flags)
						  : DThinker(STAT_EARTHQUAKE)
{
	m_QuakeSFX = quakesound;
	m_Spot = center;
	m_DamageRadius = damrad << (FRACBITS);
	m_TremorRadius = tremrad << (FRACBITS);
	m_Intensity = intensityX;
	m_IntensityY = intensityY;
	m_IntensityZ = intensityZ;
	m_Countdown = duration;
	m_CountdownStart = duration;
	m_Flags = flags;
}

fixed_t DEarthquake::GetModIntensity(int intensity) const
{
	fixed_t in = intensity << FRACBITS;
	if ((m_Flags & (VQF_SCALEDOWN | VQF_SCALEUP)) && m_CountdownStart > 0)
	{
		int scalar;
		if ((m_Flags & (VQF_SCALEDOWN | VQF_SCALEUP)) == (VQF_SCALEDOWN | VQF_SCALEUP))
		{
			scalar = (m_Flags & VQF_MAX) ? MAX(m_Countdown, m_CountdownStart - m_Countdown) : MIN(m_Countdown, m_CountdownStart - m_Countdown);
			if (m_Flags & VQF_FULLINTENSITY) scalar *= 2;
		}
		else if (m_Flags & VQF_SCALEDOWN) scalar = m_Countdown;
		else scalar = m_CountdownStart - m_Countdown;
		in = Scale(in, scalar, m_CountdownStart);
	}
	return in;
}

bool DEarthquake::StaticGetQuakeIntensities (AActor *victim, fixed_t &x, fixed_t &y, fixed_t &z)
{
	x = y = z = 0;
	if (victim->player != NULL && (victim->player->cheats & CF_NOCLIP)) return false;
	TThinkerIterator<DEarthquake> iterator (STAT_EARTHQUAKE);
	DEarthquake *quake;
	bool any = false;
	while ( (quake = iterator.Next()) != NULL)
	{
		if (quake->m_Spot == NULL) continue;
		fixed_t dist = P_AproxDistance (victim->x - quake->m_Spot->x, victim->y - quake->m_Spot->y);
		if (dist >= quake->m_TremorRadius) continue;
		any = true;
		x = MAX(x, quake->GetModIntensity(quake->m_Intensity));
		y = MAX(y, quake->GetModIntensity(quake->m_IntensityY));
		z = MAX(z, quake->GetModIntensity(quake->m_IntensityZ));
	}
	return any;
}

bool P_StartQuakeXYZ (AActor *activator, int tid, int intensityX, int intensityY, int intensityZ, int duration, int damrad, int tremrad, FSoundID quakesfx, int flags)
{
	if (intensityX) intensityX = clamp(intensityX, 1, 9);
	if (intensityY) intensityY = clamp(intensityY, 1, 9);
	if (intensityZ) intensityZ = clamp(intensityZ, 1, 9);
	bool res = false;
	if (tid == 0)
	{
		if (activator != NULL)
		{
			new DEarthquake(activator, intensityX, intensityY, intensityZ, duration, damrad, tremrad, quakesfx, flags);
			return true;
		}
	}
	else
	{
		FActorIterator iterator (tid);
		AActor *center;
		while ( (center = iterator.Next ()) )
		{
			res = true;
			new DEarthquake(center, intensityX, intensityY, intensityZ, duration, damrad, tremrad, quakesfx, flags);
		}
	}
	return res;
}

//==========================================================================
//
// DEarthquake :: Serialize
//
//==========================================================================

void DEarthquake::Serialize (FArchive &arc)
{
	Super::Serialize (arc);
	arc << m_Spot << m_Intensity << m_Countdown
		<< m_TremorRadius << m_DamageRadius
		<< m_QuakeSFX;
	arc << m_IntensityY << m_IntensityZ << m_Flags << m_CountdownStart;	// [Vita]
}

//==========================================================================
//
// DEarthquake :: Tick
//
// Deals damage to any players near the earthquake and makes sure it's
// making noise.
//
//==========================================================================

void DEarthquake::Tick ()
{
	int i;

	if (m_Spot == NULL)
	{
		Destroy ();
		return;
	}

	if (!S_IsActorPlayingSomething (m_Spot, CHAN_BODY, m_QuakeSFX))
	{
		S_Sound (m_Spot, CHAN_BODY | CHAN_LOOP, m_QuakeSFX, 1, ATTN_NORM);
	}
	if (m_DamageRadius > 0)
	{
		for (i = 0; i < MAXPLAYERS; i++)
		{
			if (playeringame[i] && !(players[i].cheats & CF_NOCLIP))
			{
				AActor *victim = players[i].mo;
				fixed_t dist;

				dist = P_AproxDistance (victim->x - m_Spot->x, victim->y - m_Spot->y);
				// Check if in damage radius
				if (dist < m_DamageRadius && victim->z <= victim->floorz)
				{
					if (pr_quake() < 50)
					{
						P_DamageMobj (victim, NULL, NULL, pr_quake.HitDice (1), NAME_None);
					}
					// Thrust player around
					angle_t an = victim->angle + ANGLE_1*pr_quake();
					P_ThrustMobj (victim, an, m_Intensity << (FRACBITS-1));
				}
			}
		}
	}
	if (--m_Countdown == 0)
	{
		if (S_IsActorPlayingSomething(m_Spot, CHAN_BODY, m_QuakeSFX))
		{
			S_StopSound(m_Spot, CHAN_BODY);
		}
		Destroy();
	}
}

//==========================================================================
//
// DEarthquake::StaticGetQuakeIntensity
//
// Searches for all quakes near the victim and returns their combined
// intensity.
//
//==========================================================================

int DEarthquake::StaticGetQuakeIntensity (AActor *victim)
{
	int intensity = 0;
	TThinkerIterator<DEarthquake> iterator (STAT_EARTHQUAKE);
	DEarthquake *quake;

	if (victim->player != NULL && (victim->player->cheats & CF_NOCLIP))
	{
		return 0;
	}

	while ( (quake = iterator.Next()) != NULL)
	{
		if (quake->m_Spot != NULL)
		{
			fixed_t dist = P_AproxDistance (victim->x - quake->m_Spot->x,
				victim->y - quake->m_Spot->y);
			if (dist < quake->m_TremorRadius)
			{
				if (intensity < quake->m_Intensity)
					intensity = quake->m_Intensity;
			}
		}
	}
	return intensity;
}

//==========================================================================
//
// P_StartQuake
//
//==========================================================================

bool P_StartQuake (AActor *activator, int tid, int intensity, int duration, int damrad, int tremrad, FSoundID quakesfx)
{
	AActor *center;
	bool res = false;

	intensity = clamp (intensity, 1, 9);

	if (tid == 0)
	{
		if (activator != NULL)
		{
			new DEarthquake(activator, intensity, duration, damrad, tremrad, quakesfx);
			return true;
		}
	}
	else
	{
		FActorIterator iterator (tid);
		while ( (center = iterator.Next ()) )
		{
			res = true;
			new DEarthquake (center, intensity, duration, damrad, tremrad, quakesfx);
		}
	}
	
	return res;
}
