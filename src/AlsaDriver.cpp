/*
 * MIT License
 *
 * Copyright (c) 2024 Robin E. R. Davies
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pch.h"
#include "util.hpp"
#include <bit>
#include <memory>
#include "ss.hpp"
#include "AlsaDriver.hpp"
#include "JackServerSettings.hpp"
#include <thread>
#include "RtInversionGuard.hpp"
#include "PiPedalException.hpp"
#include "DummyAudioDriver.hpp"

#include "CpuUse.hpp"

#include <alsa/asoundlib.h>

#include "Lv2Log.hpp"
#include <limits>
#include "ss.hpp"

#undef ALSADRIVER_CONFIG_DBG

#ifdef ALSADRIVER_CONFIG_DBG
#include <stdio.h>
#endif

using namespace pipedal;

namespace pipedal
{

    struct AudioFormat
    {
        char name[40];
        snd_pcm_format_t pcm_format;
    };

    bool SetPreferredAlsaFormat(
        const char *streamType,
        snd_pcm_t *handle,
        snd_pcm_hw_params_t *hwParams,
        AudioFormat *formats,
        size_t nItems)
    {
        snd_pcm_hw_params_t* test_params;
        snd_pcm_hw_params_alloca(&test_params);

        for (size_t i = 0; i < nItems; ++i)
        {
            snd_pcm_hw_params_copy(test_params, hwParams);

            int err = snd_pcm_hw_params_set_format(handle, test_params, formats[i].pcm_format);
            if (err == 0)
            {
                int err = snd_pcm_hw_params_set_format(handle, hwParams, formats[i].pcm_format);
                if (err == 0)
                {
                    return true;
                }
            }
        }
        return false;
    }

    static AudioFormat leFormats[]{
        {"32-bit float little-endian", SND_PCM_FORMAT_FLOAT_LE},
        {"32-bit integer little-endian", SND_PCM_FORMAT_S32_LE},
        {"24-bit little-endian", SND_PCM_FORMAT_S24_LE},
        {"24-bit little-endian in 3bytes format", SND_PCM_FORMAT_S24_3LE},
        {"16-bit little-endian", SND_PCM_FORMAT_S16_LE},

    };
    static AudioFormat beFormats[]{
        {"32-bit float big-endian", SND_PCM_FORMAT_FLOAT_BE},
        {"32-bit integer big-endian", SND_PCM_FORMAT_S32_BE},
        {"24-bit big-endian", SND_PCM_FORMAT_S24_BE},
        {"24-bit big-endian in 3bytes format", SND_PCM_FORMAT_S24_3BE},
        {"16-bit big-endian", SND_PCM_FORMAT_S16_BE},
    };
    [[noreturn]] static void AlsaError(const std::string &message)
    {
        throw PiPedalStateException(message);
    }

    std::string GetAlsaFormatDescription(snd_pcm_format_t format)
    {
        for (size_t i = 0; i < sizeof(beFormats) / sizeof(beFormats[0]); ++i)
        {
            if (beFormats[i].pcm_format == format)
            {
                return beFormats[i].name;
            }
        }
        for (size_t i = 0; i < sizeof(leFormats) / sizeof(leFormats[0]); ++i)
        {
            if (leFormats[i].pcm_format == format)
            {
                return leFormats[i].name;
            }
        }
        return "Unknown format.";
    }

    void SetPreferredAlsaFormat(
        const std::string &alsa_device_name,
        const char *streamType,
        snd_pcm_t *handle,
        snd_pcm_hw_params_t *hwParams)
    {
        int err;

        if (std::endian::native == std::endian::big)
        {
            if (SetPreferredAlsaFormat(streamType, handle, hwParams, beFormats, sizeof(beFormats) / sizeof(beFormats[0])))
                return;
            if (SetPreferredAlsaFormat(streamType, handle, hwParams, leFormats, sizeof(leFormats) / sizeof(leFormats[0])))
                return;
        }
        else
        {
            if (SetPreferredAlsaFormat(streamType, handle, hwParams, leFormats, sizeof(leFormats) / sizeof(leFormats[0])))
                return;
            if (SetPreferredAlsaFormat(streamType, handle, hwParams, beFormats, sizeof(beFormats) / sizeof(beFormats[0])))
                return;
        }
        AlsaError(SS("No supported audio formats (" << alsa_device_name << "/" << streamType << ")"));
    }

    class AlsaDriverImpl : public AudioDriver
    {
    private:
        pipedal::CpuUse cpuUse;

#ifdef ALSADRIVER_CONFIG_DBG
        snd_output_t *snd_output = nullptr;
        snd_pcm_status_t *snd_status = nullptr;

#endif
        uint32_t sampleRate = 0;

        uint32_t bufferSize;
        uint32_t numberOfBuffers;

        int playbackChannels = 0;
        int captureChannels = 0;

        uint32_t user_threshold = 0;
        bool soft_mode = false;

        snd_pcm_format_t captureFormat = snd_pcm_format_t::SND_PCM_FORMAT_UNKNOWN;

        uint32_t playbackSampleSize = 0;
        uint32_t captureSampleSize = 0;
        uint32_t playbackFrameSize = 0;
        uint32_t captureFrameSize = 0;

        using CopyFunction = void (AlsaDriverImpl::*)(size_t frames);

        CopyFunction copyInputFn;
        CopyFunction copyOutputFn;

        bool inputSwapped = false;
        bool outputSwapped = false;

        std::vector<float *> activeCaptureBuffers;
        std::vector<float *> activePlaybackBuffers;

        std::vector<float *> captureBuffers;
        std::vector<float *> playbackBuffers;

        std::vector<uint8_t> rawCaptureBuffer;
        std::vector<uint8_t> rawPlaybackBuffer;

        AudioDriverHost *driverHost = nullptr;


        void validate_capture_handle() { // leftover debugging for a buffer overrun :-/
        #ifdef DEBUG
            if (snd_pcm_type(captureHandle) != SND_PCM_TYPE_HW)
            {
                throw std::runtime_error("Capture handle has been overwritten");
            }
        #endif
        }
    public:
        AlsaDriverImpl(AudioDriverHost *driverHost)
            : driverHost(driverHost)
        {
            midiEventMemory.resize(MAX_MIDI_EVENT * MAX_MIDI_EVENT_SIZE);
            midiEvents.resize(MAX_MIDI_EVENT);
            for (size_t i = 0; i < midiEvents.size(); ++i)
            {
                midiEvents[i].buffer = midiEventMemory.data() + i * MAX_MIDI_EVENT_SIZE;
            }
        }
        virtual ~AlsaDriverImpl()
        {
            Close();
#ifdef ALSADRIVER_CONFIG_DBG
            if (snd_output)
            {
                snd_output_close(snd_output);
                snd_output = nullptr;
            }
            if (snd_status)
            {
                snd_pcm_status_free(snd_status);
                snd_status = nullptr;
            }
#endif
        }

    private:
        void OnShutdown()
        {
            Lv2Log::info("ALSA Audio Server has shut down.");
        }

        static void
        jack_shutdown_fn(void *arg)
        {
            ((AlsaDriverImpl *)arg)->OnShutdown();
        }

        static int xrun_callback_fn(void *arg)
        {
            ((AudioDriverHost *)arg)->OnUnderrun();
            return 0;
        }

        virtual uint32_t GetSampleRate()
        {
            return this->sampleRate;
        }

        JackServerSettings jackServerSettings;

        std::string alsa_device_name;

        snd_pcm_t *playbackHandle = nullptr;
        snd_pcm_t *captureHandle = nullptr;

        unsigned int periods = 0;

        snd_pcm_hw_params_t *captureHwParams = nullptr;
        snd_pcm_sw_params_t *captureSwParams = nullptr;
        snd_pcm_hw_params_t *playbackHwParams = nullptr;
        snd_pcm_sw_params_t *playbackSwParams = nullptr;

        bool capture_and_playback_not_synced = false;

        std::mutex terminateSync;

        std::atomic<bool> terminateAudio_ = false;

        void terminateAudio(bool terminate)
        {
            this->terminateAudio_ = terminate;
        }

        bool terminateAudio()
        {
            return this->terminateAudio_;
        }

    private:
        void AlsaCleanup()
        {

            if (captureHandle)
            {
                snd_pcm_close(captureHandle);
                captureHandle = nullptr;
            }
            if (playbackHandle)
            {
                snd_pcm_close(playbackHandle);
                playbackHandle = nullptr;
            }
            if (captureHwParams)
            {
                snd_pcm_hw_params_free(captureHwParams);
                captureHwParams = nullptr;
            }
            if (captureSwParams)
            {
                snd_pcm_sw_params_free(captureSwParams);
                captureSwParams = nullptr;
            }
            if (playbackHwParams)
            {
                snd_pcm_hw_params_free(playbackHwParams);
                playbackHwParams = nullptr;
            }
            if (playbackSwParams)
            {
                snd_pcm_sw_params_free(playbackSwParams);
                playbackSwParams = nullptr;
            }
            for (auto &midiState : this->midiDevices)
            {
                if (midiState)
                {
                    midiState->Close();
                }
            }
            midiDevices.resize(0);
        }

        std::string discover_alsa_using_apps()
        {
            return ""; // xxx fix me.
        }

        void AlsaConfigureStream(
            const std::string &alsa_device_name,
            const char *streamType,
            snd_pcm_t *handle,
            snd_pcm_hw_params_t *hwParams,
            snd_pcm_sw_params_t *swParams,
            int *channels,
            unsigned int *periods)
        {
            int err;
            snd_pcm_uframes_t stop_th;

            if ((err = snd_pcm_hw_params_any(handle, hwParams)) < 0)
            {
                AlsaError(SS("No playback configurations available (" << snd_strerror(err) << ")"));
            }

            err = snd_pcm_hw_params_set_access(handle, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
            if (err < 0)
            {
                AlsaError("snd_pcm_hw_params_set_access failed.");
            }

            SetPreferredAlsaFormat(alsa_device_name, streamType, handle, hwParams);

            unsigned int sampleRate = (unsigned int)this->sampleRate;
            err = snd_pcm_hw_params_set_rate_near(handle, hwParams,
                                                  &sampleRate, NULL);
            this->sampleRate = sampleRate;
            if (err < 0)
            {
                AlsaError(SS("Can't set sample rate to " << this->sampleRate << " (" << alsa_device_name << "/" << streamType << ")"));
            }
            if (!*channels)
            {
                /*if not user-specified, try to find the maximum
                 * number of channels */
                unsigned int channels_max = 0;
                unsigned int channels_min = 0;
                err = snd_pcm_hw_params_get_channels_max(hwParams,
                                                         &channels_max);
                if (err < 0) {
                    AlsaError(SS("Can't get channels_max."));
                }

                err = snd_pcm_hw_params_get_channels_min(hwParams,
                                                         &channels_min);
                if (err < 0) {
                    AlsaError(SS("Can't get channels_min."));
                }


                *channels = channels_max;

                if (channels_max > 2 && channels_min <= 2 && channels_min > 0) {
                    unsigned int bestChannelConfig = 2;

                    snd_pcm_hw_params_t* test_params;
                    snd_pcm_hw_params_alloca(&test_params);
                    snd_pcm_hw_params_copy(test_params, hwParams);

                    if ((err = snd_pcm_hw_params_set_channels(handle, test_params,
                                                            bestChannelConfig)) >= 0)
                    {
                        *channels = bestChannelConfig;
                    }

                }

                if (*channels > 1024)
                {
                    // The default PCM device has unlimited channels.
                    // report 2 channels
                    *channels = 2;
                }
            }

            if ((err = snd_pcm_hw_params_set_channels(handle, hwParams,
                                                      *channels)) < 0)
            {
                AlsaError(SS("Can't set channel count to " << *channels << " (" << alsa_device_name << "/" << streamType << ")"));
            }

            snd_pcm_uframes_t effectivePeriodSize = this->bufferSize;

            int dir = 0;
            if ((err = snd_pcm_hw_params_set_period_size_near(handle, hwParams,
                                                              &effectivePeriodSize,
                                                              &dir)) < 0)
            {
                AlsaError(SS("Can't set period size to " << this->bufferSize << " (" << alsa_device_name << "/" << streamType << ")"));
            }
            this->bufferSize = effectivePeriodSize;

            *periods = this->numberOfBuffers;
            dir = 0;
            snd_pcm_hw_params_set_periods_min(handle, hwParams, periods, &dir);
            if (*periods < this->numberOfBuffers)
                *periods = this->numberOfBuffers;
            if (snd_pcm_hw_params_set_periods_near(handle, hwParams,
                                                   periods, NULL) < 0)
            {
                AlsaError(SS("Can't set number of periods to " << (*periods) << " (" << alsa_device_name << "/" << streamType << ")"));
            }

            if (*periods < this->numberOfBuffers)
            {
                AlsaError(SS("Got smaller periods " << *periods << " than " << this->numberOfBuffers));
            }

            snd_pcm_uframes_t bSize;

            // if ((err = snd_pcm_hw_params_set_buffer_size(handle, hwParams,
            //                                              *periods *
            //                                                  this->bufferSize)) < 0)
            // {
            //     AlsaError(SS("Can't set buffer length to " << (*periods * this->bufferSize)));
            // }

            if ((err = snd_pcm_hw_params(handle, hwParams)) < 0)
            {
                AlsaError(SS("Cannot set hardware parameters for " << alsa_device_name));
            }

            snd_pcm_sw_params_current(handle, swParams);

            if (handle == this->captureHandle)
            {
                if ((err = snd_pcm_sw_params_set_start_threshold(handle, swParams,
                                                                 0)) < 0)
                {
                    AlsaError(SS("Cannot set start mode for " << alsa_device_name));
                }
            }
            else
            {
                if ((err = snd_pcm_sw_params_set_start_threshold(handle, swParams,
                                                                 0x7fffffff)) < 0)
                {
                    AlsaError(SS("Cannot set start mode for " << alsa_device_name));
                }
            }

            stop_th = *periods * this->bufferSize;
            if (this->soft_mode)
            {
                stop_th = (snd_pcm_uframes_t)-1;
            }

            if ((err = snd_pcm_sw_params_set_stop_threshold(
                     handle, swParams, stop_th)) < 0)
            {
                AlsaError(SS("ALSA: cannot set stop mode for " << alsa_device_name));
            }

            if ((err = snd_pcm_sw_params_set_silence_threshold(
                     handle, swParams, 0)) < 0)
            {
                AlsaError(SS("Cannot set silence threshold for " << alsa_device_name));
            }

            if (handle == this->playbackHandle)
                err = snd_pcm_sw_params_set_avail_min(
                    handle, swParams,
                    this->bufferSize * (*periods - this->numberOfBuffers + 1));
            else
                err = snd_pcm_sw_params_set_avail_min(
                    handle, swParams, this->bufferSize);

            if (err < 0)
            {
                AlsaError(SS("Cannot set avail min for " << alsa_device_name));
            }

            // err = snd_pcm_sw_params_set_tstamp_mode(handle, swParams, SND_PCM_TSTAMP_ENABLE);
            // if (err < 0)
            // {
            //     Lv2Log::info(SS(
            //         "Could not enable ALSA time stamp mode for " << alsa_device_name << " (err " << err << ")"));
            // }

#if SND_LIB_MAJOR >= 1 && SND_LIB_MINOR >= 1
            err = snd_pcm_sw_params_set_tstamp_type(handle, swParams, SND_PCM_TSTAMP_TYPE_MONOTONIC);
            if (err < 0)
            {
                Lv2Log::info(SS(
                    "Could not use monotonic ALSA time stamps for " << alsa_device_name << "(err " << err << ")"));
            }
#endif

            if ((err = snd_pcm_sw_params(handle, swParams)) < 0)
            {
                AlsaError(SS("Cannot set software parameters for " << alsa_device_name));
            }
            err = snd_pcm_prepare(handle);
            if (err < 0)
            {
                AlsaError(SS("ALSA prepare failed. " << snd_strerror(err)));
            }
        }
        void SetAlsaParameters(uint32_t bufferSize, uint32_t numberOfBuffers, uint32_t sampleRate)
        {
            this->bufferSize = bufferSize;
            this->numberOfBuffers = numberOfBuffers;
            this->sampleRate = sampleRate;

            if (this->captureHandle)
            {
                AlsaConfigureStream(
                    this->alsa_device_name,
                    "capture",
                    captureHandle,
                    captureHwParams,
                    captureSwParams,
                    &captureChannels,
                    &this->periods);
            }
            if (this->playbackHandle)
            {
                AlsaConfigureStream(
                    this->alsa_device_name,
                    "playback",
                    playbackHandle,
                    playbackHwParams,
                    playbackSwParams,
                    &playbackChannels,
                    &this->periods);
            }

#ifdef ALSADRIVER_CONFIG_DBG
            snd_pcm_dump(captureHandle, snd_output);
            snd_pcm_dump(playbackHandle, snd_output);
#endif
        }

        int32_t EndianSwap(int32_t v)
        {
            int32_t b0 = v & 0xFF;
            int32_t b1 = (v >> 8) & 0xFF;
            int32_t b2 = (v >> 16) & 0xFF;
            int32_t b3 = (v >> 24) & 0xFF;

            return (b0 << 24) | (b1 << 16) | (b2 << 8) | (b3);
        }
        int16_t EndianSwap(int16_t v)
        {
            int16_t b0 = v & 0xFF;
            int16_t b1 = (v >> 8) & 0xFF;

            return (b0 << 8) | (b1);
        }
        void EndianSwap(float *p, float v_)
        {
            int32_t v = EndianSwap(*(int32_t *)&v_);
            *(int32_t *)p = v;
        }
        template <typename T>
        static T *getCaptureBuffer(std::vector<uint8_t> &buffer) { return (T *)(buffer.data()); }

        void CopyCaptureFloatBe(size_t frames)
        {
            int32_t *p = getCaptureBuffer<int32_t>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->captureBuffers;
            int channels = this->captureChannels;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = EndianSwap(*p);
                    ++p;

                    *(int32_t *)(buffers[channel] + frame) = v;
                }
            }
        }

        void CopyCaptureFloatLe(size_t frames)
        {
            float *p = getCaptureBuffer<float>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->captureBuffers;
            int channels = this->captureChannels;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = *p++;
                    buffers[channel][frame] = v;
                }
            }
        }

        void CopyCaptureS16Le(size_t frames)
        {
            int16_t *p = getCaptureBuffer<int16_t>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->captureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (std::numeric_limits<int16_t>::max() + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int16_t v = *p++;
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS16Be(size_t frames)
        {
            int16_t *p = getCaptureBuffer<int16_t>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->captureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (std::numeric_limits<int16_t>::max() + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int16_t v = EndianSwap(*p++);
                    buffers[channel][frame] = scale * v;
                }
            }
        }

        void CopyCaptureS32Le(size_t frames)
        {
            int32_t *p = getCaptureBuffer<int32_t>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->captureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (std::numeric_limits<int32_t>::max() + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = *p++;
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS24_3Le(size_t frames)
        {
            uint8_t *p = getCaptureBuffer<uint8_t>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->captureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (std::numeric_limits<int32_t>::max() + 1LL);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = (p[0] << 8) + (p[1] << 16) | (p[2] << 24);
                    p += 3;
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS24_3Be(size_t frames)
        {
            uint8_t *p = (uint8_t *)rawCaptureBuffer.data();

            std::vector<float *> &buffers = this->captureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (std::numeric_limits<int32_t>::max() + 1LL);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = (p[2] << 8) + (p[1] << 16) | (p[0] << 24);
                    p += 3;
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS24Le(size_t frames)
        {
            int32_t *p = (int32_t *)rawCaptureBuffer.data();

            std::vector<float *> &buffers = this->captureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (0x00FFFFFFL + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = *p++;
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS24Be(size_t frames)
        {
            int32_t *p = (int32_t *)rawCaptureBuffer.data();

            std::vector<float *> &buffers = this->captureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (0x00FFFFFFL + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = EndianSwap(*p++);
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS32Be(size_t frames)
        {
            int32_t *p = (int32_t *)rawCaptureBuffer.data();

            std::vector<float *> &buffers = this->captureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (std::numeric_limits<int32_t>::max() + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = EndianSwap(*p++);
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyPlaybackS16Le(size_t frames)
        {
            int16_t *p = (int16_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->playbackBuffers;
            int channels = this->playbackChannels;
            constexpr float scale = std::numeric_limits<int16_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    if (v > 1.0f)
                        v = 1.0f;
                    else if (v < -1.0f)
                        v = -1.0f;
                    *p++ = (int16_t)(scale * v);
                }
            }
        }
        void CopyPlaybackS16Be(size_t frames)
        {
            int16_t *p = (int16_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->playbackBuffers;
            int channels = this->playbackChannels;
            constexpr float scale = std::numeric_limits<int16_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    if (v > 1.0f)
                        v = 1.0f;
                    else if (v < -1.0f)
                        v = -1.0f;
                    *p++ = EndianSwap((int16_t)(scale * v));
                }
            }
        }
        void CopyPlaybackS32Le(size_t frames)
        {
            int32_t *p = (int32_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->playbackBuffers;
            int channels = this->playbackChannels;
            constexpr float scale = std::numeric_limits<int32_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    if (v > 1.0f)
                        v = 1.0f;
                    else if (v < -1.0f)
                        v = -1.0f;
                    *p++ = (int32_t)(scale * v);
                }
            }
        }
        void CopyPlaybackS24Le(size_t frames)
        {
            // 24 bits in low bits of an int32_t.

            int32_t *p = (int32_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->playbackBuffers;
            int channels = this->playbackChannels;
            constexpr float scale = 0x00FFFFFF;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    if (v > 1.0f)
                        v = 1.0f;
                    else if (v < -1.0f)
                        v = -1.0f;
                    *p++ = (int32_t)(scale * v);
                }
            }
        }
        void CopyPlaybackS24Be(size_t frames)
        {
            // 24 bits in low bits of an int32_t.

            int32_t *p = (int32_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->playbackBuffers;
            int channels = this->playbackChannels;
            constexpr float scale = 0x00FFFFFF;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    if (v > 1.0f)
                        v = 1.0f;
                    else if (v < -1.0f)
                        v = -1.0f;
                    *p++ = EndianSwap((int32_t)(scale * v));
                }
            }
        }
        void CopyPlaybackS32Be(size_t frames)
        {
            int32_t *p = (int32_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->playbackBuffers;
            int channels = this->playbackChannels;
            constexpr float scale = std::numeric_limits<int32_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    if (v > 1.0f)
                        v = 1.0f;
                    else if (v < -1.0f)
                        v = -1.0f;
                    *p++ = EndianSwap((int32_t)(scale * v));
                }
            }
        }
        void CopyPlaybackS24_3Be(size_t frames)
        {
            uint8_t *p = (uint8_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->playbackBuffers;
            int channels = this->playbackChannels;
            constexpr float scale = std::numeric_limits<int32_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    if (v > 1.0f)
                        v = 1.0f;
                    else if (v < -1.0f)
                        v = -1.0f;
                    int32_t iValue = (int32_t)(scale * v);
                    p[0] = (uint8_t)(iValue >> 24);
                    p[1] = (uint8_t)(iValue >> 16);
                    p[2] = (uint8_t)(iValue >> 8);

                    p += 3;
                }
            }
        }
        void CopyPlaybackS24_3Le(size_t frames)
        {
            uint8_t *p = (uint8_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->playbackBuffers;
            int channels = this->playbackChannels;
            constexpr float scale = std::numeric_limits<int32_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    if (v > 1.0f)
                        v = 1.0f;
                    else if (v < -1.0f)
                        v = -1.0f;
                    int32_t iValue = (int32_t)(scale * v);
                    p[0] = (uint8_t)(iValue >> 8);
                    p[1] = (uint8_t)(iValue >> 16);
                    p[2] = (uint8_t)(iValue >> 24);

                    p += 3;
                }
            }
        }

        void CopyPlaybackFloatLe(size_t frames)
        {
            float *p = (float *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->playbackBuffers;
            int channels = this->captureChannels;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    *p++ = v;
                }
            }
        }
        void CopyPlaybackFloatBe(size_t frames)
        {
            float *p = (float *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->playbackBuffers;
            int channels = this->captureChannels;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    EndianSwap(p, v);
                    p++;
                }
            }
        }

    public:
        void TestFormatEncodeDecode(snd_pcm_format_t captureFormat);

    private:
        void AllocateBuffers(std::vector<float *> &buffers, size_t n)
        {
            buffers.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                buffers[i] = new float[this->bufferSize];
                for (size_t j = 0; j < this->bufferSize; ++j)
                {
                    buffers[i][j] = 0;
                }
            }
        }

        JackChannelSelection channelSelection;
        bool open = false;
        virtual void Open(const JackServerSettings &jackServerSettings, const JackChannelSelection &channelSelection)
        {
            terminateAudio_ = false;
            if (open)
            {
                throw PiPedalStateException("Already open.");
            }
            this->jackServerSettings = jackServerSettings;
            this->channelSelection = channelSelection;

            open = true;
            try
            {
                OpenMidi(jackServerSettings, channelSelection);
                OpenAudio(jackServerSettings, channelSelection);
                std::atomic_thread_fence(std::memory_order::release);
            }
            catch (const std::exception &e)
            {
                std::atomic_thread_fence(std::memory_order::release);

                Close();
                throw;
            }
        }

        void PrepareCaptureFunctions(snd_pcm_format_t captureFormat)
        {
            this->captureFormat = captureFormat;

            switch (captureFormat)
            {
            case SND_PCM_FORMAT_FLOAT_LE:
                captureSampleSize = 4;
                copyInputFn = &AlsaDriverImpl::CopyCaptureFloatLe;
                break;
            case SND_PCM_FORMAT_S24_3LE:
                copyInputFn = &AlsaDriverImpl::CopyCaptureS24_3Le;
                captureSampleSize = 3;
                break;
            case SND_PCM_FORMAT_S32_LE:
                captureSampleSize = 4;
                copyInputFn = &AlsaDriverImpl::CopyCaptureS32Le;
                break;
            case SND_PCM_FORMAT_S24_LE:
                captureSampleSize = 4;
                copyInputFn = &AlsaDriverImpl::CopyCaptureS24Le;
                break;
            case SND_PCM_FORMAT_S16_LE:
                captureSampleSize = 2;
                copyInputFn = &AlsaDriverImpl::CopyCaptureS16Le;
                break;
            case SND_PCM_FORMAT_FLOAT_BE:
                captureSampleSize = 4;
                copyInputFn = &AlsaDriverImpl::CopyCaptureFloatBe;
                captureSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S24_3BE:
                captureSampleSize = 3;
                copyInputFn = &AlsaDriverImpl::CopyCaptureS24_3Be;
                break;
            case SND_PCM_FORMAT_S32_BE:
                copyInputFn = &AlsaDriverImpl::CopyCaptureS32Be;
                captureSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S24_BE:
                copyInputFn = &AlsaDriverImpl::CopyCaptureS24Be;
                captureSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S16_BE:
                copyInputFn = &AlsaDriverImpl::CopyCaptureS16Be;
                captureSampleSize = 2;
                break;
            default:
                break;
            }
            if (copyInputFn == nullptr)
            {
                throw PiPedalStateException(SS("Audio input format not supported. (" << captureFormat << ")"));
            }

            captureFrameSize = captureSampleSize * captureChannels;
            rawCaptureBuffer.resize(captureFrameSize * bufferSize);
            memset(rawCaptureBuffer.data(), 0, captureFrameSize * bufferSize);

            AllocateBuffers(captureBuffers, captureChannels);
        }

        virtual std::string GetConfigurationDescription()
        {
            std::string result = SS(
                "ALSA, "
                << this->alsa_device_name
                << ", " << GetAlsaFormatDescription(this->captureFormat)
                << ", " << this->sampleRate
                << ", " << this->bufferSize << "x" << this->numberOfBuffers
                << ", in: " << this->InputBufferCount() << "/" << this->captureChannels
                << ", out: " << this->OutputBufferCount() << "/" << this->playbackChannels);
            return result;
        }
        void PreparePlaybackFunctions(snd_pcm_format_t playbackFormat)
        {
            copyOutputFn = nullptr;
            switch (playbackFormat)
            {
            case SND_PCM_FORMAT_FLOAT_LE:
                playbackSampleSize = 4;
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackFloatLe;
                break;
            case SND_PCM_FORMAT_S24_3LE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS24_3Le;
                playbackSampleSize = 3;
                break;
            case SND_PCM_FORMAT_S32_LE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS32Le;
                playbackSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S24_LE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS24Le;
                playbackSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S16_LE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS16Le;
                playbackSampleSize = 2;
                break;
            case SND_PCM_FORMAT_FLOAT_BE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackFloatBe;
                playbackSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S24_3BE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS24_3Be;
                playbackSampleSize = 3;
                break;
            case SND_PCM_FORMAT_S32_BE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS32Be;
                playbackSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S24_BE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS24Be;
                playbackSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S16_BE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS16Be;
                playbackSampleSize = 2;
                break;
            default:
                break;
            }
            if (copyOutputFn == nullptr)
            {
                throw PiPedalStateException(SS("Unsupported audio output format. (" << playbackFormat << ")"));
            }

            playbackFrameSize = playbackSampleSize * playbackChannels;
            rawPlaybackBuffer.resize(playbackFrameSize * bufferSize);
            memset(rawPlaybackBuffer.data(), 0, playbackFrameSize * bufferSize);

            AllocateBuffers(playbackBuffers, playbackChannels);
        }

        void OpenAudio(const JackServerSettings &jackServerSettings, const JackChannelSelection &channelSelection)
        {
            int err;

            alsa_device_name = jackServerSettings.GetAlsaInputDevice();

            this->numberOfBuffers = jackServerSettings.GetNumberOfBuffers();
            this->bufferSize = jackServerSettings.GetBufferSize();
            this->user_threshold = jackServerSettings.GetBufferSize();

            try
            {

                err = snd_pcm_open(&playbackHandle, alsa_device_name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
                if (err < 0)
                {
                    switch (errno)
                    {
                    case EBUSY:
                    {
                        std::string apps = discover_alsa_using_apps();
                        std::string message;
                        if (apps.size() != 0)
                        {
                            message =
                                SS("Device " << alsa_device_name << " in use. The following applications are using your soundcard: " << apps
                                             << ". Stop them as neccesary before trying to restart pipedald.");
                        }
                        else
                        {
                            message =
                                SS("Device " << alsa_device_name << " in use. Stop the application using it before trying to restart pipedald. ");
                        }
                        Lv2Log::error(message);
                        throw PiPedalStateException(std::move(message));
                    }
                    break;
                    case EPERM:
                        throw PiPedalStateException(SS("Permission denied opening device '" << alsa_device_name << "'"));
                    default:
                        throw PiPedalStateException(SS("Unexepected error (" << errno << ") opening device '" << alsa_device_name << "'"));
                    }
                }
                if (this->playbackHandle)
                {
                    snd_pcm_nonblock(playbackHandle, 0);
                }

                err = snd_pcm_open(&captureHandle, alsa_device_name.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);

                if (err < 0)
                {
                    switch (errno)
                    {
                    case EBUSY:
                    {
                        std::string apps = discover_alsa_using_apps();
                        std::string message;
                        if (apps.size() != 0)
                        {
                            message =
                                SS("Device " << alsa_device_name << " in use. The following applications are using your soundcard: " << apps
                                             << ". Stop them as neccesary before trying to restart pipedald.");
                        }
                        else
                        {
                            message =
                                SS("Device " << alsa_device_name << " in use. Stop the application using it before trying to restart pipedald. ");
                        }
                        Lv2Log::error(message);
                        throw PiPedalStateException(std::move(message));
                    }
                    break;
                    case EPERM:
                        throw PiPedalStateException(SS("Permission denied opening device '" << alsa_device_name << "'"));
                    default:
                        throw PiPedalStateException(SS("Unexepected error (" << errno << ") opening device '" << alsa_device_name << "'"));
                    }
                }
                if (this->captureHandle)
                {
                    snd_pcm_nonblock(captureHandle, 0);
                }

                if ((err = snd_pcm_hw_params_malloc(&captureHwParams)) < 0)
                {
                    throw PiPedalStateException("Failed to allocate captureHwParams");
                }
                if ((err = snd_pcm_sw_params_malloc(&captureSwParams)) < 0)
                {
                    throw PiPedalStateException("Failed to allocate captureSwParams");
                }
                if ((err = snd_pcm_hw_params_malloc(&playbackHwParams)) < 0)
                {
                    throw PiPedalStateException("Failed to allocate playbackHwParams");
                }
                if ((err = snd_pcm_sw_params_malloc(&playbackSwParams)) < 0)
                {
                    throw PiPedalStateException("Failed to allocate playbackSwParams");
                }

                SetAlsaParameters(jackServerSettings.GetBufferSize(), jackServerSettings.GetNumberOfBuffers(), jackServerSettings.GetSampleRate());
                capture_and_playback_not_synced = false;

                if (captureHandle && playbackHandle)
                {
                    if (snd_pcm_link(playbackHandle,
                                     captureHandle) != 0)
                    {
                        capture_and_playback_not_synced = true;
                    }
                }

                snd_pcm_format_t captureFormat;
                snd_pcm_hw_params_get_format(captureHwParams, &captureFormat);
                copyInputFn = nullptr;

                PrepareCaptureFunctions(captureFormat);

                snd_pcm_format_t playbackFormat;
                snd_pcm_hw_params_get_format(playbackHwParams, &playbackFormat);

                PreparePlaybackFunctions(playbackFormat);
            }
            catch (const std::exception &e)
            {
                AlsaCleanup();
                throw;
            }
        }

        void FillOutputBuffer()
        {
            validate_capture_handle();

            memset(rawPlaybackBuffer.data(), 0, playbackFrameSize * bufferSize);
            int retry = 0;
            while (true)
            {
                auto avail = snd_pcm_avail(this->playbackHandle);
                if (avail < 0)
                {
                    if (++retry >= 5) // kinda sus code. let's make sure we don't spin forever.
                    {
                        throw std::runtime_error("Timed out trying to fill the audio output buffer.");
                    }
                    int err = snd_pcm_prepare(playbackHandle);
                    if (err < 0)
                    {
                        throw PiPedalStateException(SS("Audio playback failed. " << snd_strerror(err)));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                if (avail == 0)
                    break;
                if (avail > this->bufferSize)
                    avail = this->bufferSize;

                ssize_t err = WriteBuffer(playbackHandle, rawPlaybackBuffer.data(), avail);
                if (err < 0)
                {
                    throw PiPedalStateException(SS("Audio playback failed. " << snd_strerror(err)));
                }
            }
            validate_capture_handle();

        }
        void recover_from_output_underrun(snd_pcm_t *capture_handle, snd_pcm_t *playback_handle, int err)
        {
            validate_capture_handle();
            if (err == -EPIPE)
            {
                err = snd_pcm_prepare(playback_handle);
                if (err < 0)
                {
                    throw PiPedalStateException(SS("Can't recover from ALSA output underrun. (" << snd_strerror(err) << ")"));
                }
                FillOutputBuffer();
            }
            else
            {
                throw PiPedalStateException(SS("Can't recover from ALSA output error. (" << snd_strerror(err) << ")"));
            }
            validate_capture_handle();
        }
        void recover_from_input_underrun(snd_pcm_t *capture_handle, snd_pcm_t *playback_handle, int err)
        {
            validate_capture_handle();

            if (err == -EPIPE)
            {

                // Unlink the streams before recovery
                snd_pcm_unlink(capture_handle);

                err = snd_pcm_drop(capture_handle);
                if (err < 0)
                {
                    throw PiPedalStateException(SS("Can't recover from ALSA underrun. (" << snd_strerror(err) << ")"));
                }
                err = snd_pcm_drop(playback_handle);
                if (err < 0)
                {
                    throw PiPedalStateException(SS("Can't recover from ALSA underrun. (" << snd_strerror(err) << ")"));
                }

                // Prepare both streams
                if ((err = snd_pcm_prepare(playback_handle)) < 0)
                {
                    throw std::runtime_error(SS("Cannot prepare playback stream: " << snd_strerror(err)));
                }
                if ((err = snd_pcm_prepare(capture_handle)) < 0)
                {
                    throw std::runtime_error(SS("Cannot prepare capture stream: " << snd_strerror(err)));
                }

                // Fill the playback buffer with silence
                FillOutputBuffer();

                // Resynchronize the streams
                if ((err = snd_pcm_link(capture_handle, playback_handle)) < 0)
                {
                    throw std::runtime_error(SS("Cannot relink streams: " << snd_strerror(err)));
                }

                // Start the streams
                if ((err = snd_pcm_start(capture_handle)) < 0)
                {
                    throw std::runtime_error(SS("Cannot restart capture stream: " << snd_strerror(err)));
                }
                validate_capture_handle();
            }
            else if (err == ESTRPIPE)
            {
                audioRunning = false;
                validate_capture_handle();

                while ((err = snd_pcm_resume(capture_handle)) == -EAGAIN)
                {
                    sleep(1);
                }
                if (err < 0)
                {
                    err = snd_pcm_prepare(capture_handle);
                    if (err < 0)
                    {
                        throw PiPedalStateException(SS("Can't recover from ALSA suspend. (" << snd_strerror(err) << ")"));
                    }
                }
                audioRunning = true;
                validate_capture_handle();

            }
            else
            {
                throw std::runtime_error(SS("Can't restart audio: " << snd_strerror(err)));
            }
        }

        void DumpStatus(snd_pcm_t *handle)
        {
#ifdef ALSADRIVER_CONFIG_DBG
            snd_pcm_status(handle, snd_status);
            snd_pcm_status_dump(snd_status, snd_output);
#endif
        }

        std::jthread *audioThread = nullptr;
        bool audioRunning;

        bool block = false;

        snd_pcm_sframes_t ReadBuffer(snd_pcm_t *handle, uint8_t *buffer, snd_pcm_uframes_t frames)
        {
            // transcode to jack format.
            // expand running status if neccessary.
            // deal with regular and sysex messages split across
            // buffer boundaries (but discard them)
            snd_pcm_sframes_t framesRead;


            auto state = snd_pcm_state(handle);
            auto frame_bytes = this->captureFrameSize;
            do
            {
                framesRead = snd_pcm_readi(handle, buffer, frames);
                if (framesRead < 0)
                {
                    return framesRead;
                }
                if (framesRead > 0)
                {
                    buffer += framesRead * frame_bytes;
                    frames -= framesRead;
                }
                if (framesRead == 0)
                {
                    snd_pcm_wait(captureHandle, 1);
                }
            } while (frames > 0);
            return framesRead;
        }

        void ReadMidiData(uint32_t audioFrame)
        {
            for (size_t i = 0; i < midiDevices.size(); ++i)
            {
                size_t nRead = midiDevices[i]->ReadMidiEvents(
                    this->midiEvents,
                    midiEventCount,
                    audioFrame);
                midiEventCount += nRead;
            }
        }

        long WriteBuffer(snd_pcm_t *handle, uint8_t *buf, size_t frames)
        {
            long framesRead;
            auto frame_bytes = this->playbackFrameSize;

            while (frames > 0)
            {
                framesRead = snd_pcm_writei(handle, buf, frames);
                if (framesRead == -EAGAIN)
                    continue;
                if (framesRead < 0)
                    return framesRead;
                buf += framesRead * frame_bytes;
                frames -= framesRead;
            }
            return 0;
        }
        void AudioThread()
        {
            SetThreadName("alsaDriver");
            try
            {
#if defined(__WIN32)
                // bump thread prioriy two levels to
                // ensure that the service thread doesn't
                // get bogged down by UIwork. Doesn't have to be realtime, but it
                // MUST run at higher priority than UI threads.
                xxx; // TO DO.
#elif defined(__linux__)
                int min = sched_get_priority_min(SCHED_RR);
                int max = sched_get_priority_max(SCHED_RR);

                struct sched_param param;
                memset(&param, 0, sizeof(param));
                param.sched_priority = RT_THREAD_PRIORITY;

                int result = sched_setscheduler(0, SCHED_RR, &param);
                if (result == 0)
                {
                    Lv2Log::debug("Service thread priority successfully boosted.");
                }
                else
                {
                    Lv2Log::error(SS("Failed to set ALSA AudioThread priority. (" << strerror(result) << ")"));
                }
#else
                xxx; // TODO!
#endif

                bool ok = true;

                auto playbackState = snd_pcm_state(playbackHandle);

                FillOutputBuffer();

                int err;
                if ((err = snd_pcm_start(captureHandle)) < 0)
                {
                    throw PiPedalStateException("Unable to start ALSA capture.");
                }

                cpuUse.SetStartTime(cpuUse.Now());
                while (true)
                {
                    validate_capture_handle();
                    cpuUse.UpdateCpuUse();

                    if (terminateAudio())
                    {
                        break;
                    }
                    this->midiEventCount = 0;

                    // snd_pcm_wait(captureHandle, 1);
                    ssize_t framesToRead = bufferSize;
                    ssize_t framesRead = 0;
                    bool xrun = false;
                    validate_capture_handle();

                    while (framesToRead != 0)
                    {
                        ReadMidiData((uint32_t)framesRead);

                        ssize_t thisTime = framesToRead;
                        ssize_t nFrames;
                        if ((nFrames = ReadBuffer(
                                 captureHandle,
                                 this->rawCaptureBuffer.data() + this->captureFrameSize * framesRead,
                                 framesToRead)) < 0)
                        {
                            this->driverHost->OnUnderrun();
                            recover_from_input_underrun(captureHandle, playbackHandle, nFrames);
                            xrun = true;
                            break;
                        }
                        framesRead += nFrames;
                        framesToRead -= nFrames;
                    }
                    validate_capture_handle();

                    if (xrun)
                    {
                        continue;
                    }
                    cpuUse.AddSample(ProfileCategory::Read);
                    if (framesRead == 0)
                        continue;
                    if (framesRead != bufferSize)
                    {
                        throw PiPedalStateException("Invalid read.");
                    }

                    (this->*copyInputFn)(framesRead);
                    cpuUse.AddSample(ProfileCategory::Driver);

                    this->driverHost->OnProcess(framesRead);

                    cpuUse.AddSample(ProfileCategory::Execute);

                    (this->*copyOutputFn)(framesRead);
                    cpuUse.AddSample(ProfileCategory::Driver);
                    // process.

                    ssize_t err = WriteBuffer(playbackHandle, rawPlaybackBuffer.data(), framesRead);

                    if (err < 0)
                    {
                        this->driverHost->OnUnderrun();
                        recover_from_output_underrun(captureHandle, playbackHandle, err);
                    }
                    cpuUse.AddSample(ProfileCategory::Write);
                }
            }
            catch (const std::exception &e)
            {
                Lv2Log::error(e.what());
                Lv2Log::error("ALSA audio thread terminated abnormally.");
            }
            this->driverHost->OnAudioStopped();

            // if we terminated abnormally, pump messages until we have been terminated.
            if (!terminateAudio())
            {
                // zero out input buffers.
                for (size_t i = 0; i < this->captureBuffers.size(); ++i)
                {
                    float *pBuffer = captureBuffers[i];
                    for (size_t j = 0; j < this->bufferSize; ++j)
                    {
                        pBuffer[j] = 0;
                    }
                }
                while (!terminateAudio())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    // zero out input buffers.
                    this->driverHost->OnProcess(this->bufferSize);
                }
            }
            this->driverHost->OnAudioTerminated();
        }

        bool alsaActive = false;

        static int IndexFromPortName(const std::string &s)
        {
            auto pos = s.find_last_of('_');
            if (pos == std::string::npos)
            {
                throw std::invalid_argument("Bad port name.");
            }
            const char *p = s.c_str() + (pos + 1);

            int v = atoi(p);
            if (v < 0)
            {
                throw std::invalid_argument("Bad port name.");
            }
            return v;
        }

        bool activated = false;
        virtual void Activate()
        {
            if (activated)
            {
                throw PiPedalStateException("Already activated.");
            }
            activated = true;

            this->activeCaptureBuffers.resize(channelSelection.GetInputAudioPorts().size());

            int ix = 0;
            for (auto &x : channelSelection.GetInputAudioPorts())
            {
                int sourceIndex = IndexFromPortName(x);
                if (sourceIndex >= captureBuffers.size())
                {
                    Lv2Log::error(SS("Invalid audio input port: " << x));
                }
                else
                {
                    this->activeCaptureBuffers[ix++] = this->captureBuffers[sourceIndex];
                }
            }

            this->activePlaybackBuffers.resize(channelSelection.GetOutputAudioPorts().size());

            ix = 0;
            for (auto &x : channelSelection.GetOutputAudioPorts())
            {
                int sourceIndex = IndexFromPortName(x);
                if (sourceIndex >= playbackBuffers.size())
                {
                    Lv2Log::error(SS("Invalid audio output port: " << x));
                }
                else
                {
                    this->activePlaybackBuffers[ix++] = this->playbackBuffers[sourceIndex];
                }
            }

            audioThread = new std::jthread([this]()
                                           { AudioThread(); });
        }

        virtual void Deactivate()
        {
            if (!activated)
            {
                return;
            }
            activated = false;
            terminateAudio(true);
            if (audioThread)
            {
                this->audioThread->join();
                this->audioThread = 0;
            }
            Lv2Log::debug("Audio thread joined.");
        }

        static constexpr size_t MAX_MIDI_EVENT_SIZE = 3;
        static constexpr size_t MIDI_BUFFER_SIZE = 16 * 1024;
        static constexpr size_t MAX_MIDI_EVENT = 4 * 1024;

        size_t midiEventCount = 0;
        std::vector<MidiEvent> midiEvents;
        std::vector<uint8_t> midiEventMemory;

    public:
        class AlsaMidiDeviceImpl
        {
        private:
            snd_rawmidi_t *hIn = nullptr;
            snd_rawmidi_params_t *hInParams = nullptr;
            std::string deviceName;

            // running status state.
            uint8_t runningStatus = 0;
            int dataLength = 0;
            int dataIndex = 0;
            size_t statusBytesRemaining = 0;
            size_t data0 = 0;
            size_t data1 = 0;

            bool inputProcessingSysex = false;
            size_t inputSysexBufferCount = 0;
            std::vector<uint8_t> inputSysexBuffer;

            uint8_t readBuffer[1024];

            void checkError(int result, const char *message)
            {
                if (result < 0)
                {
                    throw PiPedalStateException(SS("Unexpected error: " << message << " (" << this->deviceName));
                }
            }

        public:
            AlsaMidiDeviceImpl()
            {
                inputSysexBuffer.resize(1024);
            }
            void Open(const AlsaMidiDeviceInfo &device)
            {
                runningStatus = 0;
                inputProcessingSysex = false;
                inputSysexBufferCount = 0;

                dataIndex = 0;
                dataLength = 0;

                this->deviceName = device.description_;

                int err = snd_rawmidi_open(&hIn, nullptr, device.name_.c_str(), SND_RAWMIDI_NONBLOCK);
                if (err < 0)
                {
                    throw PiPedalStateException(SS("Can't open midi device " << deviceName << ". (" << snd_strerror(err)));
                }

                err = snd_rawmidi_params_malloc(&hInParams);
                checkError(err, "snd_rawmidi_params_malloc failed.");

                err = snd_rawmidi_params_set_buffer_size(hIn, hInParams, 2048);
                checkError(err, "snd_rawmidi_params_set_buffer_size failed.");

                err = snd_rawmidi_params_set_no_active_sensing(hIn, hInParams, 1);
                checkError(err, "snd_rawmidi_params_set_no_active_sensing failed.");
            }
            void Close()
            {
                if (hIn)
                {
                    snd_rawmidi_close(hIn);
                    hIn = nullptr;
                }
                if (hInParams)
                {
                    snd_rawmidi_params_free(hInParams);
                    hInParams = 0;
                }
            }

            int GetDataLength(uint8_t cc)
            {
                static int sDataLength[] = {0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 1, 1, 1, -1};
                return sDataLength[cc >> 4];
            }

            void MidiPut(uint8_t cc, uint8_t d0, uint8_t d1)
            {
                if (cc == 0)
                    return;

                // check for overrun.
                if (inputEventBufferIndex >= pInputEventBuffer->size())
                {
                    return;
                }

                auto &event = (*pInputEventBuffer)[inputEventBufferIndex];

                event.time = inputSampleFrame;
                event.size = dataLength + 1;
                assert(dataLength + 1 <= MAX_MIDI_EVENT_SIZE);
                event.buffer[0] = cc;
                event.buffer[1] = d0;
                event.buffer[2] = d1;
                ++inputEventBufferIndex;
            }

            void FillInputBuffer()
            {
                while (true)
                {
                    ssize_t nRead = snd_rawmidi_read(hIn, readBuffer, sizeof(readBuffer));
                    if (nRead == -EAGAIN)
                        return;
                    if (nRead < 0)
                    {
                        checkError(nRead, SS(this->deviceName << "MIDI event read failed. (" << snd_strerror(nRead)).c_str());
                    }
                    ProcessInputBuffer(readBuffer, nRead); // expose write to test code.
                }
            }

            uint32_t inputSampleFrame = -1;
            size_t inputEventBufferIndex;
            std::vector<MidiEvent> *pInputEventBuffer = nullptr;

            size_t ReadMidiEvents(
                std::vector<MidiEvent> &outputBuffer,
                size_t startIndex,
                uint32_t sampleFrame)
            {
                inputSampleFrame = sampleFrame;
                inputEventBufferIndex = startIndex;
                pInputEventBuffer = &outputBuffer;
                FillInputBuffer();
                pInputEventBuffer = nullptr;
                return inputEventBufferIndex - startIndex;
            }

            void FlushSysex()
            {
                if (inputProcessingSysex)
                {
                    // just discard it. :-/
                    // if (this->eventCount != MAX_MIDI_EVENT)
                    // {
                    //     auto *event = &(events[eventCount++]);
                    //     event->size = this->bufferCount - sysexStartIndex;
                    //     event->buffer = &(this->buffer[this->sysexStartIndex]);
                    //     event->time = 0;
                    // }
                    // sysexStartIndex = -1;
                }
                inputProcessingSysex = false;
            }

            int GetSystemCommonLength(uint8_t cc)
            {
                static int sizes[] = {-1, 1, 2, 1, -1, -1, 0, 0};
                return sizes[(cc >> 4) & 0x07];
            }
            void ProcessInputBuffer(uint8_t *readBuffer, size_t nRead)
            {
                for (ssize_t i = 0; i < nRead; ++i)
                {
                    uint8_t v = readBuffer[i];

                    if (v >= 0x80)
                    {
                        if (v >= 0xF0)
                        {
                            if (v == 0xF0)
                            {
                                inputProcessingSysex = true;
                                inputSysexBufferCount = 0;

                                inputSysexBuffer[inputSysexBufferCount++] = 0xF0;

                                runningStatus = 0; // discard subsequent data.
                                dataLength = -2;   // indefinitely.
                                dataIndex = -1;
                            }
                            else if (v >= 0xF8)
                            {
                                // don't overwrite running status.
                                // don't break sysexes on a running status message.
                                // LV2 standard is ambiguous how realtime messages are handled,  so just discard them.
                                continue;
                            }
                            else
                            {
                                FlushSysex();
                                int length = GetSystemCommonLength(v);
                                if (length == -1)
                                    break; // ignore illegal messages.
                                runningStatus = v;
                                dataLength = length;
                                dataIndex = 0;
                            }
                        }
                        else
                        {
                            FlushSysex();
                            int dataLength = GetDataLength(v);
                            runningStatus = v;
                            if (dataLength == -1)
                            {
                                this->dataLength = dataLength;
                                dataIndex = -1;
                            }
                            else
                            {
                                this->dataLength = dataLength;
                                dataIndex = 0;
                            }
                        }
                    }
                    else
                    {
                        if (inputProcessingSysex)
                        {
                            if (inputSysexBufferCount != inputSysexBuffer.size())
                            {
                                inputSysexBuffer[inputSysexBufferCount++] = v;
                            }
                        }
                        else
                        {
                            switch (dataIndex)
                            {
                            default:
                                // discard.
                                break;
                            case 0:
                                data0 = v;
                                dataIndex = 1;
                                break;
                            case 1:
                                data1 = v;
                                dataIndex = 2;
                                break;
                            }
                        }
                    }
                    if (dataIndex == dataLength && dataLength >= 0 && runningStatus != 0)
                    {
                        MidiPut(runningStatus, data0, data1);
                        dataIndex = 0;
                    }
                }
            }
        };

        std::vector<std::unique_ptr<AlsaMidiDeviceImpl>> midiDevices;

        void OpenMidi(const JackServerSettings &jackServerSettings, const JackChannelSelection &channelSelection)
        {
            const auto &devices = channelSelection.GetInputMidiDevices();

            midiDevices.reserve(devices.size());

            for (size_t i = 0; i < devices.size(); ++i)
            {
                try {
                    const auto &device = devices[i];
                    auto midiDevice = std::make_unique<AlsaMidiDeviceImpl>();
                    midiDevice->Open(device);
                    midiDevices.push_back(std::move(midiDevice));
                } catch (const std::exception &e)
                {
                    Lv2Log::error(e.what());
                }
            }
        }

        virtual size_t InputBufferCount() const { return activeCaptureBuffers.size(); }
        virtual float *GetInputBuffer(size_t channel) override
        {
            return activeCaptureBuffers[channel];
        }

        virtual size_t GetMidiInputEventCount() override
        {
            return midiEventCount;
        }
        virtual MidiEvent *GetMidiEvents() override
        {
            return this->midiEvents.data();
        }

        virtual size_t OutputBufferCount() const { return activePlaybackBuffers.size(); }
        virtual float *GetOutputBuffer(size_t channel) override
        {
            return activePlaybackBuffers[channel];
        }

        void FreeBuffers(std::vector<float *> &buffer)
        {
            for (size_t i = 0; i < buffer.size(); ++i)
            {
                // delete[] buffer[i];
                buffer[i] = 0;
            }
            buffer.clear();
        }
        void DeleteBuffers()
        {
            activeCaptureBuffers.clear();
            activePlaybackBuffers.clear();
            FreeBuffers(this->playbackBuffers);
            FreeBuffers(this->captureBuffers);
        }
        virtual void Close()
        {
            std::atomic_thread_fence(std::memory_order::acquire);

            if (!open)
            {
                return;
            }
            open = false;
            Deactivate();
            AlsaCleanup();
            DeleteBuffers();
            std::atomic_thread_fence(std::memory_order::release);
        }

        virtual float CpuUse()
        {
            return cpuUse.GetCpuUse();
        }

        virtual float CpuOverhead()
        {
            return cpuUse.GetCpuOverhead();
        }
    };

    AudioDriver *CreateAlsaDriver(AudioDriverHost *driverHost)
    {
        return new AlsaDriverImpl(driverHost);
    }

    bool GetAlsaChannels(const JackServerSettings &jackServerSettings,
                         std::vector<std::string> &inputAudioPorts,
                         std::vector<std::string> &outputAudioPorts)
    {
        if (jackServerSettings.IsDummyAudioDevice())
        {
            auto nChannels = GetDummyAudioChannels(jackServerSettings.GetAlsaInputDevice());
            
            inputAudioPorts.clear();
            outputAudioPorts.clear();
            for (uint32_t i = 0; i < nChannels; ++i)
            {
                inputAudioPorts.push_back(std::string(SS("system::capture_" << i)));
                outputAudioPorts.push_back(std::string(SS("system::playback_" << i)));
            }
            return true;
        }

        snd_pcm_t *playbackHandle = nullptr;
        snd_pcm_t *captureHandle = nullptr;
        snd_pcm_hw_params_t *playbackHwParams = nullptr;
        snd_pcm_hw_params_t *captureHwParams = nullptr;
        std::string alsaDeviceName = jackServerSettings.GetAlsaInputDevice();
        bool result = false;

        try
        {
            int err;
            for (int retry = 0; retry < 2; ++retry)
            {
                err = snd_pcm_open(&playbackHandle, alsaDeviceName.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
                if (err < 0) // field report of a device that is present, but won't immediately open.
                {
                    sleep(1);
                    continue;
                }
                break;
            }
            if (err < 0)
            {
                throw PiPedalStateException(SS(alsaDeviceName << " playback device not found. "
                                                              << "(" << snd_strerror(err) << ")"));
            }

            for (int retry = 0; retry < 15; ++retry)
            {
                err = snd_pcm_open(&captureHandle, alsaDeviceName.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
                if (err == -EBUSY)
                {
                    sleep(1);
                    continue;
                }
                break;
            }
            if (err < 0)
                throw PiPedalStateException(SS(alsaDeviceName << " capture device not found."));

            if (snd_pcm_hw_params_malloc(&playbackHwParams) < 0)
            {
                throw PiPedalLogicException("Out of memory.");
            }
            if (snd_pcm_hw_params_malloc(&captureHwParams) < 0)
            {
                throw PiPedalLogicException("Out of memory.");
            }

            snd_pcm_hw_params_any(playbackHandle, playbackHwParams);
            snd_pcm_hw_params_any(captureHandle, captureHwParams);

            SetPreferredAlsaFormat(alsaDeviceName, "capture", captureHandle, captureHwParams);
            SetPreferredAlsaFormat(alsaDeviceName, "output", playbackHandle, playbackHwParams);

            unsigned int sampleRate = jackServerSettings.GetSampleRate();
            err = snd_pcm_hw_params_set_rate_near(playbackHandle, playbackHwParams, &sampleRate, 0);
            if (err < 0)
            {
                throw PiPedalLogicException("Sample rate not supported.");
            }
            sampleRate = jackServerSettings.GetSampleRate();
            err = snd_pcm_hw_params_set_rate_near(captureHandle, captureHwParams, &sampleRate, 0);
            if (err < 0)
            {
                throw PiPedalLogicException("Sample rate not supported.");
            }

            unsigned int playbackChannels, captureChannels;

            err = snd_pcm_hw_params_get_channels_max(playbackHwParams, &playbackChannels);
            if (err < 0)
            {
                throw PiPedalLogicException("No outut channels.");
            }
            unsigned int channelsMin;
            err = snd_pcm_hw_params_get_channels_min(playbackHwParams, &channelsMin);
            if (err < 0)
            {
                throw PiPedalLogicException("No outut channels.");
            }
            if (playbackChannels > 2 && channelsMin <=2 && channelsMin > 0)
            {
                snd_pcm_hw_params_t* test_params;
                snd_pcm_hw_params_alloca(&test_params);
                snd_pcm_hw_params_copy(test_params, playbackHwParams);


                if (snd_pcm_hw_params_set_channels(playbackHandle,test_params,(unsigned int)2) >= 0)
                {   
                    playbackChannels = 2;
                }
            }

            err = snd_pcm_hw_params_get_channels_max(captureHwParams, &captureChannels);
            if (err < 0)
            {
                throw PiPedalLogicException("No input channels.");
            }
            err = snd_pcm_hw_params_get_channels_min(captureHwParams,&channelsMin);
            if (err >= 0)
            {
                if (captureChannels > 2 && channelsMin <= 2 && channelsMin > 0)
                {
                    snd_pcm_hw_params_t* test_params;
                    snd_pcm_hw_params_alloca(&test_params);
                    snd_pcm_hw_params_copy(test_params, captureHwParams);

                    
                    if (snd_pcm_hw_params_set_channels(captureHandle,test_params,(unsigned int)2) >= 0)
                    {   
                        captureChannels = 2;
                    }

                }                
            }

            inputAudioPorts.clear();
            for (unsigned int i = 0; i < captureChannels; ++i)
            {
                inputAudioPorts.push_back(SS("system::capture_" << i));
            }

            outputAudioPorts.clear();
            for (unsigned int i = 0; i < playbackChannels; ++i)
            {
                outputAudioPorts.push_back(SS("system::playback_" << i));
            }

            result = true;
        }
        catch (const std::exception &e)
        {
            result = false;
            throw;
        }
        if (playbackHwParams)
        {
            snd_pcm_hw_params_free(playbackHwParams);
            playbackHwParams = nullptr;
        }

        if (captureHwParams)
        {
            snd_pcm_hw_params_free(captureHwParams);
            captureHwParams = nullptr;
        }

        if (playbackHandle)
        {
            snd_pcm_close(playbackHandle);
            playbackHandle = nullptr;
        }
        if (captureHandle)
        {
            snd_pcm_close(captureHandle);
            captureHandle = nullptr;
        }
        return result;
    }

    static void AlsaAssert(bool value)
    {
        if (!value)
            throw PiPedalStateException("Assert failed.");
    }

#ifdef JUNK
    static void ExpectEvent(AlsaDriverImpl::AlsaMidiDeviceImpl &m, int event, const std::vector<uint8_t> message)
    {
        MidiEvent e;
        m.GetMidiInputEvent(&e, event);
        AlsaAssert(e.size == message.size());
        for (size_t i = 0; i < message.size(); ++i)
        {
            AlsaAssert(message[i] == e.buffer[i]);
        }
    }
#endif

    void AlsaDriverImpl::TestFormatEncodeDecode(snd_pcm_format_t captureFormat)
    {
        this->alsa_device_name = "Test";
        this->numberOfBuffers = 3;
        this->bufferSize = 64;
        this->user_threshold = this->bufferSize;
        this->sampleRate = 44100;
        this->captureChannels = 2;
        this->playbackChannels = 2;

        PrepareCaptureFunctions(captureFormat);
        PreparePlaybackFunctions(captureFormat);

        // make sure encode decode round-trips with reasonable accuracy.

        for (size_t i = 0; i < bufferSize; ++i)
        {
            for (size_t c = 0; c < captureChannels; ++c)
            {
                // provide a rich set of approximately readable bits in the output.
                float value = 1.0f * i / bufferSize + 1.0f * (i) / (128.0 * 256.0);

                // only 16-bits of precision in data for 16-bit formats
                if (captureFormat != snd_pcm_format_t::SND_PCM_FORMAT_S16_BE && captureFormat != snd_pcm_format_t::SND_PCM_FORMAT_S16_LE)
                {
                    value += 1.0f * (c) / (128.0 * 256.0 * 256.0);
                }
                this->playbackBuffers[c][i] = value;
            }
        }

        (this->*copyOutputFn)(bufferSize);

        assert(captureFrameSize == playbackFrameSize);
        memcpy(this->rawCaptureBuffer.data(), this->rawPlaybackBuffer.data(), captureFrameSize * bufferSize);

        (this->*copyInputFn)(bufferSize);

        for (size_t i = 0; i < bufferSize; ++i)
        {
            for (size_t c = 0; c < captureChannels; ++c)
            {
                float error =
                    this->captureBuffers[c][i] - this->playbackBuffers[c][i];

                assert(std::abs(error) < 4e-5);
            }
        }
    }

    void AlsaFormatEncodeDecodeTest(AudioDriverHost *testDriverHost)
    {
        static snd_pcm_format_t formats[] = {
            snd_pcm_format_t::SND_PCM_FORMAT_S16_LE,
            snd_pcm_format_t::SND_PCM_FORMAT_S16_BE,
            snd_pcm_format_t::SND_PCM_FORMAT_S32_LE,
            snd_pcm_format_t::SND_PCM_FORMAT_S32_BE,
            snd_pcm_format_t::SND_PCM_FORMAT_S24_3BE,
            snd_pcm_format_t::SND_PCM_FORMAT_S24_3LE,
            snd_pcm_format_t::SND_PCM_FORMAT_FLOAT_BE,
            snd_pcm_format_t::SND_PCM_FORMAT_FLOAT_LE,
        };

        for (auto format : formats)
        {
            // Check audio encode/decode.
            std::unique_ptr<AlsaDriverImpl> alsaDriver{
                (AlsaDriverImpl *)new AlsaDriverImpl(testDriverHost)};

            alsaDriver->TestFormatEncodeDecode(format);
        }
    }
    void MidiDecoderTest()
    {
#ifdef JUNK
        AlsaDriverImpl::AlsaMidiDeviceImpl midiState;

        MidiEvent event;

        // Running status decoding.
        {
            static uint8_t m0[] = {0x80, 0x1, 0x2, 0x3, 0x4, 0x5};
            midiState.NextEventBuffer();
            midiState.ProcessInputBuffer(m0, sizeof(m0));
            AlsaAssert(midiState.GetMidiInputEventCount() == 2);
            AlsaAssert(midiState.GetMidiInputEvent(&event, 0));

            ExpectEvent(midiState, 0, {0x80, 0x1, 0x2});
            ExpectEvent(midiState, 1, {0x80, 0x3, 0x4});

            static uint8_t m1[] = {0x06, 0xC0, 0x1, 0x2};
            midiState.NextEventBuffer();
            midiState.ProcessInputBuffer(m1, sizeof(m1));
            AlsaAssert(midiState.GetMidiInputEventCount() == 3);
            ExpectEvent(midiState, 0, {0x80, 0x05, 0x06});
            ExpectEvent(midiState, 1, {0xC0, 0x1});
            ExpectEvent(midiState, 2, {0xC0, 0x2});
        }

        // SYSEX.
        {
            static uint8_t m0[] = {0xF0, 0x76, 0xF7, 0xA};
            midiState.NextEventBuffer();
            midiState.ProcessInputBuffer(m0, 4);
            AlsaAssert(midiState.GetMidiInputEventCount() == 2);
            AlsaAssert(midiState.GetMidiInputEvent(&event, 0));
            AlsaAssert(event.size == 2);
            AlsaAssert(event.buffer[0] == 0xF0);
            AlsaAssert(event.buffer[1] == 0x76);
        }

        // SPLIT SYSEX
        {
            static uint8_t m0[] = {0xF0, 0x76, 0x3B};
            midiState.NextEventBuffer();
            midiState.ProcessInputBuffer(m0, sizeof(m0));
            AlsaAssert(midiState.GetMidiInputEventCount() == 0);
            static uint8_t m1[] = {0x77, 0xF7};
            midiState.NextEventBuffer();
            midiState.ProcessInputBuffer(m1, sizeof(m1));
            AlsaAssert(midiState.GetMidiInputEventCount() == 2);

            AlsaAssert(midiState.GetMidiInputEvent(&event, 0));
            AlsaAssert(event.size == 0x4);
            AlsaAssert(event.buffer[0] == 0xF0);
            AlsaAssert(event.buffer[1] == 0x76);
            AlsaAssert(event.buffer[2] == 0x3B);
            AlsaAssert(event.buffer[3] == 0x77);
        }
#endif
    }
} // namespace
