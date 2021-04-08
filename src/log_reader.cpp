#include "log_reader.h"

#include "log.h"

#include <algorithm>

#include <wblib/exceptions.h>
#include <wblib/json_utils.h>
#include <wblib/mqtt.h>

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
        auto boots = ExecCommand("journalctl --list-boots");
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

    std::string MakeJournalctlQuery(const Json::Value& params, bool includeCursorEntry)
    {
        std::stringstream ss;
        ss << "journalctl --no-pager -o export";
        auto service = params.get("service", "").asString();
        if (!service.empty()) {
            ss << " -u " << service;
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
            return ss.str();
        }
        if (params.isMember("cursor")) {
            auto& cursor = params["cursor"];
            if (cursor.isMember("id") && cursor.isMember("direction")) {
                ss << (includeCursorEntry ? " --cursor=" : " --after-cursor=");
                ss << "\"" << cursor["id"].asString() << "\"";
                if (cursor["direction"].asString() == "backward") {
                    ss << " -r";
                }
                return ss.str();
            }
        }
        ss << " -r";
        return ss.str();
    }

    bool IsForwardCursorQuery(const Json::Value& params)
    {
        if (!params.isMember("time") && params.isMember("cursor")) {
            auto& cursor = params["cursor"];
            return cursor.isMember("id") && cursor.isMember("direction") && (cursor["direction"].asString() != "backward");
        }
        return false;
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
        {"ERROR:",   3},
        {"WARNING:", 4},
        {"DEBUG:",   7}
    };

    void ParseMsg(const std::string& s, Json::Value& entry)
    {
        entry["msg"] = s;
        std::any_of(LibWbMqttLogLevels.begin(), LibWbMqttLogLevels.end(), [&](const auto& p){
            if (StringStartsWith(s, p.first)) {
                entry["level"] = p.second;
                return true;
            }
            return false;
        });
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
        if (!entry.isMember("level")) {
            entry["level"] = atoi(s.c_str());
        }
    }

    void ParseCursor(const std::string& s, Json::Value& entry)
    {
        entry["cursor"] = s;
    }

    typedef std::function<void(const std::string&, Json::Value&)> TJournaldParamToJsonFn;
    const std::vector<std::pair<std::string, TJournaldParamToJsonFn>> Prefixes = {
        {"MESSAGE=",              ParseMsg      },
        {"__REALTIME_TIMESTAMP=", ParseTimestamp},
        {"PRIORITY=",             ParsePriority },
        {"__CURSOR=",             ParseCursor   }
    };

    Json::Value MakeJouralctlRequest(const Json::Value& params, bool includeCursorEntry)
    {
        auto query = MakeJournalctlQuery(params, includeCursorEntry);
        LOG(Debug) << query;
        Json::Value res(Json::arrayValue);
        Json::Value entry;
        for (const auto& s: ExecCommand(query)) {
            if (s.empty()) {
                res.append(entry);
                entry.clear();
            } else {
                std::any_of(Prefixes.begin(), Prefixes.end(), [&](const auto& p){
                    if (StringStartsWith(s, p.first)) {
                        p.second(s.substr(p.first.length()), entry);
                        return true;
                    }
                    return false;
                });
            }
        }
        if (IsForwardCursorQuery(params)) {
            // Forward cursor queries return rows in ascending order, but we want a descending order
            std::reverse(res.begin(), res.end());
        }
        return res;
    }

    Json::Value GetJouralctlLogs(const Json::Value& params)
    {
        Json::Value res(MakeJouralctlRequest(params, true));
        if (IsForwardCursorQuery(params)) {
            // Forward request can reach logs head and return less than requested rows count.
            // So request additional rows after cursor to fill missing rows.
            auto wantedCount =  GetMaxLogsEntries(params);
            if (res.size() < wantedCount) {
                Json::Value p(params);
                p["cursor"]["direction"] = "backward";
                p["limit"] = wantedCount - res.size();
                for (const auto& item: MakeJouralctlRequest(p, false)) {
                    res.append(item);
                }
            }
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
