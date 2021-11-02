#include "log_reader.h"

#include "log.h"

#include <algorithm>

#include <wblib/exceptions.h>
#include <wblib/json_utils.h>
#include <wblib/mqtt.h>
#include <syslog.h>

using namespace WBMQTT;

#define LOG(logger) ::logger.Log() << "[logs] "

namespace
{
    const auto     DMESG_SERVICE   = "dmesg";
    const uint32_t MAX_LOG_RECORDS = 100;

    std::vector<std::string> ExecCommand(const std::string& cmd)
    {
        std::unique_ptr<FILE, decltype(&pclose)> fd(popen(cmd.c_str(), "r"), pclose);
        if(!fd) {
            throw std::runtime_error("Cannot open pipe for '" + cmd + "'");
        }
        std::array<char, 256> buffer;
        std::string result;
        while(!feof(fd.get())) {
            auto bytes = fread(buffer.data(), 1, buffer.size(), fd.get());
            result.append(buffer.data(), bytes);
        }
        return StringSplit(result, '\n');
    }

    // Input string example:
    // -1 e932c72aeb0b44c6a093b94797460151 Tue 2021-04-06 07:35:01 UTC—Tue 2021-04-06 07:44:15 UTC
    Json::Value GetBootRec(const std::string& str)
    {
        Json::Value res;
        std::istringstream ss(str);
        ss.exceptions(std::ios_base::failbit | std::ios_base::badbit);
        int bootId;
        ss >> bootId;
        std::string hash;
        ss >> hash;
        res["hash"] = hash;
        tm t = {};
        const auto* timeFormat = "%a %Y-%m-%d %H:%M:%S UTC";
        ss >> std::get_time(&t, timeFormat);
        res["start"] = Json::Value::Int64(mktime(&t));
        if (bootId != 0) {
            ss.seekg(3, std::ios_base::cur); // pass '—' U+2014 (0xe2, 0x80, 0x94) EM DASH
            ss >> std::get_time(&t, timeFormat);
            res["end"] = Json::Value::Int64(mktime(&t));
        }
        return res;
    }

    Json::Value GetBoots()
    {
        Json::Value res;
        auto boots = ExecCommand("journalctl --utc --list-boots");
        std::reverse(boots.begin(), boots.end());
        for(const auto& boot: boots) {
            try {
                res.append(GetBootRec(boot));
            } catch (const std::exception& e) {
                throw std::runtime_error("Failed to parse boot string '" + boot + "'");
            }
        }
        return res;
    }

    Json::Value GetServices()
    {
        const char* servicePostfix = ".service";
        const auto servicePostfixLen = strlen(servicePostfix);
        Json::Value res(Json::arrayValue);
        auto services = ExecCommand("systemctl list-unit-files *.service");
        for(const auto& service: services) {
            auto pos = service.find(servicePostfix);
            if (pos != std::string::npos) {
                res.append(service.substr(0, pos + servicePostfixLen));
            }
        }
        res.append(DMESG_SERVICE);
        return res;
    }

    uint32_t GetMaxLogsEntries(const Json::Value& params)
    {
        return std::min(MAX_LOG_RECORDS, params.get("limit", MAX_LOG_RECORDS).asUInt());
    }

    struct TMakeJournalctlQueryResult
    {
        std::string Query;
        bool        ReverseOutput = false;
        bool        ParseService = true;
    };

    TMakeJournalctlQueryResult MakeJournalctlQuery(const Json::Value& params)
    {
        TMakeJournalctlQueryResult res;
        std::stringstream ss;
        ss << "journalctl --no-pager -o export";
        auto service = params.get("service", "").asString();
        if (!service.empty()) {
            ss << " -u " << service;
            res.ParseService = false;
        }
        ss << " -n " << GetMaxLogsEntries(params);
        if (params.isMember("boot")) {
            ss << " -b " << params["boot"].asString();
        }
        if (params.isMember("time")) {
            tm dt = {};
            time_t t = params["time"].asInt64();
            gmtime_r(&t, &dt);
            ss << " -r -U " << "\"" << std::put_time(&dt, "%Y-%m-%d %H:%M:%S") << "\"";
            res.Query = ss.str();
            return res;
        }
        if (params.isMember("cursor")) {
            auto& cursor = params["cursor"];
            if (cursor.isMember("id") && cursor.isMember("direction")) {
                ss << " --after-cursor=\"" << cursor["id"].asString() << "\"";
                if (cursor["direction"].asString() == "backward") {
                    ss << " -r";
                } else {
                    // Forward cursor queries return rows in ascending order, but we want a descending order
                    res.ReverseOutput = true;
                }
                res.Query = ss.str();
                return res;
            }
        }
        // We can't use -r option as journalctl has a bug not returning all requested rows
        res.Query = ss.str();
        res.ReverseOutput = true;
        return res;
    }

    Json::Value GetDmesgLogs()
    {
        Json::Value res(Json::arrayValue);
        for (const auto& s: ExecCommand("dmesg --color=never")) {
            Json::Value entry;
            entry["msg"] = s;
            res.append(entry);
        }
        return res;
    }

    // libwbmqtt1 log prefixes to syslog severity levels map
    const std::vector<std::pair<std::string, int>> LibWbMqttLogLevels = {
        {"ERROR:",   LOG_ERR    },
        {"WARNING:", LOG_WARNING},
        {"DEBUG:",   LOG_DEBUG  }
    };

    void ParseMsg(const std::string& s, Json::Value& entry)
    {
        entry["msg"] = s;
        if (!entry.isMember("level")) {
            std::any_of(LibWbMqttLogLevels.begin(), LibWbMqttLogLevels.end(), [&](const auto& p){
                if (StringStartsWith(s, p.first)) {
                    entry["level"] = p.second;
                    return true;
                }
                return false;
            });
        }
    }

    void ParseTimestamp(const std::string& s, Json::Value& entry)
    {
        char* end;
        auto ts = strtoull(s.c_str(), &end, 10);
        if (s.c_str() != end) {
            // __REALTIME_TIMESTAMP is in microseconds, convert it to milliseconds
            entry["time"] = ts/1000;
        }
    }

    void ParsePriority(const std::string& s, Json::Value& entry)
    {
        auto level = atoi(s.c_str());
        // journald sets LOG_INFO priority for all unprefixed messages got fom stderr/stdout
        // They priority is set in ParseMsg according to a prefix.
        if (level != LOG_INFO && !entry.isMember("level")) {
            entry["level"] = level;
        }
    }

    void ParseCursor(const std::string& s, Json::Value& entry)
    {
        entry["cursor"] = s;
    }

    void ParseService(const std::string& s, Json::Value& entry)
    {
        const std::string SERVICE_SUFFIX(".service");
        if (WBMQTT::StringHasSuffix(s, SERVICE_SUFFIX)) {
            entry["service"] = s.substr(0, s.length() - SERVICE_SUFFIX.length());
        } else {
            entry["service"] = s;
        }
    }

    typedef std::function<void(const std::string&, Json::Value&)> TJournaldParamToJsonFn;
    const std::vector<std::pair<std::string, TJournaldParamToJsonFn>> Prefixes = {
        {"MESSAGE=",              ParseMsg      },
        {"__REALTIME_TIMESTAMP=", ParseTimestamp},
        {"PRIORITY=",             ParsePriority },
        {"__CURSOR=",             ParseCursor   }
    };

    Json::Value MakeJouralctlRequest(const Json::Value& params)
    {
        auto query = MakeJournalctlQuery(params);
        auto prefixes(Prefixes);
        if (query.ParseService) {
            prefixes.emplace_back("_SYSTEMD_UNIT=", ParseService);
        }
        LOG(Debug) << query.Query;
        Json::Value res(Json::arrayValue);
        Json::Value entry;
        for (const auto& s: ExecCommand(query.Query)) {
            if (StringStartsWith(s, "__CURSOR=") && !entry.empty()) {
                res.append(entry);
                entry.clear();
            }
            std::any_of(prefixes.begin(), prefixes.end(), [&](const auto& p){
                if (StringStartsWith(s, p.first)) {
                    p.second(s.substr(p.first.length()), entry);
                    return true;
                }
                return false;
            });
        }
        if (!entry.empty()) {
            res.append(entry);
        }
        if (query.ReverseOutput) {
            std::reverse(res.begin(), res.end());
        }
        return res;
    }

    Json::Value GetJouralctlLogs(const Json::Value& params)
    {
        Json::Value res(MakeJouralctlRequest(params));
        if (res.size() > 2) {
            // cursor is needed only for the first and the last record
            std::for_each(++res.begin(), --res.end(), [](auto& item) { item.removeMember("cursor"); });
        }
        return res;
    }

    Json::Value GetLogs(const Json::Value& params)
    {
        if (params.get("service", "").asString() == DMESG_SERVICE) {
            return GetDmesgLogs();
        }
        return GetJouralctlLogs(params);
    }
}

TMQTTJournaldGateway::TMQTTJournaldGateway(PMqttClient mqttClient, PMqttRpcServer rpcServer)
    : MqttClient(mqttClient),
      RpcServer(rpcServer),
      Boots(GetBoots())
{
    RpcServer->RegisterMethod("logs", "List", std::bind(&TMQTTJournaldGateway::List, this, std::placeholders::_1));
    RpcServer->RegisterMethod("logs", "Load", std::bind(&TMQTTJournaldGateway::Load, this, std::placeholders::_1));
}

Json::Value TMQTTJournaldGateway::List(const Json::Value& /*params*/)
{
    LOG(Debug) << "Run RPC List()";
    Json::Value res;
    try {
        res["boots"] = Boots;
        res["services"] = GetServices();
    }
    catch(const std::exception& e) {
        LOG(Error) << e.what();
    }
    return res;
}

Json::Value TMQTTJournaldGateway::Load(const Json::Value& params)
{
    LOG(Debug) << "Run RPC Load()";
    try {
        return GetLogs(params);
    } catch (const std::exception& e) {
        Json::Value res(Json::arrayValue);
        res.append(e.what());
        return res;
    }
}
