// Copyright 2014 GSI, Inc. All rights reserved.
//
//
//

// STD
#include <iostream>
// BOOST
#include <boost/asio.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
// DDS
#include "AgentConnectionManager.h"
#include "CommanderChannel.h"
#include "Logger.h"
#include "MonitoringThread.h"
// API
//#include <signal.h>

using namespace boost::asio;
using namespace std;
using namespace dds;
using namespace MiscCommon;
namespace sp = std::placeholders;

CAgentConnectionManager::CAgentConnectionManager(const SOptions_t& _options, boost::asio::io_service& _service)
    : m_service(_service)
    , m_signals(_service)
    , m_options(_options)
    , m_agents()
{
    // Register to handle the signals that indicate when the server should exit.
    // It is safe to register for the same signal multiple times in a program,
    // provided all registration for the specified signal is made through Asio.
    m_signals.add(SIGINT);
    m_signals.add(SIGTERM);
#if defined(SIGQUIT)
    m_signals.add(SIGQUIT);
#endif // defined(SIGQUIT)

    doAwaitStop();
}

CAgentConnectionManager::~CAgentConnectionManager()
{
}

void CAgentConnectionManager::doAwaitStop()
{
    m_signals.async_wait([this](boost::system::error_code /*ec*/, int /*signo*/)
                         {
                             // TODO: terminate user process, if any

                             // Stop transport engine
                             stop();
                         });
}

void CAgentConnectionManager::start()
{
    try
    {
        const float maxIdleTime = CUserDefaults::instance().getOptions().m_server.m_idleTime;

        CMonitoringThread::instance().start(maxIdleTime,
                                            []()
                                            {
            LOG(info) << "Idle callback called";
        });

        // Read server info file
        const string sSrvCfg(CUserDefaults::instance().getServerInfoFileLocation());
        LOG(info) << "Reading server info from: " << sSrvCfg;
        if (sSrvCfg.empty())
            throw runtime_error("Cannot find server info file.");

        boost::property_tree::ptree pt;
        boost::property_tree::ini_parser::read_ini(sSrvCfg, pt);
        const string sHost(pt.get<string>("server.host"));
        const string sPort(pt.get<string>("server.port"));

        LOG(info) << "Contacting DDS commander on " << sHost << ":" << sPort;

        // Resolve endpoint iterator from host and port
        boost::asio::ip::tcp::resolver resolver(m_service);
        boost::asio::ip::tcp::resolver::query query(sHost, sPort);
        boost::asio::ip::tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);

        // Create new agent and push handshake message
        CCommanderChannel::connectionPtr_t newAgent = CCommanderChannel::makeNew(m_service);
        // Call this callback when a user process is activated
        newAgent->registerOnNewUserTaskCallback([this](pid_t _pid)
                                                {
                                                    return this->onNewUserTask(_pid);
                                                });

        boost::asio::async_connect(newAgent->socket(),
                                   endpoint_iterator,
                                   [this, &newAgent](boost::system::error_code ec, ip::tcp::resolver::iterator)
                                   {
            if (!ec)
            {
                // Create handshake message which is the first one for all agents
                SVersionCmd ver;
                CProtocolMessage::protocolMessagePtr_t msg = make_shared<CProtocolMessage>();
                msg->encodeWithAttachment<cmdHANDSHAKE_AGENT>(ver);

                newAgent->pushMsg(msg);
                newAgent->start();
            }
            else
            {
                LOG(fatal) << "Cannot connect to server: " << ec.message();
            }
        });

        m_service.run();
    }
    catch (exception& e)
    {
        LOG(fatal) << e.what();
    }
}

void CAgentConnectionManager::stop()
{
    try
    {
        m_service.stop();
        for (const auto& v : m_agents)
        {
            v->stop();
        }
        m_agents.clear();
    }
    catch (exception& e)
    {
        LOG(fatal) << e.what();
    }
}

void CAgentConnectionManager::onNewUserTask(pid_t _pid)
{
    LOG(info) << "Starting a watchdog for user task pid = " << _pid;

    // Register the user task's watchdog
    CMonitoringThread::instance().registerCallbackFunction([this, _pid]() -> bool
                                                           {
        if (!IsProcessExist(_pid))
        {
            LOG(info) << "User Tasks cannot be found. Probably it has exited. pid = " << _pid;
            LOG(info) << "Stopping the watchdog for user task pid = " << _pid;
            return false;
        }

        // We must call "wait" to check exist status of a child process, otherwise we will crate a zombie :)
        int status;
        if (_pid == ::waitpid(_pid, &status, WNOHANG))
        {
            if (WIFEXITED(status))
                LOG(info) << "User task exited" << (WCOREDUMP(status) ? " and dumped core" : "") << " with status "
                          << WEXITSTATUS(status);
            if (WIFSTOPPED(status))
                LOG(info) << "User task stopped by signal " << WSTOPSIG(status);

            LOG(info) << "Stopping the watchdog for user task pid = " << _pid;
            return false;
        }

        return true;
    });
}
