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
#include "r_utility.h"	// [Vita] r_TicFrac for QF_WAVE
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
	m_WaveSpeedX = m_WaveSpeedY = m_WaveSpeedZ = 0;	// [Vita]
}

// [Vita] ZDoom 2.8's A_QuakeEx
enum { VQF_RELATIVE = 1, VQF_SCALEDOWN = 1 << 1, VQF_SCALEUP = 1 << 2, VQF_MAX = 1 << 3, VQF_FULLINTENSITY = 1 << 4, VQF_WAVE = 1 << 5 };

DEarthquake::DEarthquake (AActor *center, int intensityX, int intensityY, int intensityZ, int duration,
						  int damrad, int tremrad, FSoundID quakesound, int flags,
						  double waveSpeedX, double waveSpeedY, double waveSpeedZ)
						  : DThinker(STAT_EARTHQUAKE)
{
	m_WaveSpeedX = waveSpeedX;
	m_WaveSpeedY = waveSpeedY;
	m_WaveSpeedZ = waveSpeedZ;
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

// [Vita] ZDoom 2.8: QF_WAVE turns the intensity into the amplitude of a sine
// wave (waveMultiplier = full waves per second) instead of random jitter.
fixed_t DEarthquake::GetModWave(double waveMultiplier) const
{
	double time = m_Countdown - FIXED2DBL(r_TicFrac);
	return FLOAT2FIXED(sin(waveMultiplier * time * (M_PI * 2 / TICRATE)));
}

// [Vita] ZDoom 2.8: per-axis intensities; QF_RELATIVE ones follow the camera's
// angle, QF_WAVE ones are offsets on a sine wave.
int DEarthquake::StaticGetQuakeIntensities (AActor *victim, FQuakeJiggers &jiggers)
{
	if (victim->player != NULL && (victim->player->cheats & CF_NOCLIP)) return 0;
	TThinkerIterator<DEarthquake> iterator (STAT_EARTHQUAKE);
	DEarthquake *quake;
	int count = 0;
	while ( (quake = iterator.Next()) != NULL)
	{
		if (quake->m_Spot == NULL) continue;
		fixed_t dist = P_AproxDistance (victim->x - quake->m_Spot->x, victim->y - quake->m_Spot->y);
		if (dist >= quake->m_TremorRadius) continue;
		++count;
		// Doubled, as in ZDoom 2.8 (the jitter is +-2 x intensity).
		fixed_t x = 2 * quake->GetModIntensity(quake->m_Intensity);
		fixed_t y = 2 * quake->GetModIntensity(quake->m_IntensityY);
		fixed_t z = 2 * quake->GetModIntensity(quake->m_IntensityZ);
		if (!(quake->m_Flags & VQF_WAVE))
		{
			if (quake->m_Flags & VQF_RELATIVE)
			{
				jiggers.RelIntensityX = MAX(x, jiggers.RelIntensityX);
				jiggers.RelIntensityY = MAX(y, jiggers.RelIntensityY);
				jiggers.RelIntensityZ = MAX(z, jiggers.RelIntensityZ);
			}
			else
			{
				jiggers.IntensityX = MAX(x, jiggers.IntensityX);
				jiggers.IntensityY = MAX(y, jiggers.IntensityY);
				jiggers.IntensityZ = MAX(z, jiggers.IntensityZ);
			}
		}
		else
		{
			fixed_t mx = FixedMul(x, quake->GetModWave(quake->m_WaveSpeedX));
			fixed_t my = FixedMul(y, quake->GetModWave(quake->m_WaveSpeedY));
			fixed_t mz = FixedMul(z, quake->GetModWave(quake->m_WaveSpeedZ));
			// Only the last wave quake counts, as in ZDoom 2.8.
			if (quake->m_Flags & VQF_RELATIVE)
			{
				jiggers.RelOffsetX = mx;
				jiggers.RelOffsetY = my;
				jiggers.RelOffsetZ = mz;
			}
			else
			{
				jiggers.OffsetX = mx;
				jiggers.OffsetY = my;
				jiggers.OffsetZ = mz;
			}
		}
	}
	return count;
}

bool P_StartQuakeXYZ (AActor *activator, int tid, int intensityX, int intensityY, int intensityZ, int duration, int damrad, int tremrad, FSoundID quakesfx, int flags,
	double waveSpeedX, double waveSpeedY, double waveSpeedZ)
{
	if (intensityX) intensityX = clamp(intensityX, 1, 9);
	if (intensityY) intensityY = clamp(intensityY, 1, 9);
	if (intensityZ) intensityZ = clamp(intensityZ, 1, 9);
	bool res = false;
	if (tid == 0)
	{
		if (activator != NULL)
		{
			new DEarthquake(activator, intensityX, intensityY, intensityZ, duration, damrad, tremrad, quakesfx, flags, waveSpeedX, waveSpeedY, waveSpeedZ);
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
			new DEarthquake(center, intensityX, intensityY, intensityZ, duration, damrad, tremrad, quakesfx, flags, waveSpeedX, waveSpeedY, waveSpeedZ);
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
	if (SaveVersion >= 4507)
		arc << m_WaveSpeedX << m_WaveSpeedY << m_WaveSpeedZ;	// [Vita] QF_WAVE
	else if (arc.IsLoading())
		m_WaveSpeedX = m_WaveSpeedY = m_WaveSpeedZ = 0;
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
