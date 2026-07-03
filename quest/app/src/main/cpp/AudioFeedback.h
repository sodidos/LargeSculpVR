#pragma once

// Tiny synthesized UI sound feedback (AAudio, no assets): short sine blips
// with an attack/decay envelope, mixed from a small voice pool. Used to
// confirm menu clicks, tool changes, stroke starts, grabs, undo/redo...

#include <aaudio/AAudio.h>

#include <array>
#include <cmath>
#include <mutex>

namespace large::audio {

class Feedback {
 public:
  bool initialize() {
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || builder == nullptr) {
      return false;
    }
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, &Feedback::dataCallback, this);
    const bool opened = AAudioStreamBuilder_openStream(builder, &stream_) == AAUDIO_OK && stream_ != nullptr;
    AAudioStreamBuilder_delete(builder);
    if (!opened) {
      stream_ = nullptr;
      return false;
    }

    sampleRate_ = AAudioStream_getSampleRate(stream_);
    if (sampleRate_ <= 0) {
      sampleRate_ = 48000;
    }
    if (AAudioStream_requestStart(stream_) != AAUDIO_OK) {
      AAudioStream_close(stream_);
      stream_ = nullptr;
      return false;
    }
    return true;
  }

  void shutdown() {
    if (stream_ != nullptr) {
      AAudioStream_requestStop(stream_);
      AAudioStream_close(stream_);
      stream_ = nullptr;
    }
  }

  // sustain keeps the tone at constant level until the end (short release),
  // instead of the default percussive decay -- used for the arming buzz.
  void play(float frequency, float durationSeconds, float volume, bool sustain = false) {
    if (stream_ == nullptr || durationSeconds <= 0.0f) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (Voice& voice : voices_) {
      if (!voice.active) {
        voice.phase = 0.0f;
        voice.frequency = frequency;
        voice.total = durationSeconds;
        voice.remaining = durationSeconds;
        voice.volume = volume;
        voice.sustain = sustain;
        voice.active = true;
        return;
      }
    }
  }

 private:
  struct Voice {
    float phase = 0.0f;
    float frequency = 440.0f;
    float total = 0.0f;
    float remaining = 0.0f;
    float volume = 0.0f;
    bool sustain = false;
    bool active = false;
  };

  static aaudio_data_callback_result_t dataCallback(AAudioStream*, void* userData, void* audioData,
                                                    int32_t numFrames) {
    auto* self = static_cast<Feedback*>(userData);
    float* out = static_cast<float*>(audioData);
    const float dt = 1.0f / static_cast<float>(self->sampleRate_);

    std::lock_guard<std::mutex> lock(self->mutex_);
    for (int32_t i = 0; i < numFrames; ++i) {
      float sample = 0.0f;
      for (Voice& voice : self->voices_) {
        if (!voice.active) {
          continue;
        }
        const float elapsed = voice.total - voice.remaining;
        const float attack = elapsed < 0.005f ? elapsed / 0.005f : 1.0f;
        float envelope;
        if (voice.sustain) {
          const float release = voice.remaining < 0.03f ? voice.remaining / 0.03f : 1.0f;
          envelope = attack * release;
        } else {
          const float decay = voice.remaining / voice.total;
          envelope = attack * decay * decay;
        }
        sample += std::sin(voice.phase * 6.2831853f) * voice.volume * envelope;
        voice.phase += voice.frequency * dt;
        if (voice.phase > 1.0f) {
          voice.phase -= 1.0f;
        }
        voice.remaining -= dt;
        if (voice.remaining <= 0.0f) {
          voice.active = false;
        }
      }
      out[i] = sample < -1.0f ? -1.0f : (sample > 1.0f ? 1.0f : sample);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  AAudioStream* stream_ = nullptr;
  int32_t sampleRate_ = 48000;
  std::array<Voice, 8> voices_{};
  std::mutex mutex_;
};

}  // namespace large::audio
