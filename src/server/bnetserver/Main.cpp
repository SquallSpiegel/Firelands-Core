/*
 * This file is part of the FirelandsCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

 /**
 * @file main.cpp
 * @brief Authentication Server main program
 *
 * This file contains the main program for the
 * authentication server
 */

#include "AppenderDB.h"
#include "Banner.h"
#include "BNetRealmList.h"
#include "ComponentManager.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DatabaseLoader.h"
#include "DeadlineTimer.h"
#include "GitRevision.h"
#include "IoContext.h"
#include "MySQLThreading.h"
#include "ModuleManager.h"
#include "ProcessPriority.h"
#include "SessionManager.h"
#include "Util.h"
#include "ZmqContext.h"
#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem/operations.hpp>
#include <csignal>
#include <iostream>

#ifdef _WIN32 // ugly as hell
#pragma comment(lib, "iphlpapi.lib")
#endif

using boost::asio::ip::tcp;
using namespace boost::program_options;
namespace fs = boost::filesystem;

#ifndef _FIRELANDS_BNET_CONFIG
# define _FIRELANDS_BNET_CONFIG  "bnetserver.conf"
#endif

#if FIRELANDS_PLATFORM == FIRELANDS_PLATFORM_WINDOWS
#include "ServiceWin32.h"
char serviceName[] = "bnetserver";
char serviceLongName[] = "FirelandsCore bnet service";
char serviceDescription[] = "FirelandsCore Battle.net emulator authentication service";
/*
* -1 - not in service mode
*  0 - stopped
*  1 - running
*  2 - paused
*/
int m_ServiceStatus = -1;

void ServiceStatusWatcher(std::weak_ptr<Firelands::Asio::DeadlineTimer> serviceStatusWatchTimerRef, std::weak_ptr<Firelands::Asio::IoContext> ioContextRef, boost::system::error_code const& error);
#endif

bool StartDB();
void StopDB();
void SignalHandler(std::weak_ptr<Firelands::Asio::IoContext> ioContextRef, boost::system::error_code const& error, int signalNumber);
void KeepDatabaseAliveHandler(std::weak_ptr<Firelands::Asio::DeadlineTimer> dbPingTimerRef, int32 dbPingInterval, boost::system::error_code const& error);
variables_map GetConsoleArguments(int argc, char** argv, fs::path& configFile, std::string& configService);

int main(int argc, char** argv)
{
    signal(SIGABRT, &Firelands::AbortHandler);

    auto configFile = fs::absolute(_FIRELANDS_BNET_CONFIG);
    std::string configService;
    auto vm = GetConsoleArguments(argc, argv, configFile, configService);
    // exit if help is enabled
    if (vm.count("help") || vm.count("version"))
        return 0;

#if FIRELANDS_PLATFORM == FIRELANDS_PLATFORM_WINDOWS
    if (configService.compare("install") == 0)
        return WinServiceInstall() ? 0 : 1;
    else if (configService.compare("uninstall") == 0)
        return WinServiceUninstall() ? 0 : 1;
    else if (configService.compare("run") == 0)
        return WinServiceRun() ? 0 : 1;
#endif

    std::string configError;
    if (!sConfigMgr->LoadInitial(configFile.generic_string(),
        std::vector<std::string>(argv, argv + argc),
        configError))
    {
        printf("Error in config file: %s\n", configError.c_str());
        return 1;
    }

    sLog->RegisterAppender<AppenderDB>();
    sLog->Initialize(nullptr);

    Firelands::Banner::Show("bnetserver",
        [](char const* text)
        {
            FC_LOG_INFO("server.bnetserver", "%s", text);
        },
        []()
        {
            FC_LOG_INFO("server.bnetserver", "Using configuration file %s.", sConfigMgr->GetFilename().c_str());
            FC_LOG_INFO("server.bnetserver", "Using SSL version: %s (library: %s)", OPENSSL_VERSION_TEXT, SSLeay_version(SSLEAY_VERSION));
            FC_LOG_INFO("server.bnetserver", "Using Boost version: %i.%i.%i", BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);
        }
        );

    // Seed the OpenSSL's PRNG here.
    // That way it won't auto-seed when calling BigNumber::SetRand and slow down the first world login
    BigNumber seed;
    seed.SetRand(16 * 8);

    // bnetserver PID file creation
    std::string pidFile = sConfigMgr->GetStringDefault("PidFile", "");
    if (!pidFile.empty())
    {
        if (uint32 pid = CreatePIDFile(pidFile))
            FC_LOG_INFO("server.bnetserver", "Daemon PID: %u\n", pid);
        else
        {
            FC_LOG_ERROR("server.bnetserver", "Cannot create PID file %s.\n", pidFile.c_str());
            return 1;
        }
    }

    int32 worldListenPort = sConfigMgr->GetIntDefault("WorldserverListenPort", 1118);
    if (worldListenPort < 0 || worldListenPort > 0xFFFF)
    {
        FC_LOG_ERROR("server.bnetserver", "Specified worldserver listen port (%d) out of allowed range (1-65535)", worldListenPort);
        return 1;
    }

    // Initialize the database connection
    if (!StartDB())
        return 1;

    std::shared_ptr<void> dbHandle(nullptr, [](void*) { StopDB(); });

    sIpcContext->Initialize();
    std::shared_ptr<void> ipcHandle(nullptr, [](void*) { sIpcContext->Close(); });

    std::shared_ptr<Firelands::Asio::IoContext> ioContext = std::make_shared<Firelands::Asio::IoContext>();

    // Start the listening port (acceptor) for auth connections
    int32 bnport = sConfigMgr->GetIntDefault("BattlenetPort", 1119);
    if (bnport < 0 || bnport > 0xFFFF)
    {
        FC_LOG_ERROR("server.bnetserver", "Specified battle.net port (%d) out of allowed range (1-65535)", bnport);
        return 1;
    }

    // Get the list of realms for the server
    sBNetRealmList->Initialize(*ioContext, sConfigMgr->GetIntDefault("RealmsStateUpdateDelay", 10), worldListenPort);

    std::shared_ptr<void> sBNetRealmListHandle(nullptr, [](void*) { sBNetRealmList->Close(); });

    std::string bindIp = sConfigMgr->GetStringDefault("BindIP", "0.0.0.0");

    if (!sSessionMgr.StartNetwork(*ioContext, bindIp, bnport))
    {
        FC_LOG_ERROR("server.bnetserver", "Failed to initialize network");
        return 1;
    }

    std::shared_ptr<void> sSessionMgrHandle(nullptr, [](void*) { sSessionMgr.StopNetwork(); });

    // Set signal handlers
    boost::asio::signal_set signals(*ioContext, SIGINT, SIGTERM);
#if FIRELANDS_PLATFORM == FIRELANDS_PLATFORM_WINDOWS
    signals.add(SIGBREAK);
#endif
    signals.async_wait(std::bind(&SignalHandler, std::weak_ptr<Firelands::Asio::IoContext>(ioContext), std::placeholders::_1, std::placeholders::_2));

    // Set process priority according to configuration settings
    SetProcessPriority("server.bnetserver", sConfigMgr->GetIntDefault(CONFIG_PROCESSOR_AFFINITY, 0), sConfigMgr->GetBoolDefault(CONFIG_HIGH_PRIORITY, false));

    // Enabled a timed callback for handling the database keep alive ping
    int32 dbPingInterval = sConfigMgr->GetIntDefault("MaxPingTime", 30);
    std::shared_ptr<Firelands::Asio::DeadlineTimer> dbPingTimer = std::make_shared<Firelands::Asio::DeadlineTimer>(*ioContext);
    dbPingTimer->expires_from_now(boost::posix_time::minutes(dbPingInterval));
    dbPingTimer->async_wait(std::bind(&KeepDatabaseAliveHandler, std::weak_ptr<Firelands::Asio::DeadlineTimer>(dbPingTimer), dbPingInterval, std::placeholders::_1));

#if FIRELANDS_PLATFORM == FIRELANDS_PLATFORM_WINDOWS
    std::shared_ptr<Firelands::Asio::DeadlineTimer> serviceStatusWatchTimer;
    if (m_ServiceStatus != -1)
    {
        serviceStatusWatchTimer = std::make_shared<Firelands::Asio::DeadlineTimer>(*ioContext);
        serviceStatusWatchTimer->expires_from_now(boost::posix_time::seconds(1));
        serviceStatusWatchTimer->async_wait(std::bind(&ServiceStatusWatcher,
            std::weak_ptr<Firelands::Asio::DeadlineTimer>(serviceStatusWatchTimer),
            std::weak_ptr<Firelands::Asio::IoContext>(ioContext),
            std::placeholders::_1));
    }
#endif

    sComponentMgr->Load();
    sModuleMgr->Load();

    // Start the io service worker loop
    ioContext->run();

    dbPingTimer->cancel();

    FC_LOG_INFO("server.bnetserver", "Halting process...");

    signals.cancel();

    return 0;
}


/// Initialize connection to the database
bool StartDB()
{
    MySQL::Library_Init();

    // Load databases
    DatabaseLoader loader("server.bnetserver", DatabaseLoader::DATABASE_NONE);
    loader
        .AddDatabase(LoginDatabase, "Login");

    if (!loader.Load())
        return false;

    FC_LOG_INFO("server.bnetserver", "Started auth database connection pool.");
    sLog->SetRealmId(0); // Enables DB appenders when realm is set.
    return true;
}

/// Close the connection to the database
void StopDB()
{
    LoginDatabase.Close();
    MySQL::Library_End();
}

void SignalHandler(std::weak_ptr<Firelands::Asio::IoContext> ioContextRef, boost::system::error_code const& error, int /*signalNumber*/)
{
    if (!error)
        if (std::shared_ptr<Firelands::Asio::IoContext> ioContext = ioContextRef.lock())
            ioContext->stop();
}

void KeepDatabaseAliveHandler(std::weak_ptr<Firelands::Asio::DeadlineTimer> dbPingTimerRef, int32 dbPingInterval, boost::system::error_code const& error)
{
    if (!error)
    {
        if (std::shared_ptr<Firelands::Asio::DeadlineTimer> dbPingTimer = dbPingTimerRef.lock())
        {
            FC_LOG_INFO("server.bnetserver", "Ping MySQL to keep connection alive");
            LoginDatabase.KeepAlive();

            dbPingTimer->expires_from_now(boost::posix_time::minutes(dbPingInterval));
            dbPingTimer->async_wait(std::bind(&KeepDatabaseAliveHandler, dbPingTimerRef, dbPingInterval, std::placeholders::_1));
        }
    }
}

#if FIRELANDS_PLATFORM == FIRELANDS_PLATFORM_WINDOWS
void ServiceStatusWatcher(std::weak_ptr<Firelands::Asio::DeadlineTimer> serviceStatusWatchTimerRef, std::weak_ptr<Firelands::Asio::IoContext> ioContextRef, boost::system::error_code const& error)
{
    if (!error)
    {
        if (std::shared_ptr<Firelands::Asio::IoContext> ioContext = ioContextRef.lock())
        {
            if (m_ServiceStatus == 0)
            {
                ioContext->stop();
            }
            else if (std::shared_ptr<Firelands::Asio::DeadlineTimer> serviceStatusWatchTimer = serviceStatusWatchTimerRef.lock())
            {
                serviceStatusWatchTimer->expires_from_now(boost::posix_time::seconds(1));
                serviceStatusWatchTimer->async_wait(std::bind(&ServiceStatusWatcher, serviceStatusWatchTimerRef, ioContext, std::placeholders::_1));
            }
        }
    }
}
#endif

variables_map GetConsoleArguments(int argc, char** argv, fs::path& configFile, std::string& configService)
{
    (void)configService;

    options_description all("Allowed options");
    all.add_options()
        ("help,h", "print usage message")
        ("version,v", "print version build info")
        ("config,c", value<fs::path>(&configFile)->default_value(fs::absolute(_FIRELANDS_BNET_CONFIG)),
            "use <arg> as configuration file")
        ;
#if FIRELANDS_PLATFORM == FIRELANDS_PLATFORM_WINDOWS
    options_description win("Windows platform specific options");
    win.add_options()
        ("service,s", value<std::string>(&configService)->default_value(""), "Windows service options: [install | uninstall]")
        ;

    all.add(win);
#endif
    variables_map variablesMap;
    try
    {
        store(command_line_parser(argc, argv).options(all).allow_unregistered().run(), variablesMap);
        notify(variablesMap);
    }
    catch (std::exception& e) {
        std::cerr << e.what() << "\n";
    }

    if (variablesMap.count("help")) {
        std::cout << all << "\n";
    }
    else if (variablesMap.count("version"))
    {
        std::cout << GitRevision::GetFullVersion() << "\n";
    }

    return variablesMap;
}
