#include "bbaudio_soloud.h"

#include "soloud.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"
#include "soloud_filter.h"

// Not including soloud_fft.h: bbruntime is compiled with /Gz (stdcall), soloud.lib with the default cdecl.
namespace SoLoud { namespace FFT { void __cdecl fft1024(float *aBuffer); }}

struct bbBank;
int bbBankSize(bbBank *b);
void bbPokeFloat(bbBank *b, int offset, float value);

SoLoud::Soloud *soloud;

namespace {
	std::set<Sound *> sounds;

	inline void debugSound(Sound *sound) {
		if (::debug && sounds.find(sound) == sounds.end()) RTEX("Sound does not exist"); // NOLINT
	}

	inline void debugChannel(uint32_t channel) {
		if (::debug && !soloud) RTEX("Soloud has not been initialized"); // NOLINT
	}

	const int FFT_SIZE = 256;

	struct ChannelMonitorInstance : SoLoud::FilterInstance {
		double length;
		float wave[FFT_SIZE]{};

		explicit ChannelMonitorInstance(double length) : length(length) {}

		void filter(float *aBuffer, unsigned int aSamples, unsigned int aBufferSize, unsigned int aChannels,
					float aSamplerate, SoLoud::time aTime) override {
			unsigned n = aSamples < FFT_SIZE ? aSamples : FFT_SIZE;
			memmove(wave, wave + n, (FFT_SIZE - n) * sizeof(float));
			const float *src = aBuffer + aSamples - n;
			for (unsigned i = 0; i < n; ++i) {
				float sample = 0;
				for (unsigned c = 0; c < aChannels; ++c) sample += src[i + c * aBufferSize];
				wave[FFT_SIZE - n + i] = sample;
			}
		}
	};

	struct ChannelMonitor : SoLoud::Filter {
		double length = 0;

		SoLoud::FilterInstance *createInstance() override {
			return new ChannelMonitorInstance(length);
		}
	};

	// Copies the monitor state of a channel. Returns false if the channel is not (or no longer) playing.
	bool getMonitor(uint32_t channel, double *length, float *wave) {
		if (!soloud || !channel) return false;
		soloud->lockAudioMutex_internal();
		int v = soloud->getVoiceFromHandle_internal(channel);
		auto monitor = v < 0 ? nullptr : dynamic_cast<ChannelMonitorInstance *>(soloud->mVoice[v]->mFilter[0]);
		if (monitor) {
			if (length) *length = monitor->length;
			if (wave) memcpy(wave, monitor->wave, sizeof(monitor->wave));
		}
		soloud->unlockAudioMutex_internal();
		return monitor != nullptr;
	}

	const int MAX_MUSIC = 4;
	ChannelMonitor musicMonitors[MAX_MUSIC];
	SoLoud::WavStream musicStreams[MAX_MUSIC];
	uint32_t musicChannels[MAX_MUSIC];

	float rolloffFactor = 1.0f;
	float dopplerFactor = 1.0f;
	float distanceFactor = 1.0f;
}

struct Sound {
	ChannelMonitor monitor;
	SoLoud::Wav wav;

	float volume{1};
	float pan{0};
	int pitch{0};

	explicit Sound() {
		if (debug) sounds.insert(this);
	}

	~Sound() {
		if (debug) sounds.erase(this);
	}
};

Sound *bbLoadSound(BBStr *path) {
	if (!soloud) {
		delete path;
		return nullptr;
	}
	auto sound = new Sound();
	auto r = sound->wav.load(path->c_str());
	delete path;
	if (r == SoLoud::SO_NO_ERROR) {
		sound->monitor.length = sound->wav.getLength();
		sound->wav.setFilter(0, &sound->monitor);
		return sound;
	}
	delete sound;
	return nullptr;
}

void bbFreeSound(Sound *sound) {
	if (!sound) return;
	debugSound(sound);
	delete sound;
}

void bbLoopSound(Sound *sound) {
	if (!sound) return;
	debugSound(sound);
	sound->wav.setLooping(true);
}

void bbSoundVolume(Sound *sound, float volume) {
	if (!sound) return;
	debugSound(sound);
	sound->volume = volume;
}

void bbSoundPan(Sound *sound, float pan) {
	if (!sound) return;
	debugSound(sound);
	sound->pan = pan;
}

void bbSoundPitch(Sound *sound, int pitch) {
	if (!sound) return;
	debugSound(sound);
	sound->pitch = pitch;
}

uint32_t bbPlaySound(Sound *sound) {
	if (!sound) return 0;
	debugSound(sound);
	uint32_t chan;
	if (sound->pitch) {
		chan = soloud->play(sound->wav, sound->volume, sound->pan, true, 0);
		soloud->setSamplerate(chan, (float) sound->pitch);
		soloud->setPause(chan, false);
		return chan;
	}
	return soloud->play(sound->wav, sound->volume, sound->pan, false, 0);
}

uint32_t bbPlay3dSound(Sound *sound, float x, float y, float z, float vx, float vy, float vz) {
	if (!sound) return 0;
	debugSound(sound);
	sound->wav.set3dAttenuation(SoLoud::AudioSource::INVERSE_DISTANCE, rolloffFactor);
	sound->wav.set3dDopplerFactor(dopplerFactor);
	if (sound->pitch) {
		auto chan = soloud->play3d(sound->wav, x, y, z, vx, vy, vz, sound->volume, true, 0);
		soloud->setSamplerate(chan, (float) sound->pitch);
		soloud->setPause(chan, false);
		return chan;
	}
	return soloud->play3d(sound->wav, x, y, z, vx, vy, vz, sound->volume, false, 0);
}

uint32_t bbPlayMusic(BBStr *path, int loop, float volume) {
	if (!soloud) {
		delete path;
		return 0;
	}
	int i = 0;
	while (i < MAX_MUSIC && soloud->isValidVoiceHandle(musicChannels[i])) ++i;
	if (i == MAX_MUSIC) {
		delete path;
		if (::debug) RTEX("Too many music channels playing"); // NOLINT
		return 0;
	}
	auto &stream = musicStreams[i];
	auto r = stream.load(path->c_str());
	delete path;
	if (r != SoLoud::SO_NO_ERROR) return 0;
	stream.setLooping(loop != 0);
	musicMonitors[i].length = stream.getLength();
	stream.setFilter(0, &musicMonitors[i]);
	return musicChannels[i] = soloud->play(stream, volume);
}

void bbStopChannel(uint32_t channel) {
	if (!channel) return;
	debugChannel(channel);
	soloud->stop(channel);
}

void bbPauseChannel(uint32_t channel) {
	if (!channel) return;
	debugChannel(channel);
	soloud->setPause(channel, true);
}

void bbResumeChannel(uint32_t channel) {
	if (!channel) return;
	debugChannel(channel);
	soloud->setPause(channel, false);
}

void bbChannelVolume(uint32_t channel, float volume) {
	if (!channel) return;
	debugChannel(channel);
	soloud->setVolume(channel, volume);
}

void bbChannelPan(uint32_t channel, float pan) {
	if (!channel) return;
	debugChannel(channel);
	soloud->setPan(channel, pan);
}

void bbChannelPitch(uint32_t channel, int pitch) {
	if (!channel) return;
	debugChannel(channel);
	soloud->setSamplerate(channel, (float) pitch);
}

int bbChannelPlaying(uint32_t channel) {
	if (!channel) return 0;
	debugChannel(channel);
	return soloud->isValidVoiceHandle(channel);
}

float bbChannelLength(uint32_t channel) {
	double length;
	return getMonitor(channel, &length, nullptr) ? (float) length : 0;
}

float bbChannelPosition(uint32_t channel) {
	double length;
	if (!getMonitor(channel, &length, nullptr)) return 0;
	auto pos = soloud->getStreamPosition(channel);
	return (float) (length > 0 && pos > length ? length : pos);
}

int bbSeekChannel(uint32_t channel, float seconds) {
	double length;
	if (!getMonitor(channel, &length, nullptr)) return 0;
	double pos = seconds < 0 ? 0 : seconds;
	if (length > 0 && pos >= length) pos = length - 0.001;
	return soloud->seek(channel, pos) == SoLoud::SO_NO_ERROR;
}

int bbChannelFFT(uint32_t channel, bbBank *bank) {
	if (!soloud || !bank) return 0;
	int count = bbBankSize(bank) / 4;
	if (count > FFT_SIZE) count = FFT_SIZE;

	float fft[FFT_SIZE]{};
	bool ok = true;
	if (!channel) {
		// Channel 0: spectrum of the final mix output (after channel volumes).
		static bool visualizationEnabled;
		if (!visualizationEnabled) {
			soloud->setVisualizationEnable(true);
			visualizationEnabled = true;
		}
		memcpy(fft, soloud->calcFFT(), sizeof(fft));
	} else {
		// Same math as Soloud::calcFFT(), but on the channel's own source samples (before volume).
		float wave[FFT_SIZE];
		ok = getMonitor(channel, nullptr, wave);
		if (ok) {
			float temp[1024]{};
			for (int i = 0; i < FFT_SIZE; ++i) temp[i * 2] = wave[i];
			SoLoud::FFT::fft1024(temp);
			for (int i = 0; i < FFT_SIZE; ++i) fft[i] = sqrtf(temp[i * 2] * temp[i * 2] + temp[i * 2 + 1] * temp[i * 2 + 1]);
		}
	}
	for (int i = 0; i < count; ++i) bbPokeFloat(bank, i * 4, fft[i]);
	return ok ? count : 0;
}

void bbSet3dChannel(uint32_t channel, float x, float y, float z, float vx, float vy, float vz) {
	if (!channel) return;
	debugChannel(channel);
	auto sc = distanceFactor;
	soloud->set3dSourceParameters(channel, x * sc, y * sc, z * sc, vx * sc, vy * sc, vz * sc);
}

void bbSet3dListenerConfig(float roll, float dopp, float dist) {
	if (!soloud) return;
	rolloffFactor = roll;
	dopplerFactor = dopp;
	distanceFactor = dist;
}

void bbSet3dListener(float x, float y, float z, float kx, float ky, float kz, float jx, float jy, float jz, float vx,
					 float vy, float vz) {
	if (!soloud) return;
	auto sc = distanceFactor;
	soloud->set3dListenerParameters(x * sc, y * sc, z * sc, kx, ky, kz, jy, jy, jz, vx * sc, vy * sc, vz * sc);
	soloud->update3dAudio();
}

bool audio_create() {
	soloud = new SoLoud::Soloud();
	if (soloud->init(SoLoud::Soloud::LEFT_HANDED_3D) != SoLoud::SO_NO_ERROR) {
		delete soloud;
		soloud = nullptr;
	}
	return true;
}

bool audio_destroy() {
	if (!soloud) return true;
	soloud->deinit();
	for (auto &stream : musicStreams) stream.mSoloud = nullptr;
	delete soloud;
	soloud = nullptr;
	return true;
}

void audio_link(void(*rtSym)(const char *, void *)) {
	rtSym("%LoadSound$filename", bbLoadSound);
	rtSym("%Load3DSound$filename", bbLoadSound);
	rtSym("FreeSound%sound", bbFreeSound);
	rtSym("LoopSound%sound", bbLoopSound);
	rtSym("SoundPitch%sound%pitch", bbSoundPitch);
	rtSym("SoundVolume%sound#volume", bbSoundVolume);
	rtSym("SoundPan%sound#pan", bbSoundPan);
	rtSym("%PlaySound%sound", bbPlaySound);
	rtSym("%PlayMusic$midifile%loop=0#volume=1", bbPlayMusic);
	rtSym("StopChannel%channel", bbStopChannel);
	rtSym("PauseChannel%channel", bbPauseChannel);
	rtSym("ResumeChannel%channel", bbResumeChannel);
	rtSym("ChannelPitch%channel%pitch", bbChannelPitch);
	rtSym("ChannelVolume%channel#volume", bbChannelVolume);
	rtSym("ChannelPan%channel#pan", bbChannelPan);
	rtSym("%ChannelPlaying%channel", bbChannelPlaying);
	rtSym("#ChannelPosition%channel", bbChannelPosition);
	rtSym("#ChannelLength%channel", bbChannelLength);
	rtSym("%SeekChannel%channel#seconds", bbSeekChannel);
	rtSym("%ChannelFFT%channel%bank", bbChannelFFT);
}
