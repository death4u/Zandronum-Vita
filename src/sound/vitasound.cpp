#include <new>
/*
** vitasound.cpp
**
** PS Vita sound renderer: a small software mixer on top of SDL 1.2 audio,
** replacing FMOD Ex (which has no Vita build).
**
** - Sound effects are decoded to 16-bit PCM when loaded (raw/DMX, WAV, Ogg
**   Vorbis, FLAC and MP3) and mixed with linear interpolation into a 48 kHz
**   stereo float bus. 3D sounds get distance attenuation through the game's
**   own rolloff curves (S_GetRolloff) and equal-power stereo panning, which
**   mirrors what the FMOD renderer asks FMOD to do.
** - Music and other streams (OPL/Timidity/GME/DUMB callbacks, and Ogg, FLAC,
**   MP3 and WAV files) are produced by a feeder thread into per-stream ring
**   buffers, so slow synthesizers never stall the audio callback.
**
** Threading: the SDL audio callback (mixer) and the feeder thread run beside
** the game thread. Voice and stream lists are changed only with the audio
** lock held (SDL_LockAudio); stream callbacks run with FeederMutex held, and
** stopping or deleting a stream takes that mutex, so a callback never runs
** on a stream that is being destroyed.
*/

#if defined(__vita__) && defined(NO_SOUND)

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>

#include <SDL.h>
#include <SDL_thread.h>

#include "doomtype.h"
#include "doomdef.h"
#include "templates.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "i_system.h"
#include "v_text.h"
#include "files.h"
#include "i_sound.h"
#include "i_soundinternal.h"
#include "vitasound.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "thirdparty/stb_vorbis.c"
#undef L
#undef R
#undef C
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "thirdparty/dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "thirdparty/dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "thirdparty/dr_mp3.h"

EXTERN_CVAR (Bool, snd_pitched)

// Output format.
enum
{
	OUT_RATE = 48000,
	OUT_FRAMES = 1024,		// per SDL callback (~21 ms)
	MAX_VOICES = 64,		// the game itself limits to snd_channels
	STREAM_RING_FRAMES = 32768,	// per stream, at its own rate (~0.7 s at 48 kHz)
};

//==========================================================================
//
// Samples
//
//==========================================================================

struct VitaSample
{
	short *Data;			// interleaved
	unsigned Frames;
	int Channels;			// 1 or 2
	int Rate;
	int LoopStart, LoopEnd;	// frames; LoopEnd exclusive

	// Compressed sounds are decoded on a background thread (decoding a
	// long MP3 or Ogg on the game thread stalled play for a second or
	// more the first time it was heard). Until Ready, voices using the
	// sample stay silent at position 0.
	std::atomic<int> Ready;
	BYTE *Src;				// compressed data while pending
	int SrcLen;
	bool Unloaded;			// UnloadSound called while it was being decoded
};

static VitaSample *MakeSample(short *data, unsigned frames, int channels, int rate)
{
	VitaSample *s = new VitaSample;
	s->Data = data;
	s->Frames = frames;
	s->Channels = channels;
	s->Rate = rate;
	s->LoopStart = 0;
	s->LoopEnd = frames;
	s->Src = NULL;
	s->SrcLen = 0;
	s->Unloaded = false;
	s->Ready.store(1);
	return s;
}

static inline bool SampleReady(VitaSample *s)
{
	return s->Ready.load(std::memory_order_acquire) != 0;
}

//==========================================================================
//
// Voices (playing sound effects)
//
//==========================================================================

struct VitaVoice
{
	bool Active;
	bool Ended;				// reached the end; reported in UpdateSounds
	bool Stopped;			// stopped by the game; silent until released
	VitaSample *Sample;
	uint64_t Pos;			// 32.32 frame position
	uint64_t Step;			// 32.32 frames per output frame (0: sample still decoding)
	int Pitch;
	bool Loop;
	bool Pausable;			// not SNDF_NOPAUSE
	bool Is3D;
	bool Area;
	float Volume;			// game volume
	float Attenuation;		// distance rolloff (3D only)
	float TargetL, TargetR;	// gains applied this buffer
	float CurL, CurR;		// gains at the end of the last buffer (ramping)
	FVector3 Position;
	FISoundChannel *Chan;
};

//==========================================================================
//
// Streams
//
//==========================================================================

class VitaSoundRenderer;

class VitaStream : public SoundStream
{
public:
	VitaStream(VitaSoundRenderer *owner, int rate, int channels)
		: Owner(owner), Rate(rate), Channels(channels)
	{
		Ring = new float[STREAM_RING_FRAMES * 2];
		ReadPos = 0;
		WritePos = 0;
		Frac = 0;
		Step = (uint64_t(rate) << 32) / OUT_RATE;
		Playing = false;
		Paused = false;
		SourceEnded = false;
		Volume = 1;
		Looping = false;
		PlayedFrames = 0;
	}
	virtual ~VitaStream();

	bool Play(bool looping, float volume);
	void Stop();
	void SetVolume(float volume) { Volume = volume; }
	bool SetPaused(bool paused) { Paused = paused; return true; }
	unsigned int GetPosition() { return unsigned(PlayedFrames.load() * 1000 / Rate); }
	bool IsEnded() { return !Playing || (SourceEnded && ReadPos.load() == WritePos.load()); }

	// Produces up to 'frames' native-rate frames into 'out' (interleaved,
	// Channels wide). Returns the number made; fewer means the source ended.
	virtual int Produce(float *out, int frames) = 0;
	// Preferred amount of frames per Produce call.
	virtual int ChunkFrames() const = 0;
	virtual void Restart() {}
	virtual bool IsFile() const { return false; }

	// Feeder thread: top up the ring buffer.
	void Feed();
	// Mixer: add this stream into the bus.
	void Mix(float *bus, int frames, float gain);

	VitaSoundRenderer *Owner;
	int Rate, Channels;
	float *Ring;			// stereo frames
	std::atomic<unsigned> ReadPos, WritePos;	// in frames, free-running
	uint32_t Frac;			// 0.32 fractional read position
	uint64_t Step;
	volatile bool Playing, Paused, SourceEnded, Looping;
	volatile float Volume;
	std::atomic<uint64_t> PlayedFrames;
};

// Stream fed by a game callback (OPL, Timidity, GME, DUMB...).
class VitaCallbackStream : public VitaStream
{
public:
	VitaCallbackStream(VitaSoundRenderer *owner, SoundStreamCallback callback, int buffbytes, int flags, int rate, void *userdata)
		: VitaStream(owner, rate, (flags & Mono) ? 1 : 2), Callback(callback), UserData(userdata), Flags(flags)
	{
		SampleBytes = (flags & (Bits32 | Float)) ? 4 : (flags & Bits8) ? 1 : 2;
		BuffBytes = buffbytes;
		Frames = buffbytes / (SampleBytes * Channels);
		Buffer = new BYTE[buffbytes];
	}
	~VitaCallbackStream() { delete[] Buffer; }

	int ChunkFrames() const { return Frames; }
	int Produce(float *out, int frames);

	SoundStreamCallback Callback;
	void *UserData;
	int Flags, SampleBytes, BuffBytes, Frames;
	BYTE *Buffer;
};

// Stream decoded from a music file in memory.
class VitaFileStream : public VitaStream
{
public:
	enum EFormat { FMT_Vorbis, FMT_Flac, FMT_MP3, FMT_Wav };

	VitaFileStream(VitaSoundRenderer *owner, BYTE *data, int length, EFormat fmt, int rate, int channels)
		: VitaStream(owner, rate, channels), Data(data), Length(length), Format(fmt)
	{
		Vorbis = NULL; Flac = NULL; Mp3 = NULL; Wav = NULL;
	}
	~VitaFileStream();

	static VitaFileStream *Open(VitaSoundRenderer *owner, BYTE *data, int length);

	int ChunkFrames() const { return 2048; }
	int Produce(float *out, int frames);
	void Restart();
	bool IsFile() const { return true; }
	bool SetPosition(unsigned int ms);

	BYTE *Data;
	int Length;
	EFormat Format;
	stb_vorbis *Vorbis;
	drflac *Flac;
	drmp3 *Mp3;
	drwav *Wav;
};

//==========================================================================
//
// The renderer
//
//==========================================================================

class VitaSoundRenderer : public SoundRenderer
{
public:
	VitaSoundRenderer();
	~VitaSoundRenderer();

	bool IsValid() { return Valid; }
	void SetSfxVolume(float volume) { SfxVolume = volume; }
	void SetMusicVolume(float volume) { MusicVolume = volume; }
	SoundHandle LoadSound(BYTE *sfxdata, int length);
	SoundHandle LoadSoundRaw(BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend = -1);
	void UnloadSound(SoundHandle sfx);
	unsigned int GetMSLength(SoundHandle sfx);
	unsigned int GetSampleLength(SoundHandle sfx);
	float GetOutputRate() { return OUT_RATE; }

	SoundStream *CreateStream(SoundStreamCallback callback, int buffbytes, int flags, int samplerate, void *userdata);
	SoundStream *OpenStream(const char *filename, int flags, int offset, int length);

	FISoundChannel *StartSound(SoundHandle sfx, float vol, int pitch, int chanflags, FISoundChannel *reuse_chan);
	FISoundChannel *StartSound3D(SoundHandle sfx, SoundListener *listener, float vol, FRolloffInfo *rolloff, float distscale, int pitch, int priority, const FVector3 &pos, const FVector3 &vel, int channum, int chanflags, FISoundChannel *reuse_chan);
	void StopChannel(FISoundChannel *chan);
	void ChannelVolume(FISoundChannel *chan, float volume);
	void MarkStartTime(FISoundChannel *chan);
	unsigned int GetPosition(FISoundChannel *chan);
	float GetAudibility(FISoundChannel *chan);
	void Sync(bool sync) {}
	void SetSfxPaused(bool paused, int slot);
	void SetInactive(EInactiveState inactive);
	void UpdateSoundParams3D(SoundListener *listener, FISoundChannel *chan, bool areasound, const FVector3 &pos, const FVector3 &vel);
	void UpdateListener(SoundListener *listener);
	void UpdateSounds();
	void PrintStatus();
	void PrintDriversList();
	FString GatherStats();

	// Used by streams.
	void AddStream(VitaStream *stream);
	void RemoveStream(VitaStream *stream);
	SDL_mutex *FeederMutex;

private:
	static void AudioCallback(void *userdata, Uint8 *stream, int len);
	static int FeederThread(void *userdata);
	void Mix(short *out, int frames);
	VitaVoice *AllocVoice();
	VitaVoice *FindVoice(FISoundChannel *chan);
	FISoundChannel *Start(VitaSample *s, float vol, int pitch, int flags, FISoundChannel *reuse_chan, bool is3d, const FVector3 *pos, bool area, SoundListener *listener, FRolloffInfo *rolloff, float distscale);
	void Spatialize(VitaVoice *v, const SoundListener *listener, const FRolloffInfo *rolloff, float distscale);
	void SetStereoGains(VitaVoice *v);
	bool HandleChannelDelay(VitaVoice *v, FISoundChannel *reuse_chan, int flags, float freq);

	bool Valid;
	bool AudioOpen;
	volatile float SfxVolume, MusicVolume;
	volatile int SfxPausedSlots;
	volatile bool Inactive, Muted;
	std::atomic<uint64_t> DSPClock;	// output frames mixed so far

	VitaVoice Voices[MAX_VOICES];
	TArray<VitaStream *> Streams;		// guarded by the audio lock (mixer) and FeederMutex (feeder)
	SDL_Thread *Feeder;
	volatile bool FeederQuit;
	float *Bus;

	SoundListener Listener;			// last listener given to us
	bool ListenerValid;

	// Diagnostics: if ux0:data/zandronum/audiodump.txt exists at start-up, the
	// first seconds of mixed output are written to audiodump.wav.
	short *DumpBuf;
	unsigned DumpFrames;
	std::atomic<unsigned> DumpPos;
	bool DumpWritten;
	void WriteDump();
};

//==========================================================================
//
// Helpers
//
//==========================================================================

static inline float PitchFreq(float freq, int pitch)
{
	return snd_pitched ? freq * pitch / 128.f : freq;
}

//==========================================================================
//
// VitaStream
//
//==========================================================================

VitaStream::~VitaStream()
{
	Stop();
	Owner->RemoveStream(this);
	delete[] Ring;
}

bool VitaStream::Play(bool looping, float volume)
{
	Looping = looping;
	Volume = volume;
	Paused = false;
	SourceEnded = false;
	Playing = true;
	Owner->AddStream(this);
	return true;
}

void VitaStream::Stop()
{
	// Wait for the feeder to leave this stream's callback.
	SDL_mutexP(Owner->FeederMutex);
	Playing = false;
	SDL_mutexV(Owner->FeederMutex);
}

void VitaStream::Feed()
{
	if (!Playing || SourceEnded)
		return;

	float temp[4096 * 2];
	for (;;)
	{
		unsigned used = WritePos.load() - ReadPos.load();
		unsigned freeframes = STREAM_RING_FRAMES - used;
		int chunk = ChunkFrames();
		if (chunk > 4096) chunk = 4096;
		if ((int)freeframes < chunk)
			break;

		int got = Produce(temp, chunk);
		if (got < 0)
			got = 0;
		bool ended = got < chunk;
		if (ended && Looping && IsFile())
		{	// File streams loop by rewinding the decoder.
			Restart();
			ended = false;
		}

		unsigned w = WritePos.load();
		for (int i = 0; i < got; ++i)
		{
			unsigned slot = (w + i) % STREAM_RING_FRAMES;
			if (Channels == 1)
			{
				Ring[slot * 2] = Ring[slot * 2 + 1] = temp[i];
			}
			else
			{
				Ring[slot * 2] = temp[i * 2];
				Ring[slot * 2 + 1] = temp[i * 2 + 1];
			}
		}
		WritePos.store(w + got);
		if (ended)
		{
			SourceEnded = true;
			break;
		}
		if (got == 0)
			break;	// try again on the next pass
	}
}

void VitaStream::Mix(float *bus, int frames, float gain)
{
	if (!Playing || Paused)
		return;

	unsigned r = ReadPos.load();
	unsigned avail = WritePos.load() - r;
	unsigned consumed = 0;
	for (int i = 0; i < frames; ++i)
	{
		// Need the frame at 'consumed' and the next one for interpolation.
		if (consumed + 1 >= avail)
			break;	// starved: the rest of this buffer stays silent
		unsigned a = (r + consumed) % STREAM_RING_FRAMES;
		unsigned b = (r + consumed + 1) % STREAM_RING_FRAMES;
		float t = Frac * (1.f / 4294967296.f);
		bus[i * 2] += gain * (Ring[a * 2] + (Ring[b * 2] - Ring[a * 2]) * t);
		bus[i * 2 + 1] += gain * (Ring[a * 2 + 1] + (Ring[b * 2 + 1] - Ring[a * 2 + 1]) * t);
		uint64_t f = uint64_t(Frac) + (Step & 0xFFFFFFFFu);
		consumed += unsigned(Step >> 32) + unsigned(f >> 32);
		Frac = uint32_t(f);
	}
	if (SourceEnded && consumed + 1 >= avail)
		consumed = avail;	// drain the last frame so IsEnded() can trigger
	ReadPos.store(r + consumed);
	PlayedFrames.store(PlayedFrames.load() + consumed);
}

//==========================================================================
//
// VitaCallbackStream
//
//==========================================================================

int VitaCallbackStream::Produce(float *out, int frames)
{
	if (frames > Frames)
		frames = Frames;
	int bytes = frames * SampleBytes * Channels;
	if (!Callback(this, Buffer, bytes, UserData))
		return 0;

	int n = frames * Channels;
	if (Flags & Float)
	{
		memcpy(out, Buffer, n * sizeof(float));
	}
	else if (Flags & Bits32)
	{
		const int *in = (const int *)Buffer;
		for (int i = 0; i < n; ++i) out[i] = in[i] * (1.f / 2147483648.f);
	}
	else if (Flags & Bits8)
	{
		const signed char *in = (const signed char *)Buffer;
		for (int i = 0; i < n; ++i) out[i] = in[i] * (1.f / 128.f);
	}
	else
	{
		const short *in = (const short *)Buffer;
		for (int i = 0; i < n; ++i) out[i] = in[i] * (1.f / 32768.f);
	}
	return frames;
}

//==========================================================================
//
// VitaFileStream
//
//==========================================================================

VitaFileStream *VitaFileStream::Open(VitaSoundRenderer *owner, BYTE *data, int length)
{
	VitaFileStream *s = NULL;
	int err;

	if (length >= 4 && memcmp(data, "OggS", 4) == 0)
	{
		stb_vorbis *v = stb_vorbis_open_memory(data, length, &err, NULL);
		if (v == NULL) return NULL;
		stb_vorbis_info info = stb_vorbis_get_info(v);
		if (info.channels < 1 || info.channels > 2) { stb_vorbis_close(v); return NULL; }
		s = new VitaFileStream(owner, data, length, FMT_Vorbis, info.sample_rate, info.channels);
		s->Vorbis = v;
		return s;
	}
	if (length >= 4 && memcmp(data, "fLaC", 4) == 0)
	{
		drflac *f = drflac_open_memory(data, length, NULL);
		if (f == NULL) return NULL;
		if (f->channels < 1 || f->channels > 2) { drflac_close(f); return NULL; }
		s = new VitaFileStream(owner, data, length, FMT_Flac, f->sampleRate, f->channels);
		s->Flac = f;
		return s;
	}
	if (length >= 4 && (memcmp(data, "RIFF", 4) == 0 || memcmp(data, "RIFX", 4) == 0))
	{
		drwav *w = new drwav;
		if (!drwav_init_memory(w, data, length, NULL)) { delete w; return NULL; }
		if (w->channels < 1 || w->channels > 2) { drwav_uninit(w); delete w; return NULL; }
		s = new VitaFileStream(owner, data, length, FMT_Wav, w->sampleRate, w->channels);
		s->Wav = w;
		return s;
	}
	// MP3: ID3 tag or a frame sync.
	if (length >= 3 && (memcmp(data, "ID3", 3) == 0 || (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0)))
	{
		drmp3 *m = new drmp3;
		if (!drmp3_init_memory(m, data, length, NULL)) { delete m; return NULL; }
		if (m->channels < 1 || m->channels > 2) { drmp3_uninit(m); delete m; return NULL; }
		s = new VitaFileStream(owner, data, length, FMT_MP3, m->sampleRate, m->channels);
		s->Mp3 = m;
		return s;
	}
	return NULL;
}

VitaFileStream::~VitaFileStream()
{
	// The base destructor stops the stream first, but the decoder must not be
	// torn down while the feeder could be decoding.
	Stop();
	Owner->RemoveStream(this);
	if (Vorbis) stb_vorbis_close(Vorbis);
	if (Flac) drflac_close(Flac);
	if (Mp3) { drmp3_uninit(Mp3); delete Mp3; }
	if (Wav) { drwav_uninit(Wav); delete Wav; }
	delete[] Data;
}

int VitaFileStream::Produce(float *out, int frames)
{
	switch (Format)
	{
	case FMT_Vorbis:
		return stb_vorbis_get_samples_float_interleaved(Vorbis, Channels, out, frames * Channels);
	case FMT_Flac:
		return (int)drflac_read_pcm_frames_f32(Flac, frames, out);
	case FMT_MP3:
		return (int)drmp3_read_pcm_frames_f32(Mp3, frames, out);
	case FMT_Wav:
		return (int)drwav_read_pcm_frames_f32(Wav, frames, out);
	}
	return 0;
}

void VitaFileStream::Restart()
{
	switch (Format)
	{
	case FMT_Vorbis: stb_vorbis_seek_start(Vorbis); break;
	case FMT_Flac: drflac_seek_to_pcm_frame(Flac, 0); break;
	case FMT_MP3: drmp3_seek_to_pcm_frame(Mp3, 0); break;
	case FMT_Wav: drwav_seek_to_pcm_frame(Wav, 0); break;
	}
}

bool VitaFileStream::SetPosition(unsigned int ms)
{
	uint64_t frame = uint64_t(ms) * Rate / 1000;
	SDL_mutexP(Owner->FeederMutex);
	bool ok = true;
	switch (Format)
	{
	case FMT_Vorbis: ok = !!stb_vorbis_seek(Vorbis, (unsigned)frame); break;
	case FMT_Flac: ok = !!drflac_seek_to_pcm_frame(Flac, frame); break;
	case FMT_MP3: ok = !!drmp3_seek_to_pcm_frame(Mp3, frame); break;
	case FMT_Wav: ok = !!drwav_seek_to_pcm_frame(Wav, frame); break;
	}
	SDL_LockAudio();
	ReadPos.store(WritePos.load());	// drop what was buffered before the seek
	PlayedFrames.store(frame);
	SourceEnded = false;
	SDL_UnlockAudio();
	SDL_mutexV(Owner->FeederMutex);
	return ok;
}

//==========================================================================
//
// VitaSoundRenderer :: VitaSoundRenderer
//
//==========================================================================

VitaSoundRenderer::VitaSoundRenderer()
{
	Valid = false;
	AudioOpen = false;
	SfxVolume = 1;
	MusicVolume = 1;
	SfxPausedSlots = 0;
	Inactive = false;
	Muted = false;
	DSPClock = 0;
	Feeder = NULL;
	FeederQuit = false;
	ListenerValid = false;
	memset(&Listener, 0, sizeof(Listener));
	DumpBuf = NULL;
	DumpFrames = 0;
	DumpPos = 0;
	DumpWritten = false;
	{
		FILE *f = fopen("ux0:data/zandronum/audiodump.txt", "r");
		if (f != NULL)
		{
			int seconds = 30;
			if (fscanf(f, "%d", &seconds) != 1 || seconds <= 0 || seconds > 120)
				seconds = 30;
			fclose(f);
			DumpFrames = seconds * OUT_RATE;
			DumpBuf = new short[DumpFrames * 2];
		}
	}
	memset(Voices, 0, sizeof(Voices));
	Bus = new float[OUT_FRAMES * 2 * 2];
	FeederMutex = SDL_CreateMutex();

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		Printf(TEXTCOLOR_RED "Could not initialize SDL audio: %s\n", SDL_GetError());
		return;
	}

	SDL_AudioSpec want, have;
	memset(&want, 0, sizeof(want));
	want.freq = OUT_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = OUT_FRAMES;
	want.callback = AudioCallback;
	want.userdata = this;
	if (SDL_OpenAudio(&want, &have) < 0)
	{
		Printf(TEXTCOLOR_RED "Could not open audio: %s\n", SDL_GetError());
		return;
	}
	if (have.freq != OUT_RATE || have.channels != 2 || have.format != AUDIO_S16SYS || have.samples > OUT_FRAMES * 2)
	{
		Printf(TEXTCOLOR_RED "Unexpected audio format: %d Hz, %d channels, format %x, %d frames\n",
			have.freq, have.channels, have.format, have.samples);
		SDL_CloseAudio();
		return;
	}
	AudioOpen = true;

	Feeder = SDL_CreateThread(FeederThread, this);
	SDL_PauseAudio(0);
	Printf("Vita sound: %d Hz stereo, %d-frame buffer, software mixer\n", have.freq, have.samples);
	Valid = true;
}

VitaSoundRenderer::~VitaSoundRenderer()
{
	if (Feeder != NULL)
	{
		FeederQuit = true;
		SDL_WaitThread(Feeder, NULL);
		Feeder = NULL;
	}
	if (AudioOpen)
	{
		SDL_CloseAudio();
		AudioOpen = false;
	}
	// Report anything still playing as ended so the game frees its channels.
	for (int i = 0; i < MAX_VOICES; ++i)
	{
		if (Voices[i].Active && Voices[i].Chan != NULL)
		{
			FISoundChannel *c = Voices[i].Chan;
			Voices[i].Active = false;
			S_ChannelEnded(c);
		}
	}
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
	SDL_DestroyMutex(FeederMutex);
	delete[] Bus;
}

//==========================================================================
//
// Audio thread
//
//==========================================================================

void VitaSoundRenderer::AudioCallback(void *userdata, Uint8 *stream, int len)
{
	VitaSoundRenderer *self = (VitaSoundRenderer *)userdata;
	int frames = len / 4;
	short *out = (short *)stream;
	while (frames > 0)
	{
		int n = MIN<int>(frames, OUT_FRAMES * 2);
		self->Mix(out, n);
		out += n * 2;
		frames -= n;
	}
}

void VitaSoundRenderer::Mix(short *out, int frames)
{
	float *bus = Bus;
	memset(bus, 0, frames * 2 * sizeof(float));

	if (!Inactive)
	{
		float sfxvol = SfxVolume;
		bool sfxpaused = SfxPausedSlots != 0;
		float rampstep = 1.f / frames;

		for (int vi = 0; vi < MAX_VOICES; ++vi)
		{
			VitaVoice *v = &Voices[vi];
			if (!v->Active || v->Ended || v->Stopped || (v->Pausable && sfxpaused))
				continue;

			VitaSample *s = v->Sample;
			if (!SampleReady(s))
				continue;
			if (v->Step == 0)
			{
				float freq = PitchFreq(float(s->Rate), v->Pitch);
				if (freq <= 0) freq = float(s->Rate);
				v->Step = uint64_t(double(freq) / OUT_RATE * 4294967296.0);
				if (v->Step == 0) v->Step = 1;
			}
			const short *data = s->Data;
			uint64_t pos = v->Pos;
			uint64_t end = uint64_t(v->Loop ? s->LoopEnd : s->Frames) << 32;
			uint64_t loopstart = uint64_t(s->LoopStart) << 32;
			uint64_t looplen = end - loopstart;
			float l = v->CurL * sfxvol, r = v->CurR * sfxvol;
			float dl = (v->TargetL * sfxvol - l) * rampstep;
			float dr = (v->TargetR * sfxvol - r) * rampstep;
			unsigned last = s->Frames - 1;

			for (int i = 0; i < frames; ++i)
			{
				if (pos >= end)
				{
					if (v->Loop && looplen > 0)
					{
						pos = loopstart + (pos - end) % looplen;
					}
					else
					{
						v->Ended = true;
						break;
					}
				}
				unsigned idx = unsigned(pos >> 32);
				unsigned nxt = idx < last ? idx + 1 : (v->Loop ? s->LoopStart : idx);
				float t = (pos & 0xFFFFFFFFu) * (1.f / 4294967296.f);
				float a, b;
				if (s->Channels == 1)
				{
					a = data[idx] + (data[nxt] - data[idx]) * t;
					b = a;
				}
				else
				{
					a = data[idx * 2] + (data[nxt * 2] - data[idx * 2]) * t;
					b = data[idx * 2 + 1] + (data[nxt * 2 + 1] - data[idx * 2 + 1]) * t;
				}
				bus[i * 2] += a * l;
				bus[i * 2 + 1] += b * r;
				l += dl; r += dr;
				pos += v->Step;
			}
			v->Pos = pos;
			v->CurL = v->TargetL;
			v->CurR = v->TargetR;
		}

		// Sample data is 16-bit; streams are float in -1..1.
		float musvol = MusicVolume * 32768.f;
		for (unsigned i = 0; i < Streams.Size(); ++i)
		{
			VitaStream *s = Streams[i];
			s->Mix(bus, frames, s->Volume * musvol);
		}
	}

	// Soft clipping above about -3 dB, so several loud sounds at once (Doom's
	// weapons are mastered near full scale) saturate gently instead of
	// wrapping or hard clipping.
	const float knee = 0.7f * 32767.f, room = 32767.f - knee;
	for (int i = 0; i < frames * 2; ++i)
	{
		float f = Muted ? 0 : bus[i];
		float a = fabsf(f);
		if (a > knee)
		{
			float over = (a - knee) / room;
			a = knee + room * (over / (1.f + over));	// approaches full scale smoothly
			f = f < 0 ? -a : a;
		}
		out[i] = short(f);
	}
	DSPClock.store(DSPClock.load() + frames);

	if (DumpBuf != NULL)
	{
		unsigned p = DumpPos.load();
		unsigned n = MIN<unsigned>(frames, DumpFrames - p);
		memcpy(DumpBuf + p * 2, out, n * 4);
		DumpPos.store(p + n);
	}
}

void VitaSoundRenderer::WriteDump()
{
	FILE *f = fopen("ux0:data/zandronum/audiodump.wav", "wb");
	if (f == NULL)
		return;
	unsigned bytes = DumpFrames * 4;
	unsigned hdr[11] = { MAKE_ID('R','I','F','F'), 36 + bytes, MAKE_ID('W','A','V','E'), MAKE_ID('f','m','t',' '),
		16, 0x00020001, OUT_RATE, OUT_RATE * 4, 0x00100004, MAKE_ID('d','a','t','a'), bytes };
	fwrite(hdr, 4, 11, f);
	fwrite(DumpBuf, 4, DumpFrames, f);
	fclose(f);
	Printf("Wrote audiodump.wav\n");
}

//==========================================================================
//
// Feeder thread: keeps every stream's ring buffer topped up.
//
//==========================================================================

int VitaSoundRenderer::FeederThread(void *userdata)
{
	VitaSoundRenderer *self = (VitaSoundRenderer *)userdata;
	while (!self->FeederQuit)
	{
		SDL_mutexP(self->FeederMutex);
		for (unsigned i = 0; i < self->Streams.Size(); ++i)
		{
			self->Streams[i]->Feed();
		}
		SDL_mutexV(self->FeederMutex);
		SDL_Delay(5);
	}
	return 0;
}

void VitaSoundRenderer::AddStream(VitaStream *stream)
{
	SDL_mutexP(FeederMutex);
	SDL_LockAudio();
	bool found = false;
	for (unsigned i = 0; i < Streams.Size(); ++i)
		if (Streams[i] == stream) found = true;
	if (!found)
		Streams.Push(stream);
	SDL_UnlockAudio();
	SDL_mutexV(FeederMutex);
}

void VitaSoundRenderer::RemoveStream(VitaStream *stream)
{
	SDL_mutexP(FeederMutex);
	SDL_LockAudio();
	for (unsigned i = 0; i < Streams.Size(); ++i)
	{
		if (Streams[i] == stream)
		{
			Streams.Delete(i);
			break;
		}
	}
	SDL_UnlockAudio();
	SDL_mutexV(FeederMutex);
}

//==========================================================================
//
// Loading sounds
//
//==========================================================================

SoundHandle VitaSoundRenderer::LoadSoundRaw(BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend)
{
	SoundHandle retval = { NULL };
	if (length <= 0 || channels < 1 || channels > 2 || frequency <= 0)
		return retval;

	int bytes = bits == 32 ? 4 : bits == 16 ? 2 : (bits == 8 || bits == -8) ? 1 : 0;
	if (bytes == 0)
		return retval;
	unsigned frames = length / (bytes * channels);
	if (frames == 0)
		return retval;

	short *pcm = new short[frames * channels];
	unsigned n = frames * channels;
	switch (bits)
	{
	case 8:		// unsigned
		for (unsigned i = 0; i < n; ++i) pcm[i] = short((sfxdata[i] - 128) << 8);
		break;
	case -8:	// signed
		for (unsigned i = 0; i < n; ++i) pcm[i] = short(((signed char *)sfxdata)[i] << 8);
		break;
	case 16:
		memcpy(pcm, sfxdata, n * 2);
		break;
	case 32:
		for (unsigned i = 0; i < n; ++i) pcm[i] = short(((int *)sfxdata)[i] >> 16);
		break;
	}

	VitaSample *s = MakeSample(pcm, frames, channels, frequency);
	if (loopstart >= 0 && (unsigned)loopstart < frames)
	{
		s->LoopStart = loopstart;
		s->LoopEnd = (loopend == -1 || (unsigned)loopend >= frames) ? frames : loopend + 1;
		if (s->LoopEnd <= s->LoopStart)
			s->LoopEnd = frames;
	}
	retval.data = s;
	return retval;
}

// Decodes a compressed (or WAV) sound to 16-bit PCM, 1 or 2 channels.
static bool DecodeSound(BYTE *sfxdata, int length, short *&pcm_out, unsigned &frames_out, int &channels_out, int &rate_out)
{
	short *pcm = NULL;
	unsigned frames = 0;
	int channels = 0, rate = 0;

	if (length >= 4 && memcmp(sfxdata, "OggS", 4) == 0)
	{
		short *out = NULL;
		int n = stb_vorbis_decode_memory(sfxdata, length, &channels, &rate, &out);
		if (n > 0 && out != NULL)
		{	// stb uses malloc; copy so UnloadSound can use delete[]
			// [Vita] Long sounds (HaloDoom's ambience loops and cutscene
			// music, 10-20 MB as PCM) are kept as mono at half rate when they
			// would take more than 2 MB: 1/4 of the memory.
			if ((size_t)n * channels * sizeof(short) > (2u << 20) && rate >= 32000)
			{
				const int outn = n / 2;
				pcm = new (std::nothrow) short[outn];
				if (pcm != NULL)
				{
					for (int i = 0; i < outn; i++)
					{
						int acc = 0;
						for (int c = 0; c < channels; c++)
							acc += out[(i * 2) * channels + c] + out[(i * 2 + 1) * channels + c];
						pcm[i] = (short)(acc / (2 * channels));
					}
				}
				frames = outn;
				channels = 1;
				rate /= 2;
			}
			else
			{
				pcm = new (std::nothrow) short[n * channels];	// [Vita] no memory: the sound stays silent
				if (pcm != NULL) memcpy(pcm, out, n * channels * sizeof(short));
				frames = n;
			}
		}
		free(out);
	}
	else if (length >= 4 && memcmp(sfxdata, "fLaC", 4) == 0)
	{
		unsigned int ch, sr;
		drflac_uint64 count;
		drflac_int16 *out = drflac_open_memory_and_read_pcm_frames_s16(sfxdata, length, &ch, &sr, &count, NULL);
		if (out != NULL)
		{
			channels = ch; rate = sr; frames = (unsigned)count;
			pcm = new (std::nothrow) short[frames * channels];
			if (pcm != NULL) memcpy(pcm, out, frames * channels * sizeof(short));
			drflac_free(out, NULL);
		}
	}
	else if (length >= 4 && (memcmp(sfxdata, "RIFF", 4) == 0 || memcmp(sfxdata, "RIFX", 4) == 0))
	{
		unsigned int ch, sr;
		drwav_uint64 count;
		drwav_int16 *out = drwav_open_memory_and_read_pcm_frames_s16(sfxdata, length, &ch, &sr, &count, NULL);
		if (out != NULL)
		{
			channels = ch; rate = sr; frames = (unsigned)count;
			pcm = new (std::nothrow) short[frames * channels];
			if (pcm != NULL) memcpy(pcm, out, frames * channels * sizeof(short));
			drwav_free(out, NULL);
		}
	}
	else if (length >= 3 && (memcmp(sfxdata, "ID3", 3) == 0 || (sfxdata[0] == 0xFF && (sfxdata[1] & 0xE0) == 0xE0)))
	{
		drmp3_config cfg;
		drmp3_uint64 count;
		drmp3_int16 *out = drmp3_open_memory_and_read_pcm_frames_s16(sfxdata, length, &cfg, &count, NULL);
		if (out != NULL)
		{
			channels = cfg.channels; rate = cfg.sampleRate; frames = (unsigned)count;
			pcm = new (std::nothrow) short[frames * channels];
			if (pcm != NULL) memcpy(pcm, out, frames * channels * sizeof(short));
			drmp3_free(out, NULL);
		}
	}

	if (pcm == NULL || frames == 0 || channels < 1 || rate <= 0)
	{
		delete[] pcm;
		return false;
	}
	if (channels > 2)
	{	// Keep the first two channels.
		short *st = new (std::nothrow) short[frames * 2];
		if (st == NULL) { delete[] pcm; return false; }
		for (unsigned i = 0; i < frames; ++i)
		{
			st[i * 2] = pcm[i * channels];
			st[i * 2 + 1] = pcm[i * channels + 1];
		}
		delete[] pcm;
		pcm = st;
		channels = 2;
	}
	pcm_out = pcm;
	frames_out = frames;
	channels_out = channels;
	rate_out = rate;
	return true;
}

//==========================================================================
//
// Background decoding
//
//==========================================================================

static SDL_mutex *DecodeLock;
static SDL_sem *DecodeSem;
static SDL_Thread *DecodeThread;
static TArray<VitaSample *> DecodeQueue;
static VitaSample *DecodeCurrent;

static int DecodeThreadFunc(void *)
{
	for (;;)
	{
		SDL_SemWait(DecodeSem);
		SDL_mutexP(DecodeLock);
		VitaSample *s = NULL;
		if (DecodeQueue.Size() > 0)
		{
			s = DecodeQueue[0];
			DecodeQueue.Delete(0);
		}
		DecodeCurrent = s;
		SDL_mutexV(DecodeLock);
		if (s == NULL)
			continue;

		short *pcm = NULL;
		unsigned frames = 0;
		int channels = 0, rate = 0;
		bool ok = false;
		// The decoders allocate with malloc/new; out of memory must not end
		// the game from this thread.
		try { ok = DecodeSound(s->Src, s->SrcLen, pcm, frames, channels, rate); }
		catch (...) { ok = false; pcm = NULL; }

		SDL_mutexP(DecodeLock);
		DecodeCurrent = NULL;
		delete[] s->Src;
		s->Src = NULL;
		if (s->Unloaded)
		{
			delete[] pcm;
			delete s;
		}
		else if (ok)
		{
			s->Data = pcm;
			s->Frames = frames;
			s->Channels = channels;
			s->Rate = rate;
			s->LoopStart = 0;
			s->LoopEnd = frames;
			s->Ready.store(1, std::memory_order_release);
		}
		else
		{
			// Undecodable: a short silent sample, so voices can finish.
			s->Data = new short[1];
			s->Data[0] = 0;
			s->Frames = 1;
			s->Channels = 1;
			s->Rate = 11025;
			s->LoopStart = 0;
			s->LoopEnd = 1;
			s->Ready.store(1, std::memory_order_release);
		}
		SDL_mutexV(DecodeLock);
	}
	return 0;
}

CVAR(Bool, snd_vita_asyncdecode, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

SoundHandle VitaSoundRenderer::LoadSound(BYTE *sfxdata, int length)
{
	SoundHandle retval = { NULL };
	if (length <= 0)
		return retval;

	bool compressed = length >= 4 && (memcmp(sfxdata, "OggS", 4) == 0 || memcmp(sfxdata, "fLaC", 4) == 0 ||
		memcmp(sfxdata, "ID3", 3) == 0 || (sfxdata[0] == 0xFF && (sfxdata[1] & 0xE0) == 0xE0));
	if (compressed && length >= 16384 && snd_vita_asyncdecode)
	{
		if (DecodeThread == NULL)
		{
			DecodeLock = SDL_CreateMutex();
			DecodeSem = SDL_CreateSemaphore(0);
			DecodeThread = SDL_CreateThread(DecodeThreadFunc, NULL);
		}
		if (DecodeThread != NULL)
		{
			VitaSample *s = MakeSample(NULL, 0, 1, 11025);
			s->Ready.store(0);
			s->Src = new BYTE[length];
			memcpy(s->Src, sfxdata, length);
			s->SrcLen = length;
			SDL_mutexP(DecodeLock);
			DecodeQueue.Push(s);
			SDL_mutexV(DecodeLock);
			SDL_SemPost(DecodeSem);
			retval.data = s;
			return retval;
		}
	}

	short *pcm = NULL;
	unsigned frames = 0;
	int channels = 0, rate = 0;
	if (!DecodeSound(sfxdata, length, pcm, frames, channels, rate))
	{
		DPrintf("Unsupported sound format\n");
		return retval;
	}
	retval.data = MakeSample(pcm, frames, channels, rate);
	return retval;
}

void VitaSoundRenderer::UnloadSound(SoundHandle sfx)
{
	VitaSample *s = (VitaSample *)sfx.data;
	if (s == NULL)
		return;

	// Stop anything still using this sample.
	TArray<FISoundChannel *> ended;
	SDL_LockAudio();
	for (int i = 0; i < MAX_VOICES; ++i)
	{
		if (Voices[i].Active && Voices[i].Sample == s)
		{
			if (Voices[i].Chan != NULL)
				ended.Push(Voices[i].Chan);
			Voices[i].Active = false;
		}
	}
	SDL_UnlockAudio();
	for (unsigned i = 0; i < ended.Size(); ++i)
	{
		ended[i]->SysChannel = NULL;
		S_ChannelEnded(ended[i]);
	}
	if (DecodeLock != NULL)
	{
		SDL_mutexP(DecodeLock);
		if (!SampleReady(s))
		{
			if (DecodeCurrent == s)
			{	// the decoder thread deletes it when it's done
				s->Unloaded = true;
				SDL_mutexV(DecodeLock);
				return;
			}
			for (unsigned i = 0; i < DecodeQueue.Size(); ++i)
			{
				if (DecodeQueue[i] == s) { DecodeQueue.Delete(i); break; }
			}
			delete[] s->Src;
			s->Src = NULL;
		}
		SDL_mutexV(DecodeLock);
	}
	delete[] s->Data;
	delete s;
}

unsigned int VitaSoundRenderer::GetMSLength(SoundHandle sfx)
{
	VitaSample *s = (VitaSample *)sfx.data;
	return s && SampleReady(s) ? unsigned(uint64_t(s->Frames) * 1000 / s->Rate) : 0;
}

unsigned int VitaSoundRenderer::GetSampleLength(SoundHandle sfx)
{
	VitaSample *s = (VitaSample *)sfx.data;
	return s && SampleReady(s) ? s->Frames : 0;
}

//==========================================================================
//
// Streams
//
//==========================================================================

SoundStream *VitaSoundRenderer::CreateStream(SoundStreamCallback callback, int buffbytes, int flags, int samplerate, void *userdata)
{
	if (callback == NULL || buffbytes <= 0 || samplerate <= 0)
		return NULL;
	return new VitaCallbackStream(this, callback, buffbytes, flags, samplerate, userdata);
}

SoundStream *VitaSoundRenderer::OpenStream(const char *filename_or_data, int flags, int offset, int length)
{
	BYTE *data = NULL;
	int len = 0;

	if (offset == -1)
	{	// In-memory data; the stream keeps its own copy.
		if (length <= 0)
			return NULL;
		data = new BYTE[length];
		memcpy(data, filename_or_data, length);
		len = length;
	}
	else
	{
		FILE *f = fopen(filename_or_data, "rb");
		if (f == NULL)
			return NULL;
		if (length <= 0)
		{
			fseek(f, 0, SEEK_END);
			length = int(ftell(f)) - offset;
		}
		if (length <= 0 || fseek(f, offset, SEEK_SET) != 0)
		{
			fclose(f);
			return NULL;
		}
		data = new BYTE[length];
		len = (int)fread(data, 1, length, f);
		fclose(f);
	}

	VitaFileStream *s = VitaFileStream::Open(this, data, len);
	if (s == NULL)
	{
		delete[] data;
		return NULL;
	}
	s->Looping = !!(flags & SoundStream::Loop);
	return s;
}

//==========================================================================
//
// Playing sounds
//
//==========================================================================

VitaVoice *VitaSoundRenderer::AllocVoice()
{
	for (int i = 0; i < MAX_VOICES; ++i)
	{
		if (!Voices[i].Active)
			return &Voices[i];
	}
	return NULL;
}

VitaVoice *VitaSoundRenderer::FindVoice(FISoundChannel *chan)
{
	if (chan == NULL || chan->SysChannel == NULL)
		return NULL;
	VitaVoice *v = (VitaVoice *)chan->SysChannel;
	if (v < Voices || v >= Voices + MAX_VOICES || !v->Active || v->Chan != chan)
		return NULL;
	return v;
}

void VitaSoundRenderer::SetStereoGains(VitaVoice *v)
{
	float vol = v->Volume;
	if (!v->Is3D)
	{
		v->TargetL = v->TargetR = vol;
		return;
	}
	// Stereo samples get summed by 3D panning in FMOD; halve them likewise.
	if (v->Sample->Channels > 1)
		vol *= 0.5f;
	vol *= v->Attenuation;

	float pan = 0, panlevel = 1;
	if (ListenerValid)
	{
		FVector3 d = v->Position - Listener.position;
		float dist = d.Length();
		if (v->Area)
		{	// Within 32 units, blend from no panning to full 3D panning.
			panlevel = dist >= 32 ? 1 : dist / 32;
		}
		if (dist > 0.0001f)
		{
			// FMOD space: x east, y up, z north; forward (cos a, 0, sin a);
			// right = (sin a, 0, -cos a).
			float rx = sinf(Listener.angle), rz = -cosf(Listener.angle);
			pan = (d.X * rx + d.Z * rz) / dist;
		}
	}
	pan *= panlevel;
	// Equal-power panning: a centred sound plays at sqrt(1/2) on each side.
	v->TargetL = vol * sqrtf((1 - pan) * 0.5f);
	v->TargetR = vol * sqrtf((1 + pan) * 0.5f);
}

void VitaSoundRenderer::Spatialize(VitaVoice *v, const SoundListener *listener, const FRolloffInfo *rolloff, float distscale)
{
	if (listener != NULL && listener->valid)
	{
		Listener = *listener;
		ListenerValid = true;
	}
	float dist = ListenerValid ? (v->Position - Listener.position).Length() : 0;
	v->Attenuation = rolloff != NULL ? S_GetRolloff(const_cast<FRolloffInfo *>(rolloff), dist * distscale, true) : 1;
	SetStereoGains(v);
}

bool VitaSoundRenderer::HandleChannelDelay(VitaVoice *v, FISoundChannel *reuse_chan, int flags, float freq)
{
	if (reuse_chan == NULL)
		return true;

	// The sound is being restarted: seek to where it would be now.
	uint64_t now = DSPClock.load();
	VitaSample *s = v->Sample;
	if (!SampleReady(s))
		return true;
	uint64_t seek = 0;
	if (flags & SNDF_ABSTIME)
	{
		seek = reuse_chan->StartTime.Lo;
		reuse_chan->StartTime.AsOne = QWORD(now - uint64_t(seek * OUT_RATE / freq));
	}
	else if (reuse_chan->StartTime.AsOne != 0 && now > reuse_chan->StartTime.AsOne)
	{
		uint64_t diff = now - reuse_chan->StartTime.AsOne;
		seek = uint64_t(diff * freq / OUT_RATE);
	}
	if (seek == 0)
		return true;
	if (flags & SNDF_LOOP)
	{
		if (seek >= (uint64_t)s->LoopEnd && s->LoopEnd > s->LoopStart)
			seek = s->LoopStart + (seek - s->LoopStart) % (s->LoopEnd - s->LoopStart);
	}
	else if (seek >= s->Frames)
	{
		return false;	// would already have finished
	}
	v->Pos = seek << 32;
	return true;
}

FISoundChannel *VitaSoundRenderer::Start(VitaSample *s, float vol, int pitch, int flags, FISoundChannel *reuse_chan,
	bool is3d, const FVector3 *pos, bool area, SoundListener *listener, FRolloffInfo *rolloff, float distscale)
{
	if (s == NULL)
		return NULL;

	float freq = PitchFreq(float(s->Rate), pitch);
	if (freq <= 0)
		freq = float(s->Rate);

	VitaVoice nv;
	memset(&nv, 0, sizeof(nv));
	nv.Sample = s;
	nv.Pitch = pitch;
	nv.Step = SampleReady(s) ? uint64_t(double(freq) / OUT_RATE * 4294967296.0) : 0;
	nv.Loop = !!(flags & SNDF_LOOP);
	nv.Pausable = !(flags & SNDF_NOPAUSE);
	nv.Volume = vol;
	nv.Attenuation = 1;
	nv.Is3D = is3d;
	nv.Area = area;
	if (pos != NULL)
		nv.Position = *pos;
	if (is3d)
	{
		// A sound at the listener's own position is head-relative (2D).
		if (!area && listener != NULL && listener->valid && listener->position == *pos)
			nv.Is3D = false;
		Spatialize(&nv, listener, rolloff, distscale);
	}
	else
	{
		SetStereoGains(&nv);
	}
	nv.CurL = nv.TargetL;
	nv.CurR = nv.TargetR;

	if (!HandleChannelDelay(&nv, reuse_chan, flags & (SNDF_ABSTIME | SNDF_LOOP), freq))
		return NULL;

	SDL_LockAudio();
	VitaVoice *v = AllocVoice();
	if (v == NULL)
	{
		SDL_UnlockAudio();
		return NULL;
	}
	*v = nv;
	v->Active = true;
	SDL_UnlockAudio();

	FISoundChannel *schan;
	if (reuse_chan != NULL)
	{
		schan = reuse_chan;
		schan->SysChannel = v;
	}
	else
	{
		schan = S_GetChannel(v);
		schan->StartTime.AsOne = DSPClock.load();
	}
	if (is3d && rolloff != NULL)
	{
		schan->Rolloff = *rolloff;
		schan->DistanceScale = distscale;
	}
	SDL_LockAudio();
	v->Chan = schan;
	SDL_UnlockAudio();
	return schan;
}

FISoundChannel *VitaSoundRenderer::StartSound(SoundHandle sfx, float vol, int pitch, int chanflags, FISoundChannel *reuse_chan)
{
	return Start((VitaSample *)sfx.data, vol, pitch, chanflags, reuse_chan, false, NULL, false, NULL, NULL, 1);
}

FISoundChannel *VitaSoundRenderer::StartSound3D(SoundHandle sfx, SoundListener *listener, float vol,
	FRolloffInfo *rolloff, float distscale, int pitch, int priority, const FVector3 &pos, const FVector3 &vel,
	int channum, int chanflags, FISoundChannel *reuse_chan)
{
	return Start((VitaSample *)sfx.data, vol, pitch, chanflags, reuse_chan, true, &pos,
		!!(chanflags & SNDF_AREA), listener, rolloff, distscale);
}

void VitaSoundRenderer::StopChannel(FISoundChannel *chan)
{
	SDL_LockAudio();
	VitaVoice *v = FindVoice(chan);
	if (v != NULL)
		v->Stopped = true;
	SDL_UnlockAudio();
	if (v != NULL)
	{
		// FMOD reports a stopped channel as ended right away; so do we. The
		// voice stays allocated meanwhile so GetPosition still answers.
		S_ChannelEnded(chan);
		SDL_LockAudio();
		if (v->Active && v->Chan == chan && v->Stopped)
			v->Active = false;
		SDL_UnlockAudio();
	}
}

void VitaSoundRenderer::ChannelVolume(FISoundChannel *chan, float volume)
{
	SDL_LockAudio();
	VitaVoice *v = FindVoice(chan);
	if (v != NULL)
	{
		v->Volume = volume;
		SetStereoGains(v);
	}
	SDL_UnlockAudio();
}

void VitaSoundRenderer::MarkStartTime(FISoundChannel *chan)
{
	chan->StartTime.AsOne = DSPClock.load();
}

unsigned int VitaSoundRenderer::GetPosition(FISoundChannel *chan)
{
	unsigned int pos = 0;
	SDL_LockAudio();
	VitaVoice *v = FindVoice(chan);
	if (v != NULL)
	{
		pos = unsigned(v->Pos >> 32);
		if (v->Ended)
			pos = v->Sample->Frames;
	}
	SDL_UnlockAudio();
	return pos;
}

float VitaSoundRenderer::GetAudibility(FISoundChannel *chan)
{
	float aud = 0;
	SDL_LockAudio();
	VitaVoice *v = FindVoice(chan);
	if (v != NULL)
		aud = v->Volume * (v->Is3D ? v->Attenuation : 1);
	SDL_UnlockAudio();
	return aud;
}

void VitaSoundRenderer::SetSfxPaused(bool paused, int slot)
{
	if (paused)
		SfxPausedSlots |= 1 << slot;
	else
		SfxPausedSlots &= ~(1 << slot);
}

void VitaSoundRenderer::SetInactive(EInactiveState inactive)
{
	Inactive = (inactive == INACTIVE_Complete);
	Muted = (inactive == INACTIVE_Mute);
}

void VitaSoundRenderer::UpdateSoundParams3D(SoundListener *listener, FISoundChannel *chan, bool areasound, const FVector3 &pos, const FVector3 &vel)
{
	SDL_LockAudio();
	VitaVoice *v = FindVoice(chan);
	if (v != NULL && v->Is3D)
	{
		v->Position = pos;
		v->Area = areasound;
		Spatialize(v, listener, &chan->Rolloff, chan->DistanceScale);
	}
	SDL_UnlockAudio();
}

void VitaSoundRenderer::UpdateListener(SoundListener *listener)
{
	if (listener == NULL || !listener->valid)
		return;
	SDL_LockAudio();
	Listener = *listener;
	ListenerValid = true;
	SDL_UnlockAudio();
}

void VitaSoundRenderer::UpdateSounds()
{
	if (DumpBuf != NULL && !DumpWritten && DumpPos.load() >= DumpFrames)
	{
		DumpWritten = true;
		WriteDump();
	}

	// Report voices that reached their end. S_ChannelEnded may start or stop
	// other sounds, so call it without the audio lock held.
	FISoundChannel *ended[MAX_VOICES];
	int numended = 0;
	SDL_LockAudio();
	for (int i = 0; i < MAX_VOICES; ++i)
	{
		VitaVoice *v = &Voices[i];
		if (v->Active && v->Ended && !v->Stopped)
		{
			if (v->Chan != NULL)
				ended[numended++] = v->Chan;
			else
				v->Active = false;
		}
	}
	SDL_UnlockAudio();

	for (int i = 0; i < numended; ++i)
	{
		// GetPosition still works here (the voice stays active and reports its
		// full length), so the game sees the sound as finished, not evicted.
		FISoundChannel *chan = ended[i];
		S_ChannelEnded(chan);
		SDL_LockAudio();
		for (int j = 0; j < MAX_VOICES; ++j)
		{
			if (Voices[j].Active && Voices[j].Chan == chan && Voices[j].Ended)
				Voices[j].Active = false;
		}
		SDL_UnlockAudio();
	}

	// Refresh 3D panning for the latest listener orientation.
	SDL_LockAudio();
	for (int i = 0; i < MAX_VOICES; ++i)
	{
		if (Voices[i].Active && Voices[i].Is3D)
			SetStereoGains(&Voices[i]);
	}
	SDL_UnlockAudio();
}

//==========================================================================
//
// Status
//
//==========================================================================

void VitaSoundRenderer::PrintStatus()
{
	Printf("Output: SDL audio, %d Hz stereo, software mixer\n", OUT_RATE);
	Printf("Voices: %d max\n", MAX_VOICES);
}

void VitaSoundRenderer::PrintDriversList()
{
	Printf("1. PS Vita audio (SDL)\n");
}

FString VitaSoundRenderer::GatherStats()
{
	int active = 0;
	for (int i = 0; i < MAX_VOICES; ++i)
		if (Voices[i].Active && !Voices[i].Ended) active++;
	FString out;
	out.Format("%d voices playing, %u streams", active, Streams.Size());
	return out;
}

//==========================================================================
//
// Factory
//
//==========================================================================

SoundRenderer *Vita_CreateSoundRenderer()
{
	return new VitaSoundRenderer;
}

#endif
