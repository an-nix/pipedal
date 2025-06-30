// Copyright (c) 2022-2023 Robin Davies
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

#include "JackConfiguration.hpp"

#include "Lv2Pedalboard.hpp"
#include "VuUpdate.hpp"
#include "json.hpp"
#include "AudioHost.hpp"
#include "JackServerSettings.hpp"
#include <functional>
#include "PiPedalAlsa.hpp"
#include "Promise.hpp"
#include "json_variant.hpp"
#include "RealtimeMidiEventType.hpp"

namespace pipedal
{

    struct RealtimeMidiProgramRequest;
    struct RealtimeNextMidiProgramRequest;
    class PluginHost;
    class Pedalboard;
    class AlsaSequencerConfiguration;

    using PortMonitorCallback = std::function<void(int64_t handle, float value)>;

    class MonitorPortUpdate
    {
    public:
        PortMonitorCallback *callbackPtr; // pointer because function<>'s probably aren't POD.
        int64_t subscriptionHandle;
        float value;
    };
    class RealtimePatchPropertyRequest
    {
    public:
        int64_t clientId;
        int64_t instanceId;
        LV2_URID uridUri;

        enum class RequestType
        {
            PatchGet,
            PatchSet
        };
        RequestType requestType;

        std::function<void(RealtimePatchPropertyRequest *)> onPatchRequestComplete;
        std::function<void(const std::string &jsonResjult)> onSuccess;
        std::function<void(const std::string &error)> onError;

        const char *errorMessage = nullptr;
        std::string jsonResponse;
        int64_t sampleTimeout = 0;

        RealtimePatchPropertyRequest *pNext = nullptr;

        void SetSize(size_t size)
        {
            responseLength = size;
            if (responseLength > sizeof(atomBuffer))
            {
                longAtomBuffer.resize(size);
            }
        }
        size_t GetSize() const { return responseLength; }
        uint8_t *GetBuffer()
        {
            if (responseLength > sizeof(atomBuffer))
            {
                return &(longAtomBuffer[0]);
            }
            return atomBuffer;
        }

    private:
        int responseLength = 0;
        uint8_t atomBuffer[2048];
        std::vector<uint8_t> longAtomBuffer;

    public:
        RealtimePatchPropertyRequest(
            std::function<void(RealtimePatchPropertyRequest *)> onPatchRequestcomplete_,
            int64_t clientId_,
            int64_t instanceId_,
            LV2_URID uridUri_,
            std::function<void(const std::string &jsonResjult)> onSuccess_,
            std::function<void(const std::string &error)> onError_,
            size_t sampleTimeout)
            : onPatchRequestComplete(onPatchRequestcomplete_),
              clientId(clientId_),
              instanceId(instanceId_),
              uridUri(uridUri_),
              onSuccess(onSuccess_),
              onError(onError_),
              sampleTimeout((int64_t)sampleTimeout)

        {
            requestType = RequestType::PatchGet;
        }
        RealtimePatchPropertyRequest(
            std::function<void(RealtimePatchPropertyRequest *)> onPatchRequestcomplete_,
            int64_t clientId_,
            int64_t instanceId_,
            LV2_URID uridUri_,
            LV2_Atom *atomValue,
            std::function<void(const std::string &jsonResjult)> onSuccess_,
            std::function<void(const std::string &error)> onError_,
            size_t sampleTimeout)
            : onPatchRequestComplete(onPatchRequestcomplete_),
              clientId(clientId_),
              instanceId(instanceId_),
              uridUri(uridUri_),
              onSuccess(onSuccess_),
              onError(onError_),
              sampleTimeout(sampleTimeout)
        {
            requestType = RequestType::PatchSet;
            size_t size = atomValue->size + sizeof(LV2_Atom);
            SetSize(size);
            memcpy(GetBuffer(), atomValue, size);
        }
    };

    class MonitorPortSubscription
    {
    public:
        int64_t subscriptionHandle;
        int64_t instanceid;
        std::string key;
        float updateInterval;
        PortMonitorCallback onUpdate;
    };

    class IAudioHostCallbacks
    {
    public:
        virtual void OnNotifyLv2StateChanged(uint64_t instanceId) = 0;
        virtual void OnNotifyMaybeLv2StateChanged(uint64_t instanceId) = 0;
        virtual void OnNotifyVusSubscription(const std::vector<VuUpdate> &updates) = 0;
        virtual void OnNotifyMonitorPort(const MonitorPortUpdate &update) = 0;
        virtual void OnNotifyMidiValueChanged(int64_t instanceId, int portIndex, float value) = 0;
        virtual void OnNotifyMidiListen(uint8_t cc0, uint8_t cc1, uint8_t cc2) = 0;

        virtual void OnNotifyPathPatchPropertyReceived(
            int64_t instanceId,
            LV2_URID pathPatchProperty,
            LV2_Atom *pathProperty) = 0;

        virtual void OnPatchSetReply(uint64_t instanceId, LV2_URID patchSetProperty, const LV2_Atom *atomValue) = 0;

        // virtual bool WantsAtomOutput(uint64_t instanceId,LV2_URID patchSetProperty) = 0;
        // virtual void OnNotifyPatchProperty(uint64_t instanceId, LV2_URID patchSetProperty, const std::string&atomJson) = 0;

        virtual void OnNotifyMidiProgramChange(RealtimeMidiProgramRequest &midiProgramRequest) = 0;
        virtual void OnNotifyNextMidiProgram(const RealtimeNextMidiProgramRequest &request) = 0;
        virtual void OnNotifyNextMidiBank(const RealtimeNextMidiProgramRequest &request) = 0;
        virtual void OnNotifyLv2RealtimeError(int64_t instanceId, const std::string &error) = 0;
        virtual void OnNotifyMidiRealtimeEvent(RealtimeMidiEventType eventType) = 0;
        virtual void OnNotifyMidiRealtimeSnapshotRequest(int32_t snapshotIndex,int64_t snapshotRequestId) = 0;

        virtual void OnAlsaDriverTerminatedAbnormally() = 0;
        virtual void OnAlsaSequencerDeviceAdded(int client, const std::string &clientName) = 0;
        virtual void OnAlsaSequencerDeviceRemoved(int client) = 0;
    };

    class JackHostStatus
    {
    public:
        bool active_ = false;
        std::string errorMessage_;
        bool restarting_;
        uint64_t underruns_;
        float cpuUsage_ = 0;
        uint64_t msSinceLastUnderrun_ = 0;
        int32_t temperaturemC_ = -100000;
        uint64_t cpuFreqMax_ = 0;
        uint64_t cpuFreqMin_ = 0;
        bool hasCpuGovernor_ = true;
        std::string governor_;

        DECLARE_JSON_MAP(JackHostStatus);
    };

    class IHost;

    class AudioHost
    {
    protected:
        AudioHost() {}

    public:
        static AudioHost *CreateInstance(IHost *pHost);
        virtual ~AudioHost() {};

        virtual void UpdateServerConfiguration(const JackServerSettings &jackServerSettings,
                                               std::function<void(bool success, const std::string &errorMessage)> onComplete) = 0;

        virtual void SetNotificationCallbacks(IAudioHostCallbacks *pNotifyCallbacks) = 0;

        virtual void SetListenForMidiEvent(bool listen) = 0;
        virtual void SetListenForAtomOutput(bool listen) = 0;

        //virtual bool UpdatePluginStates(Pedalboard &pedalboard) = 0;
        virtual bool UpdatePluginState(PedalboardItem &pedalboardItem) = 0;

        virtual std::string AtomToJson(const LV2_Atom *atom) = 0;

        virtual void Open(const JackServerSettings &jackServerSettings, const JackChannelSelection &channelSelection) = 0;
        virtual void Close() = 0;

        virtual void SetAlsaSequencerConfiguration(const AlsaSequencerConfiguration &alsaSequencerConfiguration) = 0;
        virtual uint32_t GetSampleRate() = 0;

        virtual JackConfiguration GetServerConfiguration() = 0;

        virtual void SetPedalboard(const std::shared_ptr<Lv2Pedalboard> &pedalboard) = 0;

        virtual void SetControlValue(uint64_t instanceId, const std::string &symbol, float value) = 0;
        virtual void SetInputVolume(float value) = 0;
        virtual void SetOutputVolume(float value) = 0;
        virtual void SetPluginPreset(uint64_t instanceId, const std::vector<ControlValue> &values) = 0;
        virtual void SetBypass(uint64_t instanceId, bool enabled) = 0;

        virtual bool IsOpen() const = 0;

        virtual void SetVuSubscriptions(const std::vector<int64_t> &instanceIds) = 0;
        virtual void SetMonitorPortSubscriptions(const std::vector<MonitorPortSubscription> &subscriptions) = 0;

        virtual void SetSystemMidiBindings(const std::vector<MidiBinding> &bindings) = 0;

        virtual void sendRealtimeParameterRequest(RealtimePatchPropertyRequest *pParameterRequest) = 0;
        virtual void AckMidiProgramRequest(uint64_t requestId) = 0;
        virtual void AckSnapshotRequest(uint64_t snapshotRequestId) = 0;


        virtual JackHostStatus getJackStatus() = 0;

        virtual void LoadSnapshot(Snapshot &snapshot, PluginHost &pluginHost) = 0;

        virtual void OnNotifyPathPatchPropertyReceived(
            int64_t instanceId,
            const std::string &pathPatchPropertyUri,
            const std::string &jsonAtom) = 0;
    };

} // namespace pipedal.