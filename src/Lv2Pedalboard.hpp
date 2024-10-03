// Copyright (c) 2022 Robin Davies
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#pragma once
#include "Pedalboard.hpp"
#include "PluginHost.hpp"
#include "Lv2Effect.hpp"
#include "BufferPool.hpp"
#include <functional>
#include <lv2/urid/urid.h>
#include <functional>
#include "DbDezipper.hpp"

namespace pipedal
{

    class IPatchWriterCallback;
    class RealtimeVuBuffers;
    class RealtimePatchPropertyRequest;
    class RealtimeRingBufferWriter;

    struct Lv2PedalboardError
    {
        int64_t intanceId;
        std::string message;
    };

    class Lv2PedalboardErrorList : public std::vector<Lv2PedalboardError> // (forward declaration issues with a using statement)
    {
    };

    class Lv2Pedalboard
    {
        IHost *pHost = nullptr;

        DbDezipper inputVolume;
        DbDezipper outputVolume;

        BufferPool bufferPool;
        std::vector<float *> pedalboardInputBuffers;
        std::vector<float *> pedalboardOutputBuffers;

        std::vector<std::shared_ptr<IEffect>> effects;
        std::vector<IEffect *> realtimeEffects;

        using Action = std::function<void()>;
        using ProcessAction = std::function<void(uint32_t frames)>;

        std::vector<Action> activateActions;

        std::vector<ProcessAction> processActions;

        std::vector<Action> deactivateActions;

        float *CreateNewAudioBuffer();

        RealtimeRingBufferWriter *ringBufferWriter;

        enum class MappingType
        {
            Linear,
            Circular,
            Momentary,
            Latched,
        };
        class MidiMapping
        {
        public:
            std::shared_ptr<Lv2PluginInfo> pluginInfo; // lifecycle
            const Lv2PortInfo *pPortInfo = nullptr;    // owned by port.
            int instanceId = -1;
            int effectIndex = -1;
            int controlIndex = -1;
            int key; // key to the note or control. internal use only.
            bool hasLastValue = false;
            float lastValue = -1;
            MappingType mappingType;
            MidiBinding midiBinding;
        };

        std::vector<MidiMapping> midiMappings;

        std::vector<float *> PrepareItems(
            std::vector<PedalboardItem> &items,
            std::vector<float *> inputBuffers,
            Lv2PedalboardErrorList &errorList);

        void PrepareMidiMap(const Pedalboard &pedalboard);
        void PrepareMidiMap(const PedalboardItem &pedalboardItem);

        std::vector<float *> AllocateAudioBuffers(int nChannels);
        int CalculateChainInputs(const std::vector<float *> &inputBuffers, const std::vector<PedalboardItem> &items);
        void AppendParameterRequest(uint8_t *atomBuffer, LV2_URID uridParameter);

    public:
        Lv2Pedalboard() {}
        ~Lv2Pedalboard() {}

        void Prepare(IHost *pHost, Pedalboard &pedalboard, Lv2PedalboardErrorList &errorList);

        std::vector<IEffect *> &GetEffects() { return realtimeEffects; }

        int GetIndexOfInstanceId(uint64_t instanceId)
        {
            for (int i = 0; i < this->realtimeEffects.size(); ++i)
            {
                if (this->realtimeEffects[i]->GetInstanceId() == instanceId)
                    return i;
            }
            return -1;
        }
        IEffect *GetEffect(uint64_t instanceId)
        {
            for (int i = 0; i < realtimeEffects.size(); ++i)
            {
                if (realtimeEffects[i]->GetInstanceId() == instanceId)
                {
                    return realtimeEffects[i];
                }
            }
            return nullptr;
        }
        void Activate();
        void Deactivate();
        bool Run(float **inputBuffers, float **outputBuffers, uint32_t samples, RealtimeRingBufferWriter *realtimeWriter);

        void ResetAtomBuffers();

        void ProcessParameterRequests(RealtimePatchPropertyRequest *pParameterRequests);
        void GatherPatchProperties(RealtimePatchPropertyRequest *pParameterRequests);
        void GatherPathPatchProperties(IPatchWriterCallback *cbPatchWriter);

        std::vector<float *> &GetInputBuffers() { return this->pedalboardInputBuffers; }
        std::vector<float *> &GetoutputBuffers() { return this->pedalboardOutputBuffers; }

        int GetControlIndex(uint64_t instanceId, const std::string &symbol);
        void SetControlValue(int effectIndex, int portIndex, float value);
        void SetInputVolume(float value) { this->inputVolume.SetTarget(value); }
        void SetOutputVolume(float value) { this->outputVolume.SetTarget(value); }
        void SetBypass(int effectIndex, bool enabled);

        void ComputeVus(RealtimeVuBuffers *vuConfiguration, uint32_t samples, float **inputBuffers, float **outputBuffers);

        float GetControlOutputValue(int effectIndex, int portIndex);

        typedef void(MidiCallbackFn)(void *data, uint64_t intanceId, int controlIndex, float value);
        void OnMidiMessage(size_t size, uint8_t *data,
                           void *callbackHandle,
                           MidiCallbackFn *pfnCallback);
    };

} // namespace