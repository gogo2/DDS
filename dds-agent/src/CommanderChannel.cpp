// Copyright 2014 GSI, Inc. All rights reserved.
//
//
//

// DDS
#include "CommanderChannel.h"
#include "UserDefaults.h"
#include "version.h"
#include "BOOST_FILESYSTEM.h"
// MiscCommon
#include "FindCfgFile.h"
// BOOST
#include <boost/crc.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
// STD
#include <ctime>

using namespace MiscCommon;
using namespace dds;
using namespace std;
namespace fs = boost::filesystem;

CCommanderChannel::CCommanderChannel(boost::asio::io_service& _service)
    : CConnectionImpl<CCommanderChannel>(_service)
    , m_isHandShakeOK(false)
{
}

void CCommanderChannel::onHeaderRead()
{
    m_headerReadTime = std::chrono::steady_clock::now();
}

bool CCommanderChannel::on_cmdREPLY_HANDSHAKE_OK(CProtocolMessage::protocolMessagePtr_t _msg)
{
    m_isHandShakeOK = true;

    return true;
}

bool CCommanderChannel::on_cmdSIMPLE_MSG(CProtocolMessage::protocolMessagePtr_t _msg)
{
    return true;
}

bool CCommanderChannel::on_cmdGET_HOST_INFO(CProtocolMessage::protocolMessagePtr_t _msg)
{
    pid_t pid = getpid();

    SHostInfoCmd cmd;
    get_cuser_name(&cmd.m_username);
    get_hostname(&cmd.m_host);
    cmd.m_version = PROJECT_VERSION_STRING;
    cmd.m_DDSPath = CUserDefaults::getDDSPath();
    cmd.m_agentPort = 0;
    cmd.m_agentPid = pid;
    cmd.m_timeStamp = 0;

    CProtocolMessage::protocolMessagePtr_t msg = make_shared<CProtocolMessage>();
    msg->encodeWithAttachment<cmdREPLY_HOST_INFO>(cmd);
    pushMsg(msg);

    return true;
}

bool CCommanderChannel::on_cmdDISCONNECT(CProtocolMessage::protocolMessagePtr_t _msg)
{
    stop();
    LOG(info) << "The Agent [" << m_id << "] disconnected... Bye";

    return true;
}

bool CCommanderChannel::on_cmdSHUTDOWN(CProtocolMessage::protocolMessagePtr_t _msg)
{
    stop();
    deleteAgentUUIDFile();
    LOG(info) << "The Agent [" << m_id << "] exited.";
    exit(EXIT_SUCCESS);

    return true;
}

bool CCommanderChannel::on_cmdBINARY_ATTACHMENT(CProtocolMessage::protocolMessagePtr_t _msg)
{
    SBinaryAttachmentCmd cmd;
    cmd.convertFromData(_msg->bodyToContainer());

    chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    chrono::microseconds downloadTime = chrono::duration_cast<chrono::microseconds>(now - m_headerReadTime);

    // Calculate CRC32 of the recieved file data
    boost::crc_32_type crc32;
    crc32.process_bytes(&cmd.m_fileData[0], cmd.m_fileData.size());

    if (crc32.checksum() == cmd.m_crc32)
    {
        // Do something if file is correctly downloaded
    }

    // Form reply command
    SBinaryDownloadStatCmd reply_cmd;
    reply_cmd.m_recievedCrc32 = crc32.checksum();
    reply_cmd.m_recievedFileSize = cmd.m_fileData.size();
    reply_cmd.m_downloadTime = downloadTime.count();

    CProtocolMessage::protocolMessagePtr_t msg = make_shared<CProtocolMessage>();
    msg->encodeWithAttachment<cmdBINARY_DOWNLOAD_STAT>(reply_cmd);
    pushMsg(msg);

    return true;
}

bool CCommanderChannel::on_cmdGET_UUID(CProtocolMessage::protocolMessagePtr_t _msg)
{
    // If file exist return uuid from file.
    // If file does not exist than return uuid_nil.

    const string sAgentUUIDFile(CUserDefaults::instance().getAgentUUIDFile());
    if (MiscCommon::file_exists(sAgentUUIDFile))
    {
        readAgentUUIDFile();
    }
    else
    {
        m_id = boost::uuids::nil_uuid();
    }

    SUUIDCmd msg_cmd;
    msg_cmd.m_id = m_id;
    CProtocolMessage::protocolMessagePtr_t msg = make_shared<CProtocolMessage>();
    msg->encodeWithAttachment<cmdREPLY_UUID>(msg_cmd);
    pushMsg(msg);

    return true;
}

bool CCommanderChannel::on_cmdSET_UUID(CProtocolMessage::protocolMessagePtr_t _msg)
{
    SUUIDCmd cmd;
    cmd.convertFromData(_msg->bodyToContainer());

    LOG(info) << "cmdSET_UUID attachment [" << cmd << "] from " << remoteEndIDString();

    m_id = cmd.m_id;

    createAgentUUIDFile();

    return true;
}

bool CCommanderChannel::on_cmdGET_LOG(CProtocolMessage::protocolMessagePtr_t _msg)
{
    try
    {
        string logDir(CUserDefaults::getDDSPath());

        string hostname;
        get_hostname(&hostname);

        std::time_t now = chrono::system_clock::to_time_t(chrono::system_clock::now());
        struct std::tm* ptm = std::localtime(&now);
        char buffer[20];
        std::strftime(buffer, 20, "%Y-%m-%d-%H-%M-%S", ptm);

        stringstream ss;
        // We do not use put_time for the moment as gcc4.9 does not support it.
        // ss << std::put_time(ptm, "%Y-%m-%d-%H-%M-%S") << "_" << hostname << "_" << m_id;
        ss << buffer << "_" << hostname << "_" << m_id;

        string archiveName(ss.str());

        fs::path archiveDir(logDir + archiveName);
        if (!fs::exists(archiveDir) && !fs::create_directory(archiveDir))
        {
            string msg("Could not create directory: " + archiveDir.string());
            sendGetLogError(msg);
            return true;
        }

        vector<fs::path> logFiles;
        BOOSTHelper::get_files_by_extension(logDir, ".log", logFiles);

        for (const auto& v : logFiles)
        {
            fs::path dest(archiveDir.string() + "/" + v.filename().string());
            fs::copy_file(v, dest, fs::copy_option::overwrite_if_exists);
        }

        CFindCfgFile<string> cfg;
        cfg.SetOrder("/usr/bin/tar")("/usr/local/bin/tar")("/opt/local/bin/tar")("/bin/tar");
        string tarPath;
        cfg.GetCfg(&tarPath);

        string archiveDirName = logDir + archiveName;
        string archiveFileName = archiveDirName + ".tar.gz";
        StringVector_t params{ "czf", archiveFileName, "-C", logDir, archiveName };
        string output;

        do_execv(tarPath, params, 60, &output);

        SBinaryAttachmentCmd cmd;

        ifstream f(archiveFileName.c_str());
        if (!f.is_open() || !f.good())
        {
            string msg("Could not open archive with log files: " + archiveFileName);
            sendGetLogError(msg);
            return true;
        }
        f.seekg(0, ios::end);
        cmd.m_fileData.reserve(f.tellg());
        f.seekg(0, ios::beg);
        cmd.m_fileData.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        boost::crc_32_type crc;
        crc.process_bytes(&cmd.m_fileData[0], cmd.m_fileData.size());

        cmd.m_crc32 = crc.checksum();
        cmd.m_fileName = archiveName + ".tar.gz";
        cmd.m_fileSize = cmd.m_fileData.size();

        f.close();
        fs::remove(archiveFileName);
        fs::remove_all(archiveDirName);

        CProtocolMessage::protocolMessagePtr_t msg = make_shared<CProtocolMessage>();
        msg->encodeWithAttachment<cmdBINARY_ATTACHMENT_LOG>(cmd);
        pushMsg(msg);
    }
    catch (exception& e)
    {
        LOG(error) << e.what();
        sendGetLogError(e.what());
    }

    return true;
}

void CCommanderChannel::sendGetLogError(const string& _msg)
{
    SSimpleMsgCmd cmd;
    cmd.m_sMsg = _msg;
    CProtocolMessage::protocolMessagePtr_t pm = make_shared<CProtocolMessage>();
    pm->encodeWithAttachment<cmdGET_LOG_ERROR>(cmd);
    pushMsg(pm);
}

bool CCommanderChannel::on_cmdDOWNLOAD_TEST(CProtocolMessage::protocolMessagePtr_t _msg)
{
    SBinaryAttachmentCmd cmd;
    cmd.convertFromData(_msg->bodyToContainer());

    // Calculate CRC32 of the recieved file data
    boost::crc_32_type crc32;
    crc32.process_bytes(&cmd.m_fileData[0], cmd.m_fileData.size());

    if (crc32.checksum() == cmd.m_crc32)
    {
        chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        chrono::microseconds downloadTime = chrono::duration_cast<chrono::microseconds>(now - m_headerReadTime);

        // Form reply command
        SBinaryDownloadStatCmd reply_cmd;
        reply_cmd.m_recievedCrc32 = crc32.checksum();
        reply_cmd.m_recievedFileSize = cmd.m_fileData.size();
        reply_cmd.m_downloadTime = downloadTime.count();

        CProtocolMessage::protocolMessagePtr_t msg = make_shared<CProtocolMessage>();
        msg->encodeWithAttachment<cmdDOWNLOAD_TEST_STAT>(reply_cmd);
        pushMsg(msg);
    }
    else
    {
        stringstream ss;
        ss << "Received binary has wrong checksum: " << crc32.checksum() << " instead of " << cmd.m_crc32
           << " | size: " << cmd.m_fileData.size() << " name: " << cmd.m_fileName;
        SSimpleMsgCmd cmd;
        cmd.m_sMsg = ss.str();
        CProtocolMessage::protocolMessagePtr_t pm = make_shared<CProtocolMessage>();
        pm->encodeWithAttachment<cmdDOWNLOAD_TEST_ERROR>(cmd);
        pushMsg(pm);
    }

    return true;
}

void CCommanderChannel::readAgentUUIDFile()
{
    const string sAgentUUIDFile(CUserDefaults::getAgentUUIDFile());
    LOG(MiscCommon::info) << "Reading an agent UUID file: " << sAgentUUIDFile;
    ifstream f(sAgentUUIDFile.c_str());
    if (!f.is_open() || !f.good())
    {
        string msg("Could not open an agent UUID file: ");
        msg += sAgentUUIDFile;
        throw runtime_error(msg);
    }
    f >> m_id;
}

void CCommanderChannel::createAgentUUIDFile() const
{
    const string sAgentUUIDFile(CUserDefaults::getAgentUUIDFile());
    LOG(MiscCommon::info) << "Creating an agent UUID file: " << sAgentUUIDFile;
    ofstream f(sAgentUUIDFile.c_str());
    if (!f.is_open() || !f.good())
    {
        string msg("Could not open an agent UUID file: ");
        msg += sAgentUUIDFile;
        throw runtime_error(msg);
    }

    f << m_id;
}

void CCommanderChannel::deleteAgentUUIDFile() const
{
    const string sAgentUUIDFile(CUserDefaults::getAgentUUIDFile());
    if (sAgentUUIDFile.empty())
        return;

    // TODO: check error code
    unlink(sAgentUUIDFile.c_str());
}

void CCommanderChannel::onRemoteEndDissconnected()
{
    stop();
    deleteAgentUUIDFile();
    LOG(info) << "The Agent [" << m_id << "] exited.";
    exit(EXIT_SUCCESS);
}

bool CCommanderChannel::on_cmdASSIGN_USER_TASK(CProtocolMessage::protocolMessagePtr_t _msg)
{
    SAssignUserTaskCmd cmd;
    cmd.convertFromData(_msg->bodyToContainer());
    LOG(MiscCommon::info) << "Recieved a user task assigment. User's task: " << cmd.m_sExeFile;

    m_sUsrExe = cmd.m_sExeFile;

    return true;
}

bool CCommanderChannel::on_cmdACTIVATE_AGENT(CProtocolMessage::protocolMessagePtr_t _msg)
{
    string sUsrExe(m_sUsrExe);
    smart_path(&sUsrExe);
    StringVector_t params;
    string output;

    try
    {
        LOG(MiscCommon::info) << "Executing user task: " << sUsrExe;
        do_execv(sUsrExe, params, 60, &output);
    }
    catch (exception& e)
    {
        LOG(error) << e.what();

        // Send response back to server
        SSimpleMsgCmd cmd;
        cmd.m_sMsg = e.what();
        cmd.m_msgSeverity = MiscCommon::error;
        cmd.m_srcCommand = cmdACTIVATE_AGENT;
        CProtocolMessage::protocolMessagePtr_t pm = make_shared<CProtocolMessage>();
        pm->encodeWithAttachment<cmdSIMPLE_MSG>(cmd);
        pushMsg(pm);
    }

    return true;
}
