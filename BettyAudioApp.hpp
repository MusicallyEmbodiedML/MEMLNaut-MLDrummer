#ifndef __BETTY_AUDIO_APP_HPP__
#define __BETTY_AUDIO_APP_HPP__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "src/memllib/audio/AudioAppBase.hpp"
#include "src/memllib/audio/AudioDriver.hpp"
#include "src/memllib/synth/maximilian.h"
#include "src/daisysp/Effects/pitchshifter.h"
#include "src/daisysp/Effects/autowah.h"
#include "src/memllib/synth/OnePoleSmoother.hpp"
#include "src/memllib/utils/sharedMem.hpp"
#include "src/memllib/PicoDefs.hpp"


extern volatile float pitch_bias;


__force_inline float MapToSeries(float value, const std::vector<float>& series) {
    if (series.empty()) return 0.0f;
    if (series.size() == 1) return series[0];

    // Clamp value to [0, 1]
    value = fmaxf(0.0f, fminf(1.0f, value));

    // Calculate which segment we're in
    float segment_size = 1.0f / static_cast<float>(series.size());
    size_t index = static_cast<size_t>(value / segment_size);

    // Handle edge case where value == 1.0
    if (index >= series.size()) {
        index = series.size() - 1;
    }

    return series[index];
}


class HysteresisSmoother {
public:
    HysteresisSmoother(float sample_rate, float smoothing_time_ms, float hysteresis_time_ms) :
        sample_rate_(sample_rate),
        hysteresis_time_ms_(hysteresis_time_ms),
        smoother_(smoothing_time_ms, sample_rate),
        current_flag_(false),
        pending_flag_(false),
        counter_(0),
        hysteresis_samples_(static_cast<size_t>(hysteresis_time_ms * 0.001f * sample_rate)) {
    }

    bool Process(float x) {
        // Smooth the input
        float smoothed;
        smoother_.Process(&x, &smoothed);

        // Discretize to bool (>=0.5f = true)
        bool target_flag = smoothed >= 0.5f;

        // Apply hysteresis
        if (target_flag != current_flag_) {
            if (target_flag == pending_flag_) {
                // Same target as before, increment counter
                counter_++;
                if (counter_ >= hysteresis_samples_) {
                    // Held long enough, flip the flag
                    current_flag_ = target_flag;
                    counter_ = 0;
                }
            } else {
                // Different target, reset counter
                pending_flag_ = target_flag;
                counter_ = 1;
            }
        } else {
            // Already at target, reset counter
            counter_ = 0;
            pending_flag_ = current_flag_;
        }

        return current_flag_;
    }

private:
    float sample_rate_;
    float hysteresis_time_ms_;
    OnePoleSmoother<1> smoother_;
    bool current_flag_;
    bool pending_flag_;
    size_t counter_;
    size_t hysteresis_samples_;
};


__force_inline float BiasedScale(float value, float bias) {
    // Clamp inputs to [0, 1]
    value = fmaxf(0.0f, fminf(1.0f, value));
    bias = fmaxf(0.0f, fminf(1.0f, bias));

    // Calculate the output range based on bias
    // bias=0: [0, 0.5], bias=0.5: [0, 1], bias=1: [0.5, 1]
    float range_min, range_max;

    if (bias <= 0.5f) {
        // For bias 0 to 0.5: range from [0, bias+0.5]
        range_min = 0.0f;
        range_max = bias + 0.5f;
    } else {
        // For bias 0.5 to 1: range from [bias-0.5, 1]
        range_min = bias - 0.5f;
        range_max = 1.0f;
    }

    return range_min + value * (range_max - range_min);
}


class BettyAudioApp : public AudioAppBase
{
public:
    struct ParamDef {
        float which_shift;
        float shift;
        float shift2;
        float dlyfeedback;
        float wahlevel;
        float wahdrywet;
        float wahwah;
    };
    static constexpr size_t kN_Params = sizeof(ParamDef) / sizeof(float);

    BettyAudioApp() : AudioAppBase(), smoother(150.f, kSampleRate),
        neuralNetOutputs(kN_Params, 0),
        pitchbias_smoother(150.f, kSampleRate)
     {}

    AudioDriver::codec_config_t GetDriverConfig() const override {
        return {
            .mic_input = false,
            .line_level = 7,
            .mic_gain_dB = 0,
            .output_volume = 0.95f
        };
    }


    __force_inline stereosample_t ProcessInline(const stereosample_t x)
    {
        float raw_pitch_bias = READ_VOLATILE(pitch_bias);
        raw_pitch_bias = pitchbias_smoother.Process(raw_pitch_bias);
        // Parameter processing
        union {
            ParamDef p;
            float v[kN_Params];
        } param_u;
        smoother.Process(neuralNetOutputs.data(), param_u.v);
        // Pitch shift transposition between -12 and +12 semitones
        float mix_pitch1, mix_pitch2;
        // bias the smoothing switch parameter
        float biased_switch = BiasedScale(param_u.p.which_shift, raw_pitch_bias);
        bool smoothed_switch = smoother_pitchshift_switch.Process(biased_switch);
        if (smoothed_switch) {
            //float pitchshift_transpose = (param_u.p.shift * 24.f) - 12.f; // Scale to -12 to +12 semitones
            float pitchshift_transpose = MapToSeries(param_u.p.shift, {2.f, 5., 7.f, 10.f, 12.f});
            pitchshifter_.SetTransposition(pitchshift_transpose);
            mix_pitch1 = 1.f;
            mix_pitch2 = 0;
        } else {
            float third_down = (param_u.p.shift > 0.5f) ? -9.f : -8.f;
            float fifth_down = (param_u.p.shift2 > 0.5f) ? -7.f : -7.f;
            pitchshifter_.SetTransposition(third_down);
            pitchshifter2_.SetTransposition(fifth_down);
            mix_pitch1 = 0.5f;
            mix_pitch2 = 0.5f;
        }
        // Wah
        wah_.SetLevel(param_u.p.wahlevel);
        wah_.SetDryWet(param_u.p.wahdrywet * 100.f);
        wah_.SetWah(param_u.p.wahwah);


        // Signal processing
        float y = x.L + x.R;
        // Delay by 4/4 120 bpm
        float dly = dl_.play(y, kDLineTap, 0);
        dly = wah_.Process(dly);
        // Pitch shift
        float shift1 = pitchshifter_.Process(dly) * mix_pitch1;
        float shift2 = pitchshifter2_.Process(dly) * mix_pitch2;
        dly = dcb.play(shift1 + shift2, 0.995f);
        y += dly * 0.5f; // Mix delayed signal
        // Soft clip
        y = tanhf(y);

        return stereosample_t { y, y };
    }

    void Setup(float sample_rate, std::shared_ptr<InterfaceBase> interface) override
    {
        AudioAppBase::Setup(sample_rate, interface);
        maxiSettings::sampleRate = sample_rate;
        pitchshifter_.Init(sample_rate);
        pitchshifter2_.Init(sample_rate);
        wah_.Init(sample_rate);
    }

    void ProcessParams(const std::vector<float>& params) override
    {
        neuralNetOutputs = params;
    }

protected:

    std::vector<float> neuralNetOutputs;

    maxiDCBlocker dcb;
    daisysp::PitchShifter pitchshifter_;
    daisysp::PitchShifter pitchshifter2_;
    daisysp::Autowah wah_;

    static constexpr float kDelaySeconds = 1.f;
    static constexpr size_t kDLineLength = static_cast<size_t>(kDelaySeconds * kSampleRate);
    static constexpr size_t kDLineTap = kDLineLength - 2;
    maxiDelayline< kDLineLength > dl_;

    OnePoleSmoother<kN_Params> smoother;
    HysteresisSmoother smoother_pitchshift_switch{ kSampleRate, 400.f, 150.f };
    OnePoleSmoother<1> pitchbias_smoother;
};


#endif  // BETTY_AUDIO_APP_HPP__
