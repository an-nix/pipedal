// Copyright (c) 2022-2024 Robin Davies
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

#include "pch.h"
#include <future>
#include "ServiceConfiguration.hpp"
#include "AudioConfig.hpp"
#include "ConfigUtil.hpp"
#include <sched.h>
#include "PiPedalModel.hpp"
#include "AudioHost.hpp"
#include "Lv2Log.hpp"
#include <set>
#include "PiPedalConfiguration.hpp"
#include "AdminClient.hpp"
#include "SplitEffect.hpp"
#include "CpuGovernor.hpp"
#include "RingBufferReader.hpp"
#include "PiPedalUI.hpp"
#include "atom_object.hpp"
#include "Lv2PluginChangeMonitor.hpp"
#include "HotspotManager.hpp"
#include "DBusToLv2Log.hpp"
#include "SysExec.hpp"
#include "Updater.hpp"
#include "util.hpp"
#include "DBusLog.hpp"
#include "AvahiService.hpp"

#ifndef NO_MLOCK
#include <sys/mman.h>
#endif /* NO_MLOCK */

using namespace pipedal;

template <typename T>
T &constMutex(const T &mutex)
{
    return const_cast<T &>(mutex);
}

static const char *hexChars = "0123456789ABCDEF";

static std::string BytesToHex(const std::vector<uint8_t> &bytes)
{
    std::stringstream s;

    for (size_t i = 0; i < bytes.size(); ++i)
    {
        uint8_t b = bytes[i];
        s << hexChars[(b >> 4) & 0x0F] << hexChars[b & 0x0F];
    }
    return s.str();
}

PiPedalModel::PiPedalModel()
    : pluginHost(),
      atomConverter(pluginHost.GetMapFeature())
{
    this->updater = Updater::Create();
    this->updater->Start();
    this->currentUpdateStatus = updater->GetCurrentStatus();
    this->pedalboard = Pedalboard::MakeDefault();
#if JACK_HOST
    this->jackServerSettings = this->storage.GetJackServerSettings(); // to get onboarding flag.
    this->jackServerSettings.ReadJackDaemonConfiguration();
#else
    this->jackServerSettings = this->storage.GetJackServerSettings();
#endif
    updater->SetUpdateListener(
        [this](const UpdateStatus &updateStatus)
        {
            this->OnUpdateStatusChanged(updateStatus);
        });


    DbusLogToLv2Log();
    SetDBusLogLevel(DBusLogLevel::Info);

    hotspotManager = HotspotManager::Create();
    hotspotManager->SetNetworkChangingListener(
        [this](bool ethernetConnected, bool hotspotEnabling) {
            OnNetworkChanging(ethernetConnected,hotspotEnabling);
        }
    );
    // don't actuall start the hotspotManager until after LV2 is initialized (in order to avoid logging oddities)
}

void PiPedalModel::Close()
{
    std::unique_ptr<AudioHost> oldAudioHost;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (closed) 
        {
            return;
        }
        closed = true;

        if (avahiService) {
            this->avahiService = nullptr; // and close.
        }

        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        size_t n = this->subscribers.size();
        for (size_t i = 0; i < n; ++i)
        {
            t[i]->Close();
        }
        delete[] t;

        this->subscribers.resize(0);

        oldAudioHost = std::move(this->audioHost);
    } // end lock.

    // lockless to avoid deadlocks while shutting down the audio thread.
    if (oldAudioHost)
    {
        oldAudioHost->Close();
    }
}

PiPedalModel::~PiPedalModel()
{
    CancelNetworkChangingTimer();
    hotspotManager = nullptr; // turn off the hotspot.

    pluginChangeMonitor = nullptr; // stop monitorin LV2 directories.
    try
    {
        adminClient.UnmonitorGovernor();
    }
    catch (...) // noexcept here!
    {
    }

    try
    {
        CurrentPreset currentPreset;
        currentPreset.modified_ = this->hasPresetChanged;
        currentPreset.preset_ = this->pedalboard;
        storage.SaveCurrentPreset(currentPreset);
    }
    catch (...)
    {
    }

    try
    {
        if (audioHost)
        {
            audioHost->Close();
        }
    }
    catch (...)
    {
    }
}

#include <fstream>

void PiPedalModel::Init(const PiPedalConfiguration &configuration)
{
    std::lock_guard<std::recursive_mutex> lock(mutex); // prevent callbacks while we're initializing.

    this->configuration = configuration;
    pluginHost.SetConfiguration(configuration);
    storage.SetConfigRoot(configuration.GetDocRoot());
    storage.SetDataRoot(configuration.GetLocalStoragePath());
    storage.Initialize();
    pluginHost.SetPluginStoragePath(storage.GetPluginUploadDirectory());

    this->systemMidiBindings = storage.GetSystemMidiBindings();

#if JACK_HOST
    this->jackConfiguration = this->jackConfiguration.JackInitialize();
#else
    this->jackServerSettings = storage.GetJackServerSettings();
#endif
}

void PiPedalModel::LoadLv2PluginInfo()
{
    // Not all Lv2 directories have the lv2 base declarations. Load a full set of plugin classes generated on a default /usr/local/lib/lv2 directory.
    std::filesystem::path pluginClassesPath = configuration.GetDocRoot() / "plugin_classes.json";
    try
    {
        if (!std::filesystem::exists(pluginClassesPath))
            throw PiPedalException("File not found.");
        pluginHost.LoadPluginClassesFromJson(pluginClassesPath);
    }
    catch (const std::exception &e)
    {
        std::stringstream s;
        s << "Unable to load " << pluginClassesPath << ". " << e.what();
        throw PiPedalException(s.str().c_str());
    }

    pluginChangeMonitor = std::make_unique<Lv2PluginChangeMonitor>(*this);
    pluginHost.Load(configuration.GetLv2Path().c_str());

    // Copy all presets out of Lilv data to json files
    // so that we can close lilv while we're actually
    // running.
    for (const auto &plugin : pluginHost.GetPlugins())
    {
        if (plugin->has_factory_presets())
        {
            if (!storage.HasPluginPresets(plugin->uri()))
            {
                PluginPresets pluginPresets = pluginHost.GetFactoryPluginPresets(plugin->uri());
                storage.SavePluginPresets(plugin->uri(), pluginPresets);
            }
        }
    }
}

void PiPedalModel::Load()
{
    this->webRoot = configuration.GetWebRoot();
    this->webPort = (uint16_t)configuration.GetSocketServerPort();

    adminClient.MonitorGovernor(storage.GetGovernorSettings());

    // pluginHost.Load(configuration.GetLv2Path().c_str());

    this->pedalboard = storage.GetCurrentPreset(); // the current *saved* preset.

    // the current edited preset, saved only across orderly shutdowns.
    CurrentPreset currentPreset;
    try
    {
        if (storage.RestoreCurrentPreset(&currentPreset))
        {
            this->pedalboard = currentPreset.preset_;
            this->hasPresetChanged = currentPreset.modified_;
        }
    }
    catch (const std::exception &e)
    {
        Lv2Log::warning(SS("Failed to load current preset. " << e.what()));
    }
    UpdateDefaults(&this->pedalboard);

    std::unique_ptr<AudioHost> p{AudioHost::CreateInstance(pluginHost.asIHost())};
    this->audioHost = std::move(p);

    this->audioHost->SetNotificationCallbacks(this);

    this->systemMidiBindings = storage.GetSystemMidiBindings();

    this->audioHost->SetSystemMidiBindings(this->systemMidiBindings);

    if (configuration.GetMLock())
    {
#ifndef NO_MLOCK
        int result = mlockall(MCL_CURRENT | MCL_FUTURE);
        if (result)
        {
            throw PiPedalStateException("mlockall failed. You can disable the call to mlockall  in 'config.json'.");
        }

#endif
    }

    struct sched_param scheduler_params;
    scheduler_params.sched_priority = 10;
    memset(&scheduler_params, 0, sizeof(sched_param));
    sched_setscheduler(0, SCHED_RR, &scheduler_params);

    RestartAudio();
}

IPiPedalModelSubscriber *PiPedalModel::GetNotificationSubscriber(int64_t clientId)
{
    for (size_t i = 0; i < subscribers.size(); ++i)
    {
        if (subscribers[i]->GetClientId() == clientId)
        {
            return subscribers[i];
        }
    }
    return nullptr;
}

void PiPedalModel::AddNotificationSubscription(IPiPedalModelSubscriber *pSubscriber)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    this->subscribers.push_back(pSubscriber);
}
void PiPedalModel::RemoveNotificationSubsription(IPiPedalModelSubscriber *pSubscriber)
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);

        for (auto it = this->subscribers.begin(); it != this->subscribers.end(); ++it)
        {
            if ((*it) == pSubscriber)
            {
                this->subscribers.erase(it);
                break;
            }
        }
        int64_t clientId = pSubscriber->GetClientId();

        this->DeleteMidiListeners(clientId);
        this->DeleteAtomOutputListeners(clientId);

        for (int i = 0; i < this->outstandingParameterRequests.size(); ++i)
        {
            if (outstandingParameterRequests[i]->clientId == clientId)
            {
                outstandingParameterRequests.erase(outstandingParameterRequests.begin() + i);
                --i;
            }
        }
    }
}

void PiPedalModel::PreviewControl(int64_t clientId, int64_t pedalItemId, const std::string &symbol, float value)
{
    IEffect *effect = lv2Pedalboard->GetEffect(pedalItemId);
    if (effect->IsVst3())
    {
        int index = lv2Pedalboard->GetControlIndex(pedalItemId, symbol);
        if (index != -1)
        {
            effect->SetControl(index, value);
        }
    }
    else
    {
        audioHost->SetControlValue(pedalItemId, symbol, value);
    }
}

void PiPedalModel::OnNotifyLv2StateChanged(uint64_t instanceId)
{
    // a sent PATCH_Set, or an explicit state changed notification.
    OnNotifyMaybeLv2StateChanged(instanceId);
    this->SetPresetChanged(-1, true);
}

void PiPedalModel::OnNotifyMaybeLv2StateChanged(uint64_t instanceId)
{
    // one or more received PATCH_Sets, which MAY change the state.
    std::lock_guard<std::recursive_mutex> lock(mutex);
    PedalboardItem *item = pedalboard.GetItem(instanceId);
    if (item != nullptr)
    {

        bool changed = this->audioHost->UpdatePluginState(*item);
        if (changed)
        {

            item->stateUpdateCount(item->stateUpdateCount() + 1);

            Lv2PluginState newState = item->lv2State();
            IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
            for (size_t i = 0; i < subscribers.size(); ++i)
            {
                t[i] = this->subscribers[i];
            }
            size_t n = this->subscribers.size();
            {
                for (size_t i = 0; i < n; ++i)
                {
                    t[i]->OnLv2StateChanged(instanceId, newState);
                }
            }
            delete[] t;
        }
    }
}

void PiPedalModel::SetInputVolume(float value)
{
    PreviewInputVolume(value);
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);

        this->pedalboard.input_volume_db(value);
        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        size_t n = this->subscribers.size();
        {
            for (size_t i = 0; i < n; ++i)
            {
                t[i]->OnInputVolumeChanged(value);
            }
        }
        delete[] t;

        this->SetPresetChanged(-1, true);
    }
}
void PiPedalModel::SetOutputVolume(float value)
{
    PreviewOutputVolume(value);
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        this->pedalboard.output_volume_db(value);
        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        size_t n = this->subscribers.size();
        {
            for (size_t i = 0; i < n; ++i)
            {
                t[i]->OnOutputVolumeChanged(value);
            }
        }
        delete[] t;

        this->SetPresetChanged(-1, true);
    }
}
void PiPedalModel::PreviewInputVolume(float value)
{
    audioHost->SetInputVolume(value);
}
void PiPedalModel::PreviewOutputVolume(float value)
{
    audioHost->SetOutputVolume(value);
}

void PiPedalModel::SetControl(int64_t clientId, int64_t pedalItemId, const std::string &symbol, float value)
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);

        if (!this->pedalboard.SetControlValue(pedalItemId, symbol, value))
        {
            return;
        }

        PedalboardItem *item = pedalboard.GetItem(pedalItemId);

        // change of split type requires rebuild of the effect
        // since it can change the number of output channels.
        if (item != nullptr && item->isSplit() && symbol == "splitType")
        {
            this->FirePedalboardChanged(clientId);
            return;
        }
        PreviewControl(clientId, pedalItemId, symbol, value);

        {

            // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
            IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
            for (size_t i = 0; i < subscribers.size(); ++i)
            {
                t[i] = this->subscribers[i];
            }
            size_t n = this->subscribers.size();
            {
                for (size_t i = 0; i < n; ++i)
                {
                    t[i]->OnControlChanged(clientId, pedalItemId, symbol, value);
                }
            }
            delete[] t;

            this->SetPresetChanged(clientId, true);
        }
    }
}

void PiPedalModel::FireJackConfigurationChanged(const JackConfiguration &jackConfiguration)
{
    // noify subscribers.
    IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
    for (size_t i = 0; i < subscribers.size(); ++i)
    {
        t[i] = this->subscribers[i];
    }
    size_t n = this->subscribers.size();
    for (size_t i = 0; i < n; ++i)
    {
        t[i]->OnJackConfigurationChanged(jackConfiguration);
    }
    delete[] t;
}

void PiPedalModel::FireBanksChanged(int64_t clientId)
{
    // noify subscribers.
    IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
    for (size_t i = 0; i < subscribers.size(); ++i)
    {
        t[i] = this->subscribers[i];
    }
    size_t n = this->subscribers.size();
    for (size_t i = 0; i < n; ++i)
    {
        t[i]->OnBankIndexChanged(this->storage.GetBanks());
    }
    delete[] t;
}

void PiPedalModel::FirePedalboardChanged(int64_t clientId, bool loadAudioThread)
{
    if (loadAudioThread)
    {
        // notify the audio thread.
        if (audioHost && audioHost->IsOpen())
        {
            LoadCurrentPedalboard();

            UpdateRealtimeVuSubscriptions();
            UpdateRealtimeMonitorPortSubscriptions();
        }
    }
    // noify subscribers.
    IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
    for (size_t i = 0; i < subscribers.size(); ++i)
    {
        t[i] = this->subscribers[i];
    }
    size_t n = this->subscribers.size();
    for (size_t i = 0; i < n; ++i)
    {
        t[i]->OnPedalboardChanged(clientId, this->pedalboard);
    }
    delete[] t;
}
void PiPedalModel::SetPedalboard(int64_t clientId, Pedalboard &pedalboard)
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        this->pedalboard = pedalboard;
        UpdateDefaults(&this->pedalboard);

        this->FirePedalboardChanged(clientId);
        this->SetPresetChanged(clientId, true);
    }
}
void PiPedalModel::UpdateCurrentPedalboard(int64_t clientId, Pedalboard &pedalboard)
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);

        // update vst3 presets if neccessary.
        // the pedalboard must be a manipualted instance of the current Lv2Pedalboard.

        UpdateVst3Settings(pedalboard);

        this->pedalboard = pedalboard;
        UpdateDefaults(&this->pedalboard);

        this->FirePedalboardChanged(clientId);
        this->SetPresetChanged(clientId, true);
    }
}

void PiPedalModel::SetPedalboardItemEnable(int64_t clientId, int64_t pedalItemId, bool enabled)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    {
        this->pedalboard.SetItemEnabled(pedalItemId, enabled);

        // Notify clients.
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        size_t n = this->subscribers.size();
        for (size_t i = 0; i < n; ++i)
        {
            t[i]->OnItemEnabledChanged(clientId, pedalItemId, enabled);
        }
        delete[] t;
        this->SetPresetChanged(clientId, true);

        // Notify audo thread.
        this->audioHost->SetBypass(pedalItemId, enabled);
    }
}

void PiPedalModel::GetPresets(PresetIndex *pResult)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    this->storage.GetPresetIndex(pResult);
    pResult->presetChanged(this->hasPresetChanged);
}

Pedalboard PiPedalModel::GetPreset(int64_t instanceId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->storage.GetPreset(instanceId);
}
void PiPedalModel::GetBank(int64_t instanceId, BankFile *pResult)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    this->storage.GetBankFile(instanceId, pResult);
}

void PiPedalModel::SetPresetChanged(int64_t clientId, bool value)
{
    if (value != this->hasPresetChanged)
    {
        hasPresetChanged = value;
        FirePresetsChanged(clientId);
    }
}

void PiPedalModel::FirePresetsChanged(int64_t clientId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    {
        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        size_t n = this->subscribers.size();

        PresetIndex presets;
        GetPresets(&presets);

        for (size_t i = 0; i < n; ++i)
        {
            t[i]->OnPresetsChanged(clientId, presets);
        }
        delete[] t;
    }
}
void PiPedalModel::FirePluginPresetsChanged(const std::string &pluginUri)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    {
        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        size_t n = this->subscribers.size();

        for (size_t i = 0; i < n; ++i)
        {
            t[i]->OnPluginPresetsChanged(pluginUri);
        }
        delete[] t;
    }
}

void PiPedalModel::UpdateVst3Settings(Pedalboard &pedalboard)
{
    // get the vst3 state bundle from lv2Pedalboard for the current pedalboard.
#if ENABLE_VST3
    Pedalboard pb;
    for (IEffect *effect : lv2Pedalboard->GetEffects())
    {
        if (effect->IsVst3())
        {
            PedalboardItem *item = pedalboard.GetItem(effect->GetInstanceId());
            if (item)
            {
                Vst3Effect *vst3Effect = (Vst3Effect *)effect;
                std::vector<uint8_t> state;
                if (vst3Effect->GetState(&state))
                {
                    item->vstState(BytesToHex(state));
                }
            }
        }
    }
#endif
}

void PiPedalModel::SaveCurrentPreset(int64_t clientId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    UpdateVst3Settings(this->pedalboard);
    this->audioHost->UpdatePluginStates(this->pedalboard);
    storage.SaveCurrentPreset(this->pedalboard);
    this->SetPresetChanged(clientId, false);
}

uint64_t PiPedalModel::CopyPluginPreset(const std::string &pluginUri, uint64_t presetId)
{
    uint64_t result = storage.CopyPluginPreset(pluginUri, presetId);
    FirePluginPresetsChanged(pluginUri);
    return result;
}

void PiPedalModel::UpdatePluginPresets(const PluginUiPresets &pluginPresets)
{
    storage.UpdatePluginPresets(pluginPresets);
    FirePluginPresetsChanged(pluginPresets.pluginUri_);
}
int64_t PiPedalModel::SavePluginPresetAs(int64_t instanceId, const std::string &name)
{
    PedalboardItem *item = this->pedalboard.GetItem(instanceId);
    if (!item)
    {
        throw PiPedalException("Plugin not found.");
    }
    uint64_t presetId = storage.SavePluginPreset(name, *item);
    FirePluginPresetsChanged(item->uri());
    return presetId;
}

int64_t PiPedalModel::SaveCurrentPresetAs(int64_t clientId, const std::string &name, int64_t saveAfterInstanceId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    auto pedalboard = this->pedalboard;
    UpdateVst3Settings(pedalboard);
    pedalboard.name(name);
    int64_t result = storage.SaveCurrentPresetAs(pedalboard, name, saveAfterInstanceId);
    FirePresetsChanged(clientId);
    return result;
}

void PiPedalModel::UploadPluginPresets(const PluginPresets &pluginPresets)
{
    if (pluginPresets.pluginUri_.length() == 0)
    {
        throw PiPedalException("Invalid plugin presets.");
    }
    std::lock_guard<std::recursive_mutex> guard{mutex};
    storage.SavePluginPresets(pluginPresets.pluginUri_, pluginPresets);
    FirePluginPresetsChanged(pluginPresets.pluginUri_);
}
int64_t PiPedalModel::UploadPreset(const BankFile &bankFile, int64_t uploadAfter)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    int64_t newPreset = this->storage.UploadPreset(bankFile, uploadAfter);
    FirePresetsChanged(-1);
    return newPreset;
}
int64_t PiPedalModel::UploadBank(BankFile &bankFile, int64_t uploadAfter)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    int64_t newPreset = this->storage.UploadBank(bankFile, uploadAfter);
    FireBanksChanged(-1);
    return newPreset;
}

void PiPedalModel::OnNotifyNextMidiProgram(const RealtimeNextMidiProgramRequest &request)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    try
    {
        PresetIndex index;
        storage.GetPresetIndex(&index);
        auto currentPresetId = storage.GetCurrentPresetId();
        size_t currentPresetIndex = 0;

        for (size_t i = 0; i < index.presets().size(); ++i)
        {
            if (index.presets()[i].instanceId() == currentPresetId)
            {
                currentPresetIndex = i;
                break;
            }
        }
        if (index.presets().size() == 0)
        {
            throw PiPedalException("No presets loaded.");
        }
        if (request.direction < 0)
        {
            if (currentPresetIndex == 0)
            {
                currentPresetIndex = index.presets().size() - 1;
            }
            else
            {
                --currentPresetIndex;
            }
        }
        else
        {
            ++currentPresetIndex;
            if (currentPresetIndex >= index.presets().size())
            {
                currentPresetIndex = 0;
            }
        }
        LoadPreset(-1, index.presets()[currentPresetIndex].instanceId());
    }
    catch (std::exception &e)
    {

        Lv2Log::error(e.what());
    }
    if (this->audioHost)
    {
        this->audioHost->AckMidiProgramRequest(request.requestId);
    }
}

void PiPedalModel::OnNotifyMidiProgramChange(RealtimeMidiProgramRequest &midiProgramRequest)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    try
    {
        if (midiProgramRequest.bank >= 0)
        {
            int64_t bankId = storage.GetBankByMidiBankNumber(midiProgramRequest.bank);
            if (bankId == -1)
                throw PiPedalException("Bank not found.");
            if (bankId != this->storage.GetBanks().selectedBank())
            {
                storage.LoadBank(bankId);

                FireBanksChanged(-1);
                FirePresetsChanged(-1);
            }
        }
        int64_t presetId = storage.GetPresetByProgramNumber(midiProgramRequest.program);
        if (presetId == -1)
            throw PiPedalException("No valid preset.");
        LoadPreset(-1, presetId);
    }
    catch (std::exception &e)
    {

        Lv2Log::error(e.what());
    }
    if (this->audioHost)
    {
        this->audioHost->AckMidiProgramRequest(midiProgramRequest.requestId);
    }
}

void PiPedalModel::LoadPreset(int64_t clientId, int64_t instanceId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    if (storage.LoadPreset(instanceId))
    {
        this->pedalboard = storage.GetCurrentPreset();
        UpdateDefaults(&this->pedalboard);

        this->hasPresetChanged = false; // no fire.
        this->FirePedalboardChanged(clientId);
        this->FirePresetsChanged(clientId); // fire now.
    }
}

int64_t PiPedalModel::CopyPreset(int64_t clientId, int64_t from, int64_t to)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    int64_t result = storage.CopyPreset(from, to);
    if (result != -1)
    {
        this->FirePresetsChanged(clientId); // fire now.
    }
    else
    {
        throw PiPedalStateException("Copy failed.");
    }
    return result;
}
bool PiPedalModel::UpdatePresets(int64_t clientId, const PresetIndex &presets)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    storage.SetPresetIndex(presets);
    FirePresetsChanged(clientId);
    return true;
}

void PiPedalModel::MoveBank(int64_t clientId, int from, int to)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    storage.MoveBank(from, to);
    FireBanksChanged(clientId);
}
int64_t PiPedalModel::DeleteBank(int64_t clientId, int64_t instanceId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    int64_t selectedBank = this->storage.GetBanks().selectedBank();
    int64_t newSelection = storage.DeleteBank(instanceId);

    int64_t newSelectedBank = this->storage.GetBanks().selectedBank();

    this->FireBanksChanged(clientId); // fire now.

    if (newSelectedBank != selectedBank)
    {
        this->OpenBank(clientId, newSelectedBank);
    }
    return newSelection;
}

int64_t PiPedalModel::DeletePreset(int64_t clientId, int64_t instanceId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    int64_t oldSelection = storage.GetCurrentPresetId();
    int64_t newSelection = storage.DeletePreset(instanceId);
    this->FirePresetsChanged(clientId); // fire now.
    if (oldSelection != newSelection)
    {
        this->LoadPreset(
            -1, // can't use cached version.
            newSelection);
    }
    return newSelection;
}
bool PiPedalModel::RenamePreset(int64_t clientId, int64_t instanceId, const std::string &name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (storage.RenamePreset(instanceId, name))
    {
        this->FirePresetsChanged(clientId);
        if (storage.GetCurrentPresetId() == instanceId)
        {
            this->pedalboard.name(name);
            this->FirePedalboardChanged(-1);
        }
        return true;
    }
    else
    {
        throw PiPedalStateException("Rename failed.");
    }
}

GovernorSettings PiPedalModel::GetGovernorSettings()
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        GovernorSettings result;
        result.governor_ = storage.GetGovernorSettings();
        result.governors_ = pipedal::GetAvailableGovernors();
        return result;
    }
}
void PiPedalModel::SetGovernorSettings(const std::string &governor)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    adminClient.SetGovernorSettings(governor);

    this->storage.SetGovernorSettings(governor);

    IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
    {
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        size_t n = this->subscribers.size();
        for (size_t i = 0; i < n; ++i)
        {
            t[i]->OnGovernorSettingsChanged(governor);
        }
        delete[] t;
    }
}

void PiPedalModel::SetWifiConfigSettings(const WifiConfigSettings &wifiConfigSettings)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);


    
#if NEW_WIFI_CONFIG
    if (this->storage.SetWifiConfigSettings(wifiConfigSettings))
    {
        this->UpdateDnsSd();
        if (this->hotspotManager)
        {
            this->hotspotManager->Reload();
        }
    }
#else
    this->storage.SetWifiConfigSettings(wifiConfigSettings);
    adminClient.SetWifiConfig(wifiConfigSettings);
#endif

    {
        // yyy: review locking semantics here. This is wrong. Convert to shared pointers?
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        size_t n = this->subscribers.size();

        WifiConfigSettings settingsWithNoSecrets = storage.GetWifiConfigSettings(); // (the passwordless version)

        for (size_t i = 0; i < n; ++i)
        {
            t[i]->OnWifiConfigSettingsChanged(settingsWithNoSecrets);
        }
        delete[] t;
    }
}

static std::string GetP2pdName()
{
    std::string name = "/etc/pipedal/config/pipedal_p2pd.conf";

    std::string result;

    if (ConfigUtil::GetConfigLine(name, "p2p_device_name", &result))
    {
        return result;
    }
    return "";
}

void PiPedalModel::UpdateDnsSd()
{
    if (!avahiService)
    {
        throw std::runtime_error("Not ready.");
    }
    ServiceConfiguration deviceIdFile;
    deviceIdFile.Load();
    WifiConfigSettings wifiSettings;
    wifiSettings.Load();
    std::string serviceName = wifiSettings.hotspotName_;
    if (serviceName == "")
    {
        serviceName = deviceIdFile.deviceName;
    }
    if (serviceName == "")
    {
        serviceName = "pipedal";
    }

    std::string hostName = GetHostName();
    if (serviceName != "" && deviceIdFile.uuid != "")
    {
        avahiService->Announce(webPort, serviceName, deviceIdFile.uuid, hostName,true);
    }
    else
    {
        // device_uuid file is written at install time. This warning is harmless if you're debugging.
        // Without it, we can't pulblish the website via dnsDS.
        // Run "pipedalconfig --install" to create the file.
        Lv2Log::warning("Cant read device_uuid file from service.conf file. dnsSD announcement skipped.");
    }
}
void PiPedalModel::SetWifiDirectConfigSettings(const WifiDirectConfigSettings &wifiDirectConfigSettings)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    adminClient.SetWifiDirectConfig(wifiDirectConfigSettings);

    this->storage.SetWifiDirectConfigSettings(wifiDirectConfigSettings);

    {
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        size_t n = this->subscribers.size();

        WifiDirectConfigSettings tWifiDirectConfigSettings = storage.GetWifiDirectConfigSettings(); // (the passwordless version)

        for (size_t i = 0; i < n; ++i)
        {
            t[i]->OnWifiDirectConfigSettingsChanged(tWifiDirectConfigSettings);
        }
        delete[] t;

        // update NSD-SD announement.
        UpdateDnsSd();
    }
}

WifiConfigSettings PiPedalModel::GetWifiConfigSettings()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->storage.GetWifiConfigSettings();
}
WifiDirectConfigSettings PiPedalModel::GetWifiDirectConfigSettings()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->storage.GetWifiDirectConfigSettings();
}

void PiPedalModel::SetShowStatusMonitor(bool show)
{
    IPiPedalModelSubscriber **t;
    size_t n;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex); // copy atomically.
        t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        n = this->subscribers.size();
        storage.SetShowStatusMonitor(show);

        // Notify clients.
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        for (size_t i = 0; i < n; ++i)
        {
            t[i]->OnShowStatusMonitorChanged(show);
        }
        delete[] t;
    }
}
bool PiPedalModel::GetShowStatusMonitor()
{
    std::lock_guard<std::recursive_mutex> lock(mutex); // copy atomically.
    return storage.GetShowStatusMonitor();
}

JackConfiguration PiPedalModel::GetJackConfiguration()
{
    std::lock_guard<std::recursive_mutex> lock(mutex); // copy atomically.
    return this->jackConfiguration;
}

void PiPedalModel::RestartAudio(bool useDummyAudioDriver)
{
    try
    {
        if (this->audioHost->IsOpen())
        {

            this->audioHost->Close();
        }
        // restarting is a bit dodgy. It was impossible with Jack, but
        // now very plausible with the ALSA audio stack.

        // Still bugs wrt/ restarting the circular buffers for the audio thread.

        // do a complete reload.

        this->audioHost->SetPedalboard(nullptr);

        auto jackServerSettings = this->jackServerSettings;
        if (useDummyAudioDriver)
        {
            jackServerSettings.UseDummyAudioDevice();
        }

        auto jackConfiguration = this->jackConfiguration;
        jackConfiguration.AlsaInitialize(jackServerSettings);
        if (jackConfiguration.isValid())
        {
            JackChannelSelection selection = storage.GetJackChannelSelection(jackConfiguration);
            selection = selection.RemoveInvalidChannels(jackConfiguration);
        }
        else
        {
            jackConfiguration.setErrorStatus("Error");
        }
        if (!useDummyAudioDriver)
        {
            this->jackConfiguration = jackConfiguration;
            FireJackConfigurationChanged(jackConfiguration);
        }

        if (!jackServerSettings.IsValid() || !jackConfiguration.isValid())
        {
            throw std::runtime_error("Audio configuration not valid.");
        }

        JackChannelSelection channelSelection = this->storage.GetJackChannelSelection(jackConfiguration);
        if (this->jackConfiguration.isValid())
        {
            channelSelection.RemoveInvalidChannels(this->jackConfiguration);
        }
        if (!channelSelection.isValid())
        {
            throw std::runtime_error("Audio configuration not valid.");
        }
        this->audioHost->Open(jackServerSettings, channelSelection);

        this->pluginHost.OnConfigurationChanged(jackConfiguration, channelSelection);

        LoadCurrentPedalboard();

        this->UpdateRealtimeVuSubscriptions();
        UpdateRealtimeMonitorPortSubscriptions();
    }
    catch (const std::exception &e)
    {
        this->audioHost->Close();
        if (useDummyAudioDriver)
        {
            Lv2Log::error(SS("Failed to start dummy audio driver. " << e.what()));
        }
        else
        {
            Lv2Log::error(SS("Failed to start audio. " << e.what()));
        }
        if (!useDummyAudioDriver)
        {
            RestartAudio(true); // use the dummy audio driver.
        }
    }
}

void PiPedalModel::SetJackChannelSelection(int64_t clientId, const JackChannelSelection &channelSelection)
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex); // copy atomically.
        this->storage.SetJackChannelSelection(channelSelection);

        this->pluginHost.OnConfigurationChanged(jackConfiguration, channelSelection);
    }

    RestartAudio(); // no lock to avoid mutex deadlock when reader thread is sending notifications..

    this->FireChannelSelectionChanged(clientId);
}

void PiPedalModel::FireChannelSelectionChanged(int64_t clientId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    {
        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        size_t n = this->subscribers.size();

        JackChannelSelection channelSelection = storage.GetJackChannelSelection(this->jackConfiguration);

        for (size_t i = 0; i < n; ++i)
        {
            t[i]->OnChannelSelectionChanged(clientId, channelSelection);
        }
        delete[] t;
    }
}

JackChannelSelection PiPedalModel::GetJackChannelSelection()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    JackChannelSelection t = this->storage.GetJackChannelSelection(this->jackConfiguration);
    if (this->jackConfiguration.isValid())
    {
        t = t.RemoveInvalidChannels(this->jackConfiguration);
    }
    return t;
}

int64_t PiPedalModel::AddVuSubscription(int64_t instanceId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    int64_t subscriptionId = ++nextSubscriptionId;
    activeVuSubscriptions.push_back(VuSubscription{subscriptionId, instanceId});

    UpdateRealtimeVuSubscriptions();

    return subscriptionId;
}
void PiPedalModel::RemoveVuSubscription(int64_t subscriptionHandle)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (auto i = activeVuSubscriptions.begin(); i != activeVuSubscriptions.end(); ++i)
    {
        if ((*i).subscriptionHandle == subscriptionHandle)
        {
            activeVuSubscriptions.erase(i);
            break;
        }
    }
    UpdateRealtimeVuSubscriptions();
}

void PiPedalModel::OnNotifyMidiValueChanged(int64_t instanceId, int portIndex, float value)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    PedalboardItem *item = this->pedalboard.GetItem(instanceId);
    if (item)
    {
        Lv2PluginInfo::ptr pPluginInfo;
        if (item->uri() == SPLIT_PEDALBOARD_ITEM_URI)
        {
            pPluginInfo = GetSplitterPluginInfo();
        }
        else
        {
            pPluginInfo = pluginHost.GetPluginInfo(item->uri());
        }
        if (pPluginInfo)
        {
            if (portIndex == -1)
            {
                this->pedalboard.SetItemEnabled(instanceId, value != 0);
                // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
                IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
                for (size_t i = 0; i < subscribers.size(); ++i)
                {
                    t[i] = this->subscribers[i];
                }
                size_t n = this->subscribers.size();
                for (size_t i = 0; i < n; ++i)
                {
                    t[i]->OnItemEnabledChanged(-1, instanceId, value != 0);
                }
                delete[] t;

                this->SetPresetChanged(-1, true);
                return;
            }
            else
            {
                for (int i = 0; i < pPluginInfo->ports().size(); ++i)
                {
                    auto &port = pPluginInfo->ports()[i];
                    if (port->index() == portIndex)
                    {
                        std::string symbol = port->symbol();

                        this->pedalboard.SetControlValue(instanceId, symbol, value);
                        {

                            // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
                            IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
                            for (size_t i = 0; i < subscribers.size(); ++i)
                            {
                                t[i] = this->subscribers[i];
                            }
                            size_t n = this->subscribers.size();
                            for (size_t i = 0; i < n; ++i)
                            {
                                t[i]->OnMidiValueChanged(instanceId, symbol, value);
                            }
                            delete[] t;

                            this->SetPresetChanged(-1, true);
                            return;
                        }
                    }
                }
            }
        }
    }
}

void PiPedalModel::OnNotifyVusSubscription(const std::vector<VuUpdate> &updates)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (size_t i = 0; i < updates.size(); ++i)
    {
        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
        for (size_t i = 0; i < subscribers.size(); ++i)
        {
            t[i] = this->subscribers[i];
        }
        size_t n = this->subscribers.size();
        for (size_t i = 0; i < n; ++i)
        {
            t[i]->OnVuMeterUpdate(updates);
        }
        delete[] t;
    }
}

void PiPedalModel::UpdateRealtimeVuSubscriptions()
{
    std::set<int64_t> addedInstances;

    for (int i = 0; i < activeVuSubscriptions.size(); ++i)
    {
        auto instanceId = activeVuSubscriptions[i].instanceid;
        if (pedalboard.HasItem(instanceId) || instanceId == Pedalboard::INPUT_VOLUME_ID || instanceId == Pedalboard::OUTPUT_VOLUME_ID)
        {
            addedInstances.insert(activeVuSubscriptions[i].instanceid);
        }
    }
    if (audioHost)
    {
        std::vector<int64_t> instanceids(addedInstances.begin(), addedInstances.end());
        audioHost->SetVuSubscriptions(instanceids);
    }
}

void PiPedalModel::UpdateRealtimeMonitorPortSubscriptions()
{
    if (!audioHost)
    {
        return;
    }
    audioHost->SetMonitorPortSubscriptions(this->activeMonitorPortSubscriptions);
}

int64_t PiPedalModel::MonitorPort(int64_t instanceId, const std::string &key, float updateInterval, PortMonitorCallback onUpdate)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    int64_t subscriptionId = ++nextSubscriptionId;
    activeMonitorPortSubscriptions.push_back(
        MonitorPortSubscription{subscriptionId, instanceId, key, updateInterval, onUpdate});

    UpdateRealtimeMonitorPortSubscriptions();

    return subscriptionId;
}
void PiPedalModel::UnmonitorPort(int64_t subscriptionHandle)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    for (auto i = activeMonitorPortSubscriptions.begin(); i != activeMonitorPortSubscriptions.end(); ++i)
    {
        if ((*i).subscriptionHandle == subscriptionHandle)
        {
            activeMonitorPortSubscriptions.erase(i);
            UpdateRealtimeMonitorPortSubscriptions();
            break;
        }
    }
}

void PiPedalModel::OnNotifyMonitorPort(const MonitorPortUpdate &update)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    for (auto i = activeMonitorPortSubscriptions.begin(); i != activeMonitorPortSubscriptions.end(); ++i)
    {
        if ((*i).subscriptionHandle == update.subscriptionHandle)
        {

            // make the call ONLY if the subscription handle is still valid.
            (*update.callbackPtr)(update.subscriptionHandle, update.value);
            break;
        }
    }
}

void PiPedalModel::SendSetPatchProperty(
    int64_t clientId,
    int64_t instanceId,
    const std::string propertyUri,
    const json_variant &value,
    std::function<void()> onSuccess,
    std::function<void(const std::string &error)> onError)
{

    std::lock_guard<std::recursive_mutex> lock(mutex);

    LV2_Atom *atomValue = atomConverter.ToAtom(value);

    std::function<void(RealtimePatchPropertyRequest *)> onRequestComplete{
        [this, onSuccess](RealtimePatchPropertyRequest *pParameter)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                bool cancelled = true;
                for (auto i = this->outstandingParameterRequests.begin();
                     i != this->outstandingParameterRequests.end(); ++i)
                {
                    if ((*i) == pParameter)
                    {
                        cancelled = false;
                        this->outstandingParameterRequests.erase(i);
                        break;
                    }
                }
                if (!cancelled)
                {
                    if (pParameter->errorMessage != nullptr)
                    {
                        if (pParameter->onError)
                        {
                            pParameter->onError(pParameter->errorMessage);
                        }
                    }
                    else
                    {
                        if (pParameter->onSuccess)
                        {
                            onSuccess();
                        }
                    }
                }
                delete pParameter;
            }
        }};

    LV2_URID urid = this->pluginHost.GetLv2Urid(propertyUri.c_str());

    RealtimePatchPropertyRequest *request = new RealtimePatchPropertyRequest(
        onRequestComplete,
        clientId, instanceId, urid, atomValue, nullptr, onError);

    outstandingParameterRequests.push_back(request);
    if (this->audioHost)
    {
        this->audioHost->sendRealtimeParameterRequest(request);
    }
}

void PiPedalModel::SendGetPatchProperty(
    int64_t clientId,
    int64_t instanceId,
    const std::string uri,
    std::function<void(const std::string &jsonResjult)> onSuccess,
    std::function<void(const std::string &error)> onError)
{
    std::function<void(RealtimePatchPropertyRequest *)> onRequestComplete{
        [this](RealtimePatchPropertyRequest *pParameter)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                bool cancelled = true;
                for (auto i = this->outstandingParameterRequests.begin();
                     i != this->outstandingParameterRequests.end(); ++i)
                {
                    if ((*i) == pParameter)
                    {
                        cancelled = false;
                        this->outstandingParameterRequests.erase(i);
                        break;
                    }
                }
                if (!cancelled)
                {
                    if (pParameter->errorMessage != nullptr)
                    {
                        if (pParameter->onError)
                        {
                            pParameter->onError(pParameter->errorMessage);
                        }
                    }
                    else if (pParameter->GetSize() == 0 && pParameter->requestType == RealtimePatchPropertyRequest::RequestType::PatchGet)
                    {
                        if (pParameter->onError)
                        {
                            pParameter->onError("No response.");
                        }
                    }
                    else
                    {
                        if (pParameter->onSuccess)
                        {
                            pParameter->onSuccess(pParameter->jsonResponse);
                        }
                    }
                }
                delete pParameter;
            }
        }};

    LV2_URID urid = this->pluginHost.GetLv2Urid(uri.c_str());
    RealtimePatchPropertyRequest *request = new RealtimePatchPropertyRequest(
        onRequestComplete,
        clientId, instanceId, urid, onSuccess, onError);

    std::lock_guard<std::recursive_mutex> lock(mutex);
    outstandingParameterRequests.push_back(request);
    if (this->audioHost)
    {
        this->audioHost->sendRealtimeParameterRequest(request);
    }
}

BankIndex PiPedalModel::GetBankIndex() const
{
    std::lock_guard<std::recursive_mutex> guard(const_cast<std::recursive_mutex &>(mutex));
    return storage.GetBanks();
}

void PiPedalModel::RenameBank(int64_t clientId, int64_t bankId, const std::string &newName)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    storage.RenameBank(bankId, newName);
    FireBanksChanged(clientId);
}

int64_t PiPedalModel::SaveBankAs(int64_t clientId, int64_t bankId, const std::string &newName)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    int64_t newId = storage.SaveBankAs(bankId, newName);
    FireBanksChanged(clientId);
    return newId;
}

void PiPedalModel::OpenBank(int64_t clientId, int64_t bankId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    storage.LoadBank(bankId);
    FireBanksChanged(clientId);

    FirePresetsChanged(clientId);

    this->pedalboard = storage.GetCurrentPreset();

    UpdateDefaults(&this->pedalboard);
    this->hasPresetChanged = false;
    this->FirePedalboardChanged(clientId);
}

JackServerSettings PiPedalModel::GetJackServerSettings()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->jackServerSettings;
}

void PiPedalModel::SetOnboarding(bool value)
{
    std::unique_lock<std::recursive_mutex> guard(mutex);
    this->jackServerSettings.SetIsOnboarding(value);
    SetJackServerSettings(this->jackServerSettings);
}

void PiPedalModel::SetJackServerSettings(const JackServerSettings &jackServerSettings)
{
    std::unique_lock<std::recursive_mutex> guard(mutex);

#if JACK_HOST
    if (!adminClient.CanUseShutdownClient())
    {
        throw PiPedalException("Can't change server settings when running a debug server.");
    }
#endif

    this->jackServerSettings = jackServerSettings;

    // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
    IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
    for (size_t i = 0; i < subscribers.size(); ++i)
    {
        t[i] = this->subscribers[i];
    }
    size_t n = this->subscribers.size();
    for (size_t i = 0; i < n; ++i)
    {
        t[i]->OnJackServerSettingsChanged(jackServerSettings);
    }
    delete[] t;

#if ALSA_HOST
    storage.SetJackServerSettings(jackServerSettings);

    FireJackConfigurationChanged(this->jackConfiguration);

    guard.unlock();
    RestartAudio();

#endif
#if JACK_HOST
    if (adminClient.CanUseShutdownClient())
    {
        // save the current (edited) preset now in case the service shutdown isn't clean.
        CurrentPreset currentPreset;
        currentPreset.modified_ = this->hasPresetChanged;
        currentPreset.preset_ = this->pedalboard;
        storage.SaveCurrentPreset(currentPreset);

        this->jackConfiguration.SetIsRestarting(true);
        FireJackConfigurationChanged(this->jackConfiguration);
        this->audioHost->UpdateServerConfiguration(
            jackServerSettings,
            [this](bool success, const std::string &errorMessage)
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                if (!success)
                {
                    std::stringstream s;
                    s << "UpdateServerconfiguration failed: " << errorMessage;
                    Lv2Log::error(s.str().c_str());
                }
                // Update jack server status.
                if (!success)
                {
                    this->jackConfiguration.SetIsRestarting(false);
                    this->jackConfiguration.SetErrorStatus(errorMessage);
                    FireJackConfigurationChanged(this->jackConfiguration);
                }
                else
                {
                // we now do a complete restart of the services,
                // so just sit tight and wait for the restart.
#ifdef JUNK
                    this->jackConfiguration.SetErrorStatus("");
                    FireJackConfigurationChanged(this->jackConfiguration);

                    // restart the pedalboard on a new instance.
                    std::shared_ptr<Lv2Pedalboard> lv2Pedalboard{this->pluginHost.CreateLv2Pedalboard(this->pedalboard)};
                    this->lv2Pedalboard = lv2Pedalboard;

                    audioHost->SetPedalboard(lv2Pedalboard);
                    UpdateRealtimeVuSubscriptions();
                    UpdateRealtimeMonitorPortSubscriptions();
#endif
                }
            });
    }
#endif
}

void PiPedalModel::UpdateDefaults(PedalboardItem *pedalboardItem)
{
    std::shared_ptr<Lv2PluginInfo> pPlugin = pluginHost.GetPluginInfo(pedalboardItem->uri());
    if (!pPlugin)
    {
        if (pedalboardItem->uri() == SPLIT_PEDALBOARD_ITEM_URI)
        {
            pPlugin = GetSplitterPluginInfo();
        }
    }
    if (pPlugin)
    {
        for (size_t i = 0; i < pPlugin->ports().size(); ++i)
        {
            auto port = pPlugin->ports()[i];
            if (port->is_control_port() && port->is_input())
            {
                ControlValue *pValue = pedalboardItem->GetControlValue(port->symbol());
                if (pValue == nullptr)
                {
                    // Missing? Set it to default value.
                    pedalboardItem->controlValues().push_back(
                        pipedal::ControlValue(port->symbol().c_str(), port->default_value()));
                }
            }
        }
    }
    for (size_t i = 0; i < pedalboardItem->topChain().size(); ++i)
    {
        UpdateDefaults(&(pedalboardItem->topChain()[i]));
    }
    for (size_t i = 0; i < pedalboardItem->bottomChain().size(); ++i)
    {
        UpdateDefaults(&(pedalboardItem->bottomChain()[i]));
    }
}
void PiPedalModel::UpdateDefaults(Pedalboard *pedalboard)
{
    for (size_t i = 0; i < pedalboard->items().size(); ++i)
    {
        UpdateDefaults(&(pedalboard->items()[i]));
    }
}

PluginPresets PiPedalModel::GetPluginPresets(const std::string &pluginUri)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    return storage.GetPluginPresets(pluginUri);
}

PluginUiPresets PiPedalModel::GetPluginUiPresets(const std::string &pluginUri)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    return storage.GetPluginUiPresets(pluginUri);
}

void PiPedalModel::LoadPluginPreset(int64_t pluginInstanceId, uint64_t presetInstanceId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    PedalboardItem *pedalboardItem = this->pedalboard.GetItem(pluginInstanceId);
    if (pedalboardItem != nullptr)
    {
        int32_t oldStateUpdateCount = pedalboardItem->stateUpdateCount();

        PluginPresetValues presetValues = storage.GetPluginPresetValues(pedalboardItem->uri(), presetInstanceId);
        // if the plugin has state, we have to rebuild the pedalboard, since setting state is not thread-safe.
        // Same goes if lilvPresetUri is not empty.

        // lilvPresetUri: use lilv to load the preset from the RDF model. Occurs when using a factory preset
        // that has state:state, because lilv doesn't allow us to read this data, but does load it.
        // This is a transient condition.

        for (auto &control : presetValues.controls)
        {
            this->pedalboard.SetControlValue(pluginInstanceId, control.key(), control.value());
        }

        if ((!presetValues.state.isValid_) && presetValues.lilvPresetUri.empty())
        {
            audioHost->SetPluginPreset(pluginInstanceId, presetValues.controls);

            IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
            for (size_t i = 0; i < subscribers.size(); ++i)
            {
                t[i] = this->subscribers[i];
            }
            size_t n = this->subscribers.size();
            for (size_t i = 0; i < n; ++i)
            {
                t[i]->OnLoadPluginPreset(pluginInstanceId, presetValues.controls);
            }
            delete[] t;
        }
        else
        {
            pedalboardItem->lv2State(presetValues.state);
            pedalboardItem->lilvPresetUri(presetValues.lilvPresetUri);
            pedalboardItem->stateUpdateCount(oldStateUpdateCount + 1);
            FirePedalboardChanged(-1); // does a complete reload of both client and audio server.
        }
        this->SetPresetChanged(-1, true);
    }
}

void PiPedalModel::DeleteAtomOutputListeners(int64_t clientId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (size_t i = 0; i < atomOutputListeners.size(); ++i)
    {
        if (atomOutputListeners[i].clientId == clientId)
        {
            atomOutputListeners.erase(atomOutputListeners.begin() + i);
            --i;
        }
    }
    if (audioHost)
    {
        audioHost->SetListenForAtomOutput(atomOutputListeners.size() != 0);
    }
}

void PiPedalModel::DeleteMidiListeners(int64_t clientId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (size_t i = 0; i < midiEventListeners.size(); ++i)
    {
        if (midiEventListeners[i].clientId == clientId)
        {
            midiEventListeners.erase(midiEventListeners.begin() + i);
            --i;
        }
    }
    if (audioHost)
    {
        audioHost->SetListenForMidiEvent(midiEventListeners.size() != 0);
    }
}

void PiPedalModel::OnPatchSetReply(uint64_t instanceId, LV2_URID patchSetProperty, const LV2_Atom *atomValue)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    std::string propertyUri = pluginHost.GetMapFeature().UridToString(patchSetProperty);

    {
        PedalboardItem *item = pedalboard.GetItem((int64_t)instanceId);
        if (item == nullptr)
            return;
        atom_object atomObject{atomValue};

        PedalboardItem::PropertyMap &properties = item->PatchProperties();
        if (properties.contains(propertyUri))
        {
            if (properties[propertyUri] == atomObject)
            {
                return;
            }
        }
        properties[propertyUri] = std::move(atomObject);
    }

    bool hasAtomJson = false;
    std::string atomJson;

    for (int i = 0; i < atomOutputListeners.size(); ++i)
    {
        auto &listener = atomOutputListeners[i];
        if (listener.WantsProperty(instanceId, patchSetProperty))
        {
            auto subscriber = this->GetNotificationSubscriber(listener.clientId);
            if (subscriber)
            {
                if (!hasAtomJson)
                {
                    atomJson = this->audioHost->AtomToJson(atomValue);
                    hasAtomJson = true;
                }
                subscriber->OnNotifyPatchProperty(listener.clientHandle, instanceId, propertyUri, atomJson);
            }
            else
            {
                atomOutputListeners.erase(atomOutputListeners.begin() + i);
                --i;
            }
        }
    }
    audioHost->SetListenForAtomOutput(atomOutputListeners.size() != 0);
}

void PiPedalModel::OnNotifyMidiListen(bool isNote, uint8_t noteOrControl)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    for (int i = 0; i < midiEventListeners.size(); ++i)
    {
        auto &listener = midiEventListeners[i];
        if ((!isNote) || (!listener.listenForControlsOnly))
        {
            auto subscriber = this->GetNotificationSubscriber(listener.clientId);
            if (subscriber)
            {
                subscriber->OnNotifyMidiListener(listener.clientHandle, isNote, noteOrControl);
            }
            else
            {
                midiEventListeners.erase(midiEventListeners.begin() + i);
                --i;
            }
        }
    }
    audioHost->SetListenForMidiEvent(midiEventListeners.size() != 0);
}

void PiPedalModel::ListenForMidiEvent(int64_t clientId, int64_t clientHandle, bool listenForControlsOnly)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    MidiListener listener{clientId, clientHandle, listenForControlsOnly};
    midiEventListeners.push_back(listener);
    audioHost->SetListenForMidiEvent(true);
}

void PiPedalModel::CancelListenForMidiEvent(int64_t clientId, int64_t clientHandle)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (size_t i = 0; i < midiEventListeners.size(); ++i)
    {
        const auto &listener = midiEventListeners[i];
        if (listener.clientId == clientId && listener.clientHandle == clientHandle)
        {
            midiEventListeners.erase(midiEventListeners.begin() + i);
            break;
        }
    }
    if (midiEventListeners.size() == 0)
    {
        audioHost->SetListenForMidiEvent(false);
    }
}

void PiPedalModel::MonitorPatchProperty(int64_t clientId, int64_t clientHandle, uint64_t instanceId, const std::string &propertyUri)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    LV2_URID propertyUrid = 0;
    if (propertyUri.length() != 0)
    {
        propertyUrid = pluginHost.GetMapFeature().GetUrid(propertyUri.c_str());
    }
    AtomOutputListener listener{clientId, clientHandle, instanceId, propertyUrid};
    atomOutputListeners.push_back(listener);
    audioHost->SetListenForAtomOutput(true);

    PedalboardItem *item = this->pedalboard.GetItem(instanceId);
    if (item)
    {
        auto &map = item->PatchProperties();
        if (map.contains(propertyUri))
        {
            const auto &value = map[propertyUri];
            std::string json = this->audioHost->AtomToJson(value.get());
            for (auto &subscriber : this->subscribers)
            {
                if (subscriber->GetClientId() == clientId)
                {
                    subscriber->OnNotifyPatchProperty(clientHandle, instanceId, propertyUri, json);
                }
            }
        }
    }
}

void PiPedalModel::CancelMonitorPatchProperty(int64_t clientId, int64_t clientHandle)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (size_t i = 0; i < atomOutputListeners.size(); ++i)
    {
        const auto &listener = atomOutputListeners[i];
        if (listener.clientId == clientId && listener.clientHandle == clientHandle)
        {
            atomOutputListeners.erase(atomOutputListeners.begin() + i);
            break;
        }
    } 
    if (midiEventListeners.size() == 0)
    {
        audioHost->SetListenForMidiEvent(false);
    }
}
std::vector<AlsaDeviceInfo> PiPedalModel::GetAlsaDevices()
{
    return this->alsaDevices.GetAlsaDevices();
}

const std::filesystem::path &PiPedalModel::GetWebRoot() const
{
    return webRoot;
}

std::map<std::string, bool> PiPedalModel::GetFavorites() const
{
    std::lock_guard<std::recursive_mutex> guard(const_cast<std::recursive_mutex &>(mutex));

    return storage.GetFavorites();
}
void PiPedalModel::SetFavorites(const std::map<std::string, bool> &favorites)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    storage.SetFavorites(favorites);

    // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
    std::vector<IPiPedalModelSubscriber *> t;
    t.reserve(this->subscribers.size());

    for (size_t i = 0; i < subscribers.size(); ++i)
    {
        t.push_back(this->subscribers[i]);
    }
    for (size_t i = 0; i < t.size(); ++i)
    {
        t[i]->OnFavoritesChanged(favorites);
    }
}
std::vector<MidiBinding> PiPedalModel::GetSystemMidiBidings()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->systemMidiBindings;
}
void PiPedalModel::SetSystemMidiBindings(std::vector<MidiBinding> &bindings)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    this->systemMidiBindings = bindings;
    storage.SetSystemMidiBindings(bindings);
    if (this->audioHost)
    {
        this->audioHost->SetSystemMidiBindings(bindings);
    }

    IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[this->subscribers.size()];
    for (size_t i = 0; i < subscribers.size(); ++i)
    {
        t[i] = this->subscribers[i];
    }
    size_t n = this->subscribers.size();
    for (size_t i = 0; i < n; ++i)
    {
        t[i]->OnSystemMidiBindingsChanged(bindings);
    }
    delete[] t;
}

std::vector<std::string> PiPedalModel::GetFileList(const UiFileProperty &fileProperty)
{
    try
    {
        return this->storage.GetFileList(fileProperty);
    }
    catch (const std::exception &e)
    {
        Lv2Log::warning("GetFileList() failed:  (%s)", e.what());
        return std::vector<std::string>(); // don't disclose to users what the problem is.
    }
}
std::vector<FileEntry> PiPedalModel::GetFileList2(const std::string &relativePath, const UiFileProperty &fileProperty)
{
    try
    {
        return this->storage.GetFileList2(relativePath, fileProperty);
    }
    catch (const std::exception &e)
    {
        Lv2Log::warning("GetFileList() failed:  (%s)", e.what());
        return std::vector<FileEntry>(); // don't disclose to users what the problem is.
    }
}

std::string PiPedalModel::RenameFilePropertyFile(
    const std::string &oldRelativePath,
    const std::string &newRelativePath,
    const UiFileProperty &uiFileProperty)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return storage.RenameFilePropertyFile(oldRelativePath, newRelativePath, uiFileProperty);
}

void PiPedalModel::DeleteSampleFile(const std::filesystem::path &fileName)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    storage.DeleteSampleFile(fileName);
}

std::string PiPedalModel::CreateNewSampleDirectory(const std::string &relativePath, const UiFileProperty &uiFileProperty)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return storage.CreateNewSampleDirectory(relativePath, uiFileProperty);
}
FilePropertyDirectoryTree::ptr PiPedalModel::GetFilePropertydirectoryTree(const UiFileProperty &uiFileProperty)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return storage.GetFilePropertydirectoryTree(uiFileProperty);
}

std::string PiPedalModel::UploadUserFile(const std::string &directory, const std::string &patchProperty, const std::string &filename, std::istream &stream, size_t contentLength)
{
    return storage.UploadUserFile(directory, patchProperty, filename, stream, contentLength);
}

uint64_t PiPedalModel::CreateNewPreset()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    return storage.CreateNewPreset();
}

void PiPedalModel::CheckForResourceInitialization(Pedalboard &pedalboard)
{
    for (auto item : pedalboard.GetAllPlugins())
    {
        if (!item->isSplit())
        {
            pluginHost.CheckForResourceInitialization(item->uri(), storage.GetPluginUploadDirectory());
        }
    }
}
Pedalboard &PiPedalModel::GetPedalboard()
{
    return this->pedalboard;
}
std::shared_ptr<Lv2Pedalboard> PiPedalModel::GetLv2Pedalboard()
{
    // test only.
    Lv2PedalboardErrorList errorMessages;
    std::shared_ptr<Lv2Pedalboard> lv2Pedalboard{this->pluginHost.CreateLv2Pedalboard(this->pedalboard, errorMessages)};
    if (errorMessages.size() != 0)
    {
        throw std::runtime_error(errorMessages[0].message);
    }
    return lv2Pedalboard;
}
bool PiPedalModel::LoadCurrentPedalboard()
{
    Lv2PedalboardErrorList errorMessages;
    std::shared_ptr<Lv2Pedalboard> lv2Pedalboard{this->pluginHost.CreateLv2Pedalboard(this->pedalboard, errorMessages)};
    this->lv2Pedalboard = lv2Pedalboard;

    // apply the error messages to the lv2Pedalboard.
    // return true if the error messages have changed
    CheckForResourceInitialization(this->pedalboard);
    audioHost->SetPedalboard(lv2Pedalboard);
    return true;
}

void PiPedalModel::OnNotifyLv2RealtimeError(int64_t instanceId, const std::string &error)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // Notify clients.
    size_t n = subscribers.size();
    IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[n];
    for (size_t i = 0; i < n; ++i)
    {
        t[i] = this->subscribers[i];
    }
    for (size_t i = 0; i < n; ++i)
    {
        t[i]->OnErrorMessage(error);
    }
    delete[] t;
}
std::filesystem::path PiPedalModel::GetPluginUploadDirectory() const
{
    return storage.GetPluginUploadDirectory();
}

void PiPedalModel::OnLv2PluginsChanged()
{
    Lv2Log::info("Lv2 plugins have changed. Reloading plugins.");
    std::lock_guard<std::recursive_mutex> lock(mutex);
    {
        // Notify clients.
        size_t n = subscribers.size();
        IPiPedalModelSubscriber **t = new IPiPedalModelSubscriber *[n];
        for (size_t i = 0; i < n; ++i)
        {
            t[i] = this->subscribers[i];
        }
        for (size_t i = 0; i < n; ++i)
        {
            t[i]->OnLv2PluginsChanging();
        }
        delete[] t;
    }
    std::thread(
        [this]()
        {
            // wait for the message to propagate. It would be better to use some kind of flush()
            // operation, but it's not clear how to do that with asyncio.
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            restartListener();
        })
        .detach();
}
void PiPedalModel::SetRestartListener(std::function<void(void)> &&listener)
{
    this->restartListener = std::move(listener);
}

void PiPedalModel::OnUpdateStatusChanged(const UpdateStatus &updateStatus)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (this->currentUpdateStatus != updateStatus)
    {
        this->currentUpdateStatus = updateStatus;
        FireUpdateStatusChanged(this->currentUpdateStatus);
    }
}
void PiPedalModel::FireUpdateStatusChanged(const UpdateStatus &updateStatus)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    std::vector<IPiPedalModelSubscriber *> t;
    t.reserve(subscribers.size());

    for (auto subscriber : subscribers)
    {
        t.push_back(subscriber);
    }

    for (auto subscriber : t)
    {
        subscriber->OnUpdateStatusChanged(updateStatus);
    }
}
UpdateStatus PiPedalModel::GetUpdateStatus()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return updater->GetCurrentStatus();
}

void PiPedalModel::UpdateNow(const std::string &updateUrl)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::filesystem::path fileName, signatureName;
    updater->DownloadUpdate(updateUrl, &fileName, &signatureName);

    adminClient.InstallUpdate(fileName);
}
void PiPedalModel::ForceUpdateCheck()
{
    updater->ForceUpdateCheck();
}
void PiPedalModel::SetUpdatePolicy(UpdatePolicyT updatePolicy)
{
    updater->SetUpdatePolicy(updatePolicy);
}

static bool HasAlsaDevice(const std::vector<AlsaDeviceInfo> devices, const std::string &deviceId)
{
    for (auto &device : devices)
    {
        if (device.id_ == deviceId)
            return true;
    }
    return false;
}

void PiPedalModel::StartHotspotMonitoring()
{
    this->avahiService = std::make_unique<AvahiService>();

    SetThreadName("avahi"); // hack to name the avahi service thread.
    UpdateDnsSd(); // now that the server is running, publish a  DNS-SD announcement.
    SetThreadName("main");

    this->hotspotManager->Open();
}

void PiPedalModel::WaitForAudioDeviceToComeOnline()
{
    auto serverSettings = this->GetJackServerSettings();
    // Wait for selected audio device to be initialized.
    // It may take some time for ALSA to publish all available devices when rebooting.

    if (serverSettings.IsValid())
    {
        // wait up to 15 seconds for the midi device to come online.
        auto devices = GetAlsaDevices();
        bool found = false;
        if (HasAlsaDevice(devices, serverSettings.GetAlsaInputDevice()))
        {
            Lv2Log::info(SS("Found ALSA device " << serverSettings.GetAlsaInputDevice() << "."));
        }
        else
        {
            Lv2Log::info(SS("Waiting for ALSA device " << serverSettings.GetAlsaInputDevice() << "."));
            for (int i = 0; i < 5; ++i)
            {
                sleep(2);
                devices = GetAlsaDevices();
                if (HasAlsaDevice(devices, serverSettings.GetAlsaInputDevice()))
                {
                    found = true;
                    break;
                }
            }
            if (found)
            {
                Lv2Log::info(SS("Found ALSA device " << serverSettings.GetAlsaInputDevice() << "."));
            }
            else
            {
                Lv2Log::info(SS("ALSA device " << serverSettings.GetAlsaInputDevice() << " not found."));
            }
        }
    }
    else
    {
        Lv2Log::info("No ALSA device selected.");
    }

    // pre-cache device info before we let audio services run.
    GetAlsaDevices();
}

PiPedalModel::PostHandle PiPedalModel::Post(PostCallback&&fn)
{
    // I know. odd place to forward this to, but it's a very serviceable dispatcher implementation.
    // Why? because it's there, and PiPedalModel has no thread of its own to do dispatching.
    if (!hotspotManager)
    {
        throw std::runtime_error("Too early. It's not ready yet.");
    }
    return hotspotManager->Post(std::move(fn));
}
PiPedalModel::PostHandle PiPedalModel::PostDelayed(const clock::duration&delay,PostCallback&&fn)
{
    if (!hotspotManager)
    {
        throw std::runtime_error("Too early. It's not ready yet.");
    }
    return hotspotManager->PostDelayed(delay,std::move(fn));

}
bool PiPedalModel::CancelPost(PostHandle handle) {
    if (!hotspotManager)
    {
        throw std::runtime_error("Too early. It's not ready yet.");
    }
    return hotspotManager->CancelPost(handle);
}

void PiPedalModel::CancelNetworkChangingTimer()
{
    if (networkChangingDelayHandle)
    {
        CancelPost(networkChangingDelayHandle);
        networkChangingDelayHandle = 0;
    }
}
        

std::vector<std::string> PiPedalModel::GetKnownWifiNetworks()
{
    if (!this->hotspotManager)
    {
        return std::vector<std::string>();
    }
    return this->hotspotManager->GetKnownWifiNetworks();
}

void PiPedalModel::OnNetworkChanging(bool ethernetConnected,bool hotspotConnected)
{
    CancelNetworkChangingTimer();
    this->networkChangingDelayHandle = 
        PostDelayed(std::chrono::seconds(10), // takes a while for network configuration to be fully applied.
        [this,ethernetConnected,hotspotConnected]() {
            this->networkChangingDelayHandle = 0;
            OnNetworkChanged(ethernetConnected,hotspotConnected);
        }
        );

    // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
    std::vector<IPiPedalModelSubscriber *> t;
    t.reserve(this->subscribers.size());

    for (size_t i = 0; i < subscribers.size(); ++i)
    {
        t.push_back(this->subscribers[i]);
    }
    for (size_t i = 0; i < t.size(); ++i)
    {
        t[i]->OnNetworkChanging(hotspotConnected);
    }

}
void PiPedalModel::OnNetworkChanged(bool ethernetConnected, bool hotspotConnected)
{
    FireNetworkChanged();
}

void PiPedalModel::OnNotifyMidiRealtimeEvent(RealtimeMidiEventType eventType) 
{
    try {
        switch (eventType)
        {
            case RealtimeMidiEventType::Shutdown:
                {
                    this->RequestShutdown(false);
                }
                break;
            case RealtimeMidiEventType::Reboot:
                {
                    this->RequestShutdown(true);
                }
                break;
            case RealtimeMidiEventType::StartHotspot:
            {
                WifiConfigSettings settings = storage.GetWifiConfigSettings();
                if (!settings.hasSavedPassword_)
                {
                    throw std::runtime_error("Can't start Wi-Fi hotspot because no password has been configured.");
                }
                settings.autoStartMode_ = (uint16_t)HotspotAutoStartMode::Always;
                this->SetWifiConfigSettings(settings);
            }
            break;
            case RealtimeMidiEventType::StopHotspot:
            {
                WifiConfigSettings settings = storage.GetWifiConfigSettings();
                settings.autoStartMode_ = (uint16_t)HotspotAutoStartMode::Never;
                this->SetWifiConfigSettings(settings);
            }
            break;

            default:
                break;
        }
    } catch (const std::exception&e)
    {
        Lv2Log::error(SS("Failed to process realtime MIDI event. " << e.what()));
    }
}

void PiPedalModel::RequestShutdown(bool restart)
{
    if (GetAdminClient().CanUseAdminClient())
    {
        GetAdminClient().RequestShutdown(restart);
    }
    else
    {
        // ONLY works when interactively logged in.
        std::stringstream s;
        s << "/usr/sbin/shutdown ";
        if (restart)
        {
            s << "-r";
        }
        else
        {
            s << "-P";
        }
        s << " now";

        if (sysExec(s.str().c_str()) != EXIT_SUCCESS)
        {
            Lv2Log::error("shutdown failed.");
            if (restart)
            {
                throw new PiPedalStateException("Restart request failed.");
            }
            else
            {
                throw new PiPedalStateException("Shutdown request failed.");
            }
        }
    }
}
