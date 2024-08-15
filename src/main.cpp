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

#include "pch.h"
#include "util.hpp"

#include "AudioConfig.hpp"
#include "WebServer.hpp"
#include <iostream>
#include "Lv2Log.hpp"
#include "ServiceConfiguration.hpp"
#include "AvahiService.hpp"

#include "PiPedalSocket.hpp"
#include "PluginHost.hpp"
#include <boost/system/error_code.hpp>
#include <filesystem>
#include "PiPedalConfiguration.hpp"
#include "AdminClient.hpp"
#include "CommandLineParser.hpp"
#include "Lv2SystemdLogger.hpp"
#include <sys/stat.h>
#include <boost/asio.hpp>
#include "HtmlHelper.hpp"
#include "Ipv6Helpers.hpp"
#include <thread>
#include <atomic>

#include <signal.h>
#include <semaphore.h>

#include <systemd/sd-daemon.h>

using namespace pipedal;

#define PRESET_EXTENSION ".piPreset"
#define BANK_EXTENSION ".piBank"
#define PLUGIN_PRESETS_EXTENSION ".piPluginPresets"


#ifdef __ARM_ARCH_ISA_A64
#define AARCH64
#endif


sem_t signalSemaphore;

bool HasAlsaDevice(const std::vector<AlsaDeviceInfo> devices, const std::string &deviceId)
{
    for (auto &device : devices)
    {
        if (device.id_ == deviceId)
            return true;
    }
    return false;
}

class application_category : public boost::system::error_category
{
public:
    const char *name() const noexcept { return "PiPedal"; }
    std::string message(int ev) const { return "error message"; }
};

static volatile bool g_SigBreak = false;
std::atomic<bool> ga_SigBreak { false };

void sig_handler(int signo)
{
    if (!g_SigBreak)
    {
        g_SigBreak = true;
        sem_post(&signalSemaphore);
    }
}

void throwSystemError(int error)
{
    
}
using namespace boost::system;

class DownloadIntercept : public RequestHandler
{
    PiPedalModel *model;

public:
    DownloadIntercept(PiPedalModel *model)
        : RequestHandler("/var"),
          model(model)
    {
    }

    virtual bool wants(const std::string &method, const uri &request_uri) const
    {
        if (request_uri.segment_count() != 2 || request_uri.segment(0) != "var")
        {
            return false;
        }
        std::string segment = request_uri.segment(1);
        if (segment == "uploadPluginPresets")
        {
            return true;
        }
        if (segment == "downloadPluginPresets")
        {
            return true;
        }
        if (segment == "downloadPreset")
        {
            std::string strInstanceId = request_uri.query("id");
            if (strInstanceId != "")
                return true;
        }
        else if (segment == "uploadPreset")
        {
            return true;
        }
        if (segment == "downloadBank")
        {
            std::string strInstanceId = request_uri.query("id");
            if (strInstanceId != "")
                return true;
        }
        else if (segment == "uploadBank")
        {
            return true;
        } else if (segment == "uploadUserFile")
        {
            return true;
        }
        return false;
    }

    std::string GetContentDispositionHeader(const std::string &name, const std::string &extension)
    {
        std::string fileName = name.substr(0, 64) + extension;
        std::stringstream s;

        s << "attachment; filename*=" << HtmlHelper::Rfc5987EncodeFileName(fileName) << "; filename=\"" << HtmlHelper::SafeFileName(fileName) << "\"";
        std::string result = s.str();
        return result;
    }

    void GetPluginPresets(const uri &request_uri, std::string *pName, std::string *pContent)
    {
        std::string pluginUri = request_uri.query("id");
        auto plugin = model->GetLv2Host().GetPluginInfo(pluginUri);
        *pName = plugin->name();

        PluginPresets pluginPresets = model->GetPluginPresets(pluginUri);

        std::stringstream s;
        json_writer writer(s,false);
        writer.write(pluginPresets);
        *pContent = s.str();
    }
    void GetPreset(const uri &request_uri, std::string *pName, std::string *pContent)
    {
        std::string strInstanceId = request_uri.query("id");
        int64_t instanceId = std::stol(strInstanceId);
        auto pedalboard = model->GetPreset(instanceId);

        // a certain elegance to using same file format for banks and presets.
        BankFile file;
        file.name(pedalboard.name());
        int64_t newInstanceId = file.addPreset(pedalboard);
        file.selectedPreset(newInstanceId);

        std::stringstream s;
        json_writer writer(s,false);
        writer.write(file);
        *pContent = s.str();
        *pName = pedalboard.name();
    }
    void GetBank(const uri &request_uri, std::string *pName, std::string *pContent)
    {
        std::string strInstanceId = request_uri.query("id");
        int64_t instanceId = std::stol(strInstanceId);
        BankFile bank;
        model->GetBank(instanceId, &bank);

        std::stringstream s;
        json_writer writer(s, true); // do what we can to reduce the file size.
        writer.write(bank);
        *pContent = s.str();
        *pName = bank.name();
    }

    virtual void head_response(
        const uri &request_uri,
        HttpRequest &req,
        HttpResponse &res,
        std::error_code &ec) override
    {
        try
        {
            std::string segment = request_uri.segment(1);
            if (segment == "downloadPluginPresets")
            {
                std::string name;
                std::string content;
                GetPluginPresets(request_uri, &name, &content);
                res.set(HttpField::content_type, "application/octet-stream");
                res.set(HttpField::cache_control, "no-cache");
                res.setContentLength(content.length());
                res.set(HttpField::content_disposition, GetContentDispositionHeader(name, PLUGIN_PRESETS_EXTENSION));
                return;
            }
            if (segment == "downloadPreset")
            {
                std::string name;
                std::string content;
                GetPreset(request_uri, &name, &content);

                res.set(HttpField::content_type, "application/octet-stream");
                res.set(HttpField::cache_control, "no-cache");
                res.setContentLength(content.length());
                res.set(HttpField::content_disposition, GetContentDispositionHeader(name, PRESET_EXTENSION));
                return;
            }
            if (segment == "downloadBank")
            {
                std::string name;
                std::string content;
                GetBank(request_uri, &name, &content);

                res.set(HttpField::content_type, "application/octet-stream");
                res.set(HttpField::cache_control, "no-cache");
                res.set(HttpField::content_disposition, GetContentDispositionHeader(name, BANK_EXTENSION));
                res.setContentLength(content.length());
                return;
            }
            throw PiPedalException("Not found.");
        }
        catch (const std::exception &e)
        {
            if (strcmp(e.what(), "Not found") == 0)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
            }
            else
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
            }
        }
    }

    virtual void get_response(
        const uri &request_uri,
        HttpRequest &req,
        HttpResponse &res,
        std::error_code &ec) override
    {
        try
        {
            std::string segment = request_uri.segment(1);

            if (segment == "downloadPluginPresets")
            {
                std::string name;
                std::string content;
                GetPluginPresets(request_uri, &name, &content);
                res.set(HttpField::content_type, "application/octet-stream");
                res.set(HttpField::cache_control, "no-cache");
                res.setContentLength(content.length());
                res.set(HttpField::content_disposition, GetContentDispositionHeader(name, PLUGIN_PRESETS_EXTENSION));
                res.setBody(content);
            }
            else if (segment == "downloadPreset")
            {
                std::string name;
                std::string content;
                GetPreset(request_uri, &name, &content);

                res.set(HttpField::content_type, "application/octet-stream");
                res.set(HttpField::cache_control, "no-cache");
                res.setContentLength(content.length());
                res.set(HttpField::content_disposition, GetContentDispositionHeader(name, PRESET_EXTENSION));
                res.setBody(content);
            }
            else if (segment == "downloadBank")
            {
                std::string name;
                std::string content;
                GetBank(request_uri, &name, &content);

                res.set(HttpField::content_type, "application/octet-stream");
                res.set(HttpField::cache_control, "no-cache");
                res.setContentLength(content.length());
                res.set(HttpField::content_disposition, GetContentDispositionHeader(name, BANK_EXTENSION));
                res.setBody(content);
            }
            else
            {
                throw PiPedalException("Not found");
            }
        }
        catch (const std::exception &e)
        {
            if (strcmp(e.what(), "Not found") == 0)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
            }
            else
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
            }
        }
    }
    virtual void post_response(
        const uri &request_uri,
        HttpRequest &req,
        HttpResponse &res,
        std::error_code &ec) override
    {
        try
        {
            std::string segment = request_uri.segment(1);

            if (segment == "uploadPluginPresets")
            {
                json_reader reader(req.get_body_input_stream());
                PluginPresets presets;
                reader.read(&presets);
                model->UploadPluginPresets(presets);

                res.set(HttpField::content_type, "application/json");
                res.set(HttpField::cache_control, "no-cache");
                std::stringstream sResult;
                sResult << -1;
                std::string result = sResult.str();
                res.setContentLength(result.length());

                res.setBody(result);
            }
            else if (segment == "uploadPreset")
            {
                json_reader reader(req.get_body_input_stream());

                uint64_t uploadAfter = -1;
                std::string strUploadAfter = request_uri.query("uploadAfter");
                if (strUploadAfter.length() != 0)
                {
                    uploadAfter = std::stol(strUploadAfter);
                }

                BankFile bankFile;
                reader.read(&bankFile);

                uint64_t instanceId = model->UploadPreset(bankFile, uploadAfter);

                res.set(HttpField::content_type, "application/json");
                res.set(HttpField::cache_control, "no-cache");
                std::stringstream sResult;
                sResult << instanceId;
                std::string result = sResult.str();
                res.setContentLength(result.length());

                res.setBody(result);
            }
            else if (segment == "uploadBank")
            {
                json_reader reader(req.get_body_input_stream());

                uint64_t uploadAfter = -1;
                std::string strUploadAfter = request_uri.query("uploadAfter");
                if (strUploadAfter.length() != 0)
                {
                    uploadAfter = std::stol(strUploadAfter);
                }

                BankFile bankFile;
                reader.read(&bankFile);

                uint64_t instanceId = model->UploadBank(bankFile, uploadAfter);

                res.set(HttpField::content_type, "application/json");
                res.set(HttpField::cache_control, "no-cache");
                std::stringstream sResult;
                sResult << instanceId;
                std::string result = sResult.str();
                res.setContentLength(result.length());

                res.setBody(result);
            } else if (segment == "uploadUserFile")
            {
                res.set(HttpField::content_type, "application/json");
                res.set(HttpField::cache_control, "no-cache");

                const std::string &directory = request_uri.query("directory");
                const std::string &filename = request_uri.query("filename");
                const std::string &patchProperty = request_uri.query("property");


                if (patchProperty.length() == 0 && directory.length() == 0)
                {
                    throw PiPedalException("Malformed request.");

                }

                res.set(HttpField::content_type, "application/json");
                res.set(HttpField::cache_control, "no-cache");

                std::string outputFileName = std::filesystem::path(directory) / filename;

                std::string path = this->model->UploadUserFile(directory,patchProperty,filename,req.get_body_input_stream(), req.content_length());

                std::stringstream ss;
                json_writer writer(ss);
                writer.write(outputFileName);
                std::string response = ss.str();

                res.setContentLength(response.length());
                res.setBody(response);
            }
            else
            {
                throw PiPedalException("Not found");
            }
        }
        catch (const std::exception &e)
        {
            if (strcmp(e.what(), "Not found") == 0)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
            }
            else
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
            }
        }
    }
};

/* When hosting a react app, replace /var/config.json with
   data that will connect the react app with our socket server.
*/

class InterceptConfig : public RequestHandler
{
private:
    uint64_t maxUploadSize;
    int portNumber;

public:
    InterceptConfig(int portNumber, uint64_t maxUploadSize)
        : RequestHandler("/var/config.json"),
          maxUploadSize(maxUploadSize),
          portNumber(portNumber)
    {
    }
    std::string GetConfig(const std::string &fromAddress)
    {
        #define LINK_LOCAL_WEB_SOCKET 1
        #if LINK_LOCAL_WEB_SOCKET 
        std::string webSocketAddress = GetLinkLocalAddress(fromAddress);
        Lv2Log::info(SS("Web Socket Address: " << webSocketAddress << ":" << portNumber));
        #else
        std::string webSocketAddress = "*";
        #endif

        std::stringstream s;

        s << "{ \"socket_server_port\": " << portNumber
          << ", \"socket_server_address\": \"" << webSocketAddress << "\", \"ui_plugins\": [ ], \"max_upload_size\": " << maxUploadSize << " }";

        return s.str();
    }
    virtual ~InterceptConfig() {}

    virtual void head_response(
        const uri &request_uri,
        HttpRequest &req,
        HttpResponse &res,
        std::error_code &ec) override
    {
        // intercepted. See the other overload.
    }

    virtual void head_response(
        const std::string &fromAddress,
        const uri &request_uri,
        HttpRequest &req,
        HttpResponse &res,
        std::error_code &ec) override
    {
        std::string response = GetConfig(fromAddress);
        res.set(HttpField::content_type, "application/json");
        res.set(HttpField::cache_control, "no-cache");
        res.setContentLength(response.length());
        return;
    }

    virtual void get_response(
        const uri &request_uri,
        HttpRequest &req,
        HttpResponse &res,
        std::error_code &ec) override
    {
        // intercepted. see the other overload.
    }

    virtual void get_response(
        const std::string &fromAddress,
        const uri &request_uri,
        HttpRequest &req,
        HttpResponse &res,
        std::error_code &ec) override
    {
        std::string response = GetConfig(fromAddress);
        res.set(HttpField::content_type, "application/json");
        res.set(HttpField::cache_control, "no-cache");
        res.setContentLength(response.length());
        res.setBody(response);
    }
};

static bool isJackServiceRunning()
{
    // look for the jack shmem .
    std::filesystem::path path = "/dev/shm/jack_default_0";
    return std::filesystem::exists(path);
}

int main(int argc, char *argv[])
{

    sem_init(&signalSemaphore, 0, 0);


#ifndef WIN32
    umask(002); // newly created files in /var/pipedal get 775-ish permissions, which improves debugging/live-service interaction.
#endif

    // Check command line arguments.
    bool help = false;
    bool error = false;
    bool systemd = false;
    bool testExtraDevice = false;
    std::string portOption;

    CommandLineParser parser;
    parser.AddOption("-h", &help);
    parser.AddOption("--help", &help);
    parser.AddOption("-systemd", &systemd);
    parser.AddOption("-port", &portOption);
    parser.AddOption("-test-extra-device", &testExtraDevice); // advertise two different devices (for testing multi-device connect)

    try
    {
        parser.Parse(argc, (const char **)argv);
        if (parser.Arguments().size() > 2)
        {
            throw PiPedalException("Too many arguments.");
        }
        if (parser.Arguments().size() == 0)
        {
            throw PiPedalException("<config_root> not provided.");
        }
        if (help || parser.Arguments().size() == 0)
        {
            std::cout << "pipedald - Pipedal web socket server.\n"
                         "Copyright (c) 2022 Robin Davies.\n"
                         "\n";
        }
    }
    catch (const std::exception &e)
    {
        std::cout << "Error: " << e.what() << "\n\n";
        error = true;
        help = true;
    }

    if (help)
    {
        std::cout << "Usage: pipedald <doc_root> [<web_root>] [options...]\n\n"
                  << "Options:\n"
                  << "   -systemd: Log to systemd journals instead of to the console.\n"
                  << "   -port: Port to listen on e.g. 0.0.0.0:80\n"
                  << "Example:\n"
                  << "    pipedald /etc/pipedal/config /etc/pipedal/react -port 0.0.0.0:80 \n"
                     "\n"
                     "Description:\n\n"
                     "    Configuration is read from <doc_root>/config.json\n"
                     "\n"
                     "    If <web_root> is not provided, pipedal will serve from <doc_root>\n"
                     "\n"
                     "    While debugging, bind the port to 0.0.0.0:8080, and connect to the default React\n"
                     "    server that's provided when you run 'npm run start' in 'react/src'. By default, the\n"
                     "    React debug server will connect to the socket server on 0.0.0.0:8080.\n\n";
        return error ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    if (systemd)
    {
        Lv2Log::set_logger(MakeLv2SystemdLogger());
    }
    SetThreadName("main");


    std::filesystem::path doc_root = parser.Arguments()[0];
    std::filesystem::path web_root = doc_root;
    if (parser.Arguments().size() >= 2)
    {
        web_root = parser.Arguments()[1];
    }

    PiPedalConfiguration configuration;
    try
    {
        configuration.Load(doc_root, web_root);
    }
    catch (const std::exception &e)
    {
        std::stringstream s;
        s << "Unable to read configuration from '" << (doc_root / "config.json") << "'. (" << e.what() << ")";
        Lv2Log::error(s.str());
        return EXIT_FAILURE;
    }

    Lv2Log::log_level(configuration.GetLogLevel());

    if (portOption.length() != 0)
    {
        configuration.SetSocketServerEndpoint(portOption);
    }

    uint16_t port;
    std::shared_ptr<WebServer> server;
    try
    {
        auto const address = boost::asio::ip::make_address(configuration.GetSocketServerAddress());
        port = static_cast<uint16_t>(configuration.GetSocketServerPort());

        auto const threads = std::max<int>(1, configuration.GetThreads());

        server = WebServer::create(
            address, port, web_root.c_str(), threads);

        Lv2Log::info("Document root: %s Threads: %d", doc_root.c_str(), (int)threads);

        server->SetLogHttpRequests(configuration.LogHttpRequests());
    }
    catch (const std::exception &e)
    {
        std::stringstream s;
        s << "Fatal error: " << e.what() << std::endl;
        Lv2Log::error(s.str());
        return EXIT_FAILURE;
    }

    try
    {
        PiPedalModel model;
        model.Init(configuration);

        // Get heavy IO out of the way before letting dependent (Jack/ALSA) services run.
        model.LoadLv2PluginInfo();
        if (systemd)
        {
            // Tell systemd we're done.
            sd_notifyf(0, "READY=1\n"
                          "MAINPID=%lu",
                       (unsigned long)getpid());
        }


        auto serverSettings = model.GetJackServerSettings();


        {
            // Wait for selected audio device to be initialized.
            // It may take some time for ALSA to publish all available devices when rebooting.

            if (serverSettings.IsValid())
            {
                // wait up to 15 seconds for the midi device to come online.
                auto devices = model.GetAlsaDevices();
                bool found = false;
                if (HasAlsaDevice(devices, serverSettings.GetAlsaInputDevice()))
                {
                    Lv2Log::info(SS("Found ALSA device " << serverSettings.GetAlsaInputDevice() << "."));
                } else 
                {
                    for (int i = 0; i < 5; ++i)
                    {
                        sleep(3);
                        devices = model.GetAlsaDevices();
                        if (HasAlsaDevice(devices, serverSettings.GetAlsaInputDevice()))
                        {
                            found = true;
                            break;
                        }
                        if (g_SigBreak)
                            exit(1);
                        if (!systemd)
                        {
                            break;
                        }
                        Lv2Log::info(SS("Waiting for ALSA device " << serverSettings.GetAlsaInputDevice() << " to come online..."));
                    }
                    if (found)
                    {
                        Lv2Log::info(SS("Found ALSA device " << serverSettings.GetAlsaInputDevice() << "."));
                    } else {
                        Lv2Log::info(SS("ALSA device " << serverSettings.GetAlsaInputDevice() << " not found."));
                    }
                }
            } else {
                Lv2Log::info("No ALSA device selected.");

            }

            // pre-cache device info before we let audio services run.
            model.GetAlsaDevices();
        }

        // only accept signals on the main thread.
        int sig;
        sigset_t sigSet;
        int s;
        sigemptyset(&sigSet);
        sigaddset(&sigSet, SIGINT);
        sigaddset(&sigSet, SIGTERM);
        sigaddset(&sigSet, SIGUSR1);

        s = pthread_sigmask(SIG_BLOCK, &sigSet, NULL);
        if (s != 0)
        {
            throw std::logic_error("pthread_sigmask failed.");
        }

#if JACK_HOST
        if (systemd)
        {

            if (!isJackServiceRunning())
            {
                Lv2Log::info("Waiting for Jack service.");
                // wait up to 15 seconds for the jack service to come online.
                for (int i = 0; i < 15; ++i)
                {
                    // use the time to prepopulate ALSA device cache before jack
                    // opens the device and we can't read properties.
                    model.GetAlsaDevices();

                    sleep(1);
                    if (isJackServiceRunning())
                    {
                        break;
                    }
                }
            }
            if (isJackServiceRunning())
            {
                Lv2Log::info("Found  Jack service.");
                sleep(3); // jack needs a little time to get up to speed.
            }
            else
            {
                Lv2Log::info("Jack service not started.");
            }
            model.GetAlsaDevices();
        }
#endif

        model.Load();

        auto pipedalSocketFactory = MakePiPedalSocketFactory(model);

        server->AddSocketFactory(pipedalSocketFactory);

        std::shared_ptr<RequestHandler> interceptConfig{new InterceptConfig(port, configuration.GetMaxUploadSize())};
        server->AddRequestHandler(interceptConfig);

        std::shared_ptr<DownloadIntercept> downloadIntercept = std::make_shared<DownloadIntercept>(&model);
        server->AddRequestHandler(downloadIntercept);

        {
            server->RunInBackground(-1);

            SetThreadName("avahi");
            model.UpdateDnsSd(); // now that the server is running, publish a  DNS-SD announcement.
            SetThreadName("main");

            // AARCH64 sem_wait pins CPU 100%.
                // static_assert(std::atomic<bool>::is_always_lock_free);

                // while (true)
                // {
                //     auto sigBreak = ga_SigBreak.load();
                //     if (sigBreak)
                //     {
                //         break;
                //     }
                //     std::this_thread::sleep_for(std::chrono::milliseconds(50));

                // }


            {
                sigwait(&sigSet,&sig);
                
                if (systemd)
                {
                    sd_notify(0, "STOPPING=1");
                }
            }
        }

        Lv2Log::info("Closing audio session.");
        model.Close();
        
        Lv2Log::info("Stopping web server.");
        server->ShutDown(5000);
        server->Join();

        Lv2Log::info("Shutdown complete.");
    }
    catch (const std::exception &e)
    {
        Lv2Log::error(e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS; // only exit in response to a signal.
}
