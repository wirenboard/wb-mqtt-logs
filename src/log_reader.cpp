#include "log_reader.h"

#include "log.h"

#include <algorithm>
#include <set>
#include <unicode/regex.h>

#include <sys/sysinfo.h>
#include <syslog.h>
#include <systemd/sd-journal.h>
#include <wblib/exceptions.h>
#include <wblib/json_utils.h>
#include <wblib/mqtt.h>

using namespace WBMQTT;
using icu::RegexMatcher;
using icu::UnicodeString;

#define LOG(logger) ::logger.Log() << "[logs] "

namespace
{
    const auto DMESG_SERVICE = "dmesg";
    const uint32_t MAX_LOG_RECORDS = 100;

    void SdThrowError(int res, const std::string& msg)
    {
        if (res < 0) {
            throw std::runtime_error(std::string(msg) + ": " + strerror(-res));
        }
    }

    std::vector<std::string> ExecCommand(const std::string& cmd)
    {
        std::unique_ptr<FILE, decltype(&pclose)> fd(popen(cmd.c_str(), "r"), pclose);
        if (!fd) {
            throw std::runtime_error("Cannot open pipe for '" + cmd + "'");
        }
        std::array<char, 256> buffer;
        std::string result;
        while (!feof(fd.get())) {
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
        for (const auto& boot: boots) {
            try {
                res.append(GetBootRec(boot));
            } catch (const std::exception& e) {
                LOG(Warn) << "Failed to parse boot string '" + boot + "'";
            }
        }
        return res;
    }

    Json::Value GetServices()
    {
        const char* servicePostfix = ".service";
        const auto servicePostfixLen = strlen(servicePostfix);
        Json::Value res(Json::arrayValue);
        auto services = ExecCommand("systemctl list-units --type=service --state=loaded --no-pager --plain");
        for (const auto& service: services) {
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

    const char* GetData(sd_journal* j, const std::string& fieldName)
    {
        const char* d;
        size_t l;
        int r = sd_journal_get_data(j, fieldName.c_str(), (const void**)&d, &l);
        if (r == 0 && l > fieldName.size() + 1) {
            return d + fieldName.size() + 1;
        }
        return nullptr;
    }

    enum class TFilterDirection
    {
        Forward,
        Backward,
        Default
    };
    struct TJournalctlFilterParams
    {
        TFilterDirection Direction = TFilterDirection::Default;
        std::string Service;
        uint32_t MaxEntries = MAX_LOG_RECORDS;
        std::chrono::microseconds From = std::chrono::microseconds::zero();
        std::string Cursor;
        UnicodeString Pattern;
        bool CaseSensitive = true;
        bool RegEx = false;
    };

    TJournalctlFilterParams SetFilter(sd_journal* j, const Json::Value& params)
    {
        TJournalctlFilterParams filter;
        auto service = params.get("service", "").asString();
        if (!service.empty()) {
            SdThrowError(sd_journal_add_match(j, ("_SYSTEMD_UNIT=" + service).c_str(), 0), "Adding match failed");
            filter.Service = service;
        }

        filter.MaxEntries = GetMaxLogsEntries(params);

        auto boot = params.get("boot", "").asString();
        if (!boot.empty()) {
            sd_journal_add_match(j, ("_BOOT_ID=" + boot).c_str(), 0);
        }

        std::set<int> levels;
        for (const auto& lv: params["levels"]) {
            if (lv.isInt()) {
                int l = lv.asInt();
                if (l >= LOG_EMERG && l <= LOG_DEBUG && 0 == levels.count(l)) {
                    levels.insert(l);
                    SdThrowError(sd_journal_add_match(j, ("PRIORITY=" + std::to_string(l)).c_str(), 0),
                                 "Adding match failed");
                }
            }
        }

        if (params.isMember("time")) {
            filter.From = std::chrono::microseconds(params["time"].asInt64() * 1000000);
        }

        if (params.isMember("cursor")) {
            auto& cursor = params["cursor"];
            filter.Cursor = cursor.get("id", "").asString();

            auto directionString = cursor.get("direction", "").asString();
            if (directionString == "forward") {
                filter.Direction = TFilterDirection::Forward;
            } else if (directionString == "backward") {
                filter.Direction = TFilterDirection::Backward;
            }
        }

        filter.Pattern = UnicodeString::fromUTF8(params.get("pattern", "").asString());
        filter.CaseSensitive = params.get("case-sensitive", true).asBool();
        filter.RegEx = params.get("regex", false).asBool();

        return filter;
    }

    bool HasSubstring(const UnicodeString& msg, const UnicodeString& pattern, bool caseSensitive)
    {
        if (caseSensitive) {
            return (msg.indexOf(pattern) >= 0);
        }
        return (UnicodeString(msg).foldCase().indexOf(UnicodeString(pattern).foldCase()) >= 0);
    }

    bool MatchesRegex(const UnicodeString& msg, const UnicodeString& pattern, bool caseSensitive)
    {
        UErrorCode status = U_ZERO_ERROR;
        RegexMatcher m(pattern, (caseSensitive ? 0 : UREGEX_CASE_INSENSITIVE), status);
        if (U_FAILURE(status)) {
            throw std::runtime_error("Could not create a RegexMatcher object");
        }
        m.reset(msg);
        bool ok = m.find(status);
        if (U_FAILURE(status)) {
            throw std::runtime_error("Error searching for pattern");
        }
        return ok;
    }

    Json::Value ParseDmesgLog(const std::string& line, std::chrono::system_clock::time_point bootTime)
    {
        Json::Value entry;
        size_t p = 0;
        if (line[0] == '[') {
            auto sec = strtod(line.c_str() + 1, nullptr);
            auto t = bootTime + std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(sec * 1000));
            entry["time"] = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
            p = line.find(']');
            p = (p == std::string::npos) ? 0 : p + 1;
            if (line[p] == ' ') {
                ++p;
            }
        }
        entry["msg"] = line.substr(p);
        return entry;
    }

    Json::Value GetDmesgLogs(const Json::Value& params, std::chrono::system_clock::time_point bootTime)
    {
        Json::Value res(Json::arrayValue);

        auto pattern = UnicodeString::fromUTF8(params.get("pattern", "").asString());
        auto caseSensitive = params.get("case-sensitive", true).asBool();
        auto regEx = params.get("regex", false).asBool();

        for (const auto& s: ExecCommand("dmesg --color=never --force-prefix")) {
            Json::Value entry(ParseDmesgLog(s, bootTime));

            if (!pattern.isEmpty()) {
                auto msg = UnicodeString::fromUTF8(entry["msg"].asString());
                if (regEx) {
                    if (!MatchesRegex(msg, pattern, caseSensitive)) {
                        continue;
                    }
                } else {
                    if (!HasSubstring(msg, pattern, caseSensitive)) {
                        continue;
                    }
                }
            }

            res.append(entry);
        }
        return res;
    }

    // libwbmqtt1 log prefixes to syslog severity levels map
    const std::vector<std::pair<std::string, int>> LibWbMqttLogLevels = {{"ERROR:", LOG_ERR},
                                                                         {"WARNING:", LOG_WARNING},
                                                                         {"DEBUG:", LOG_DEBUG}};

    bool AddMsg(sd_journal* j, Json::Value& entry, const UnicodeString& pattern, bool caseSensitive, bool regEx)
    {
        const char* d = GetData(j, "MESSAGE");
        if (d == nullptr) {
            return false;
        }
        if (!pattern.isEmpty()) {
            auto msg = UnicodeString::fromUTF8(d);
            if (regEx) {
                if (!MatchesRegex(msg, pattern, caseSensitive)) {
                    return false;
                }
            } else {
                if (!HasSubstring(msg, pattern, caseSensitive)) {
                    return false;
                }
            }
        }
        entry["msg"] = d;
        if (!entry.isMember("level")) {
            std::any_of(LibWbMqttLogLevels.begin(), LibWbMqttLogLevels.end(), [&](const auto& p) {
                if (StringStartsWith(d, p.first)) {
                    entry["level"] = p.second;
                    return true;
                }
                return false;
            });
        }
        return true;
    }

    void AddTimestamp(sd_journal* j, Json::Value& entry)
    {
        uint64_t ts;
        SdThrowError(sd_journal_get_realtime_usec(j, &ts), "Failed to read timestamp");
        // __REALTIME_TIMESTAMP is in microseconds, convert it to milliseconds
        entry["time"] = ts / 1000;
    }

    void AddPriority(sd_journal* j, Json::Value& entry)
    {
        const char* d = GetData(j, "PRIORITY");
        if (d == nullptr) {
            return;
        }
        auto level = atoi(d);
        // journald sets LOG_INFO priority for all unprefixed messages got fom stderr/stdout
        // They priority is set in ParseMsg according to a prefix.
        if (level != LOG_INFO && !entry.isMember("level")) {
            entry["level"] = level;
        }
    }

    void AddCursor(sd_journal* j, Json::Value& entry)
    {
        char* k = nullptr;
        SdThrowError(sd_journal_get_cursor(j, &k), "Failed to get cursor");
        entry["cursor"] = k;
        free(k);
    }

    void AddService(sd_journal* j, Json::Value& entry)
    {
        const char* d = GetData(j, "_SYSTEMD_UNIT");
        if (d == nullptr) {
            return;
        }
        std::string s(d);
        const std::string SERVICE_SUFFIX(".service");
        if (WBMQTT::StringHasSuffix(s, SERVICE_SUFFIX)) {
            entry["service"] = s.substr(0, s.length() - SERVICE_SUFFIX.length());
        } else {
            entry["service"] = s;
        }
    }

    Json::Value MakeJouralctlRequest(const Json::Value& params, std::atomic_bool& cancelLoading)
    {
        Json::Value res(Json::arrayValue);
        sd_journal* j = nullptr;
        SdThrowError(sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY), "Failed to open journal");
        std::unique_ptr<sd_journal, decltype(&sd_journal_close)> journalPtr(j, &sd_journal_close);

        auto filter = SetFilter(j, params);

        auto moveFn = filter.Direction == TFilterDirection::Forward ? sd_journal_next : sd_journal_previous;
        if (!filter.Cursor.empty()) {
            SdThrowError(sd_journal_seek_cursor(j, filter.Cursor.c_str()), "Failed to seek to tail of journal");
            if (filter.Direction != TFilterDirection::Default) {
                moveFn(j); // Pass pointed by cursor record
            }
        } else if (filter.From.count() > 0) {
            SdThrowError(sd_journal_seek_realtime_usec(j, filter.From.count()), "Failed to seek to tail of journal");
        } else {
            SdThrowError(sd_journal_seek_tail(j), "Failed to seek to tail of journal");
        }

        int r = moveFn(j);
        while (r > 0 && filter.MaxEntries && !cancelLoading) {
            Json::Value item;
            if (AddMsg(j, item, filter.Pattern, filter.CaseSensitive, filter.RegEx)) {
                AddTimestamp(j, item);
                AddCursor(j, item);
                AddPriority(j, item);
                if (filter.Service.empty()) {
                    AddService(j, item);
                }
                res.append(item);
                --filter.MaxEntries;
            }
            r = moveFn(j);
        }

        if (r < 0) {
            LOG(Error) << "Failed to get next journal entry: " << strerror(-r);
        }

        // Forward queries return rows in ascending order, but we want a descending order
        if (filter.Direction == TFilterDirection::Forward) {
            std::reverse(res.begin(), res.end());
        }
        return res;
    }

    Json::Value GetJouralctlLogs(const Json::Value& params, std::atomic_bool& cancelLoading)
    {
        Json::Value res(MakeJouralctlRequest(params, cancelLoading));
        if (res.size() > 2) {
            // cursor is needed only for the first and the last record
            std::for_each(++res.begin(), --res.end(), [](auto& item) { item.removeMember("cursor"); });
        }
        return res;
    }

    Json::Value GetLogs(const Json::Value& params,
                        std::atomic_bool& cancelLoading,
                        std::chrono::system_clock::time_point bootTime)
    {
        if (params.get("service", "").asString() == DMESG_SERVICE) {
            return GetDmesgLogs(params, bootTime);
        }
        return GetJouralctlLogs(params, cancelLoading);
    }

    std::chrono::system_clock::time_point GetBootTime()
    {
        auto time = std::chrono::system_clock::now();
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            time -= std::chrono::seconds(si.uptime);
        }
        return time;
    }
}

TMQTTJournaldGateway::TMQTTJournaldGateway(PMqttClient mqttClient,
                                           PMqttRpcServer requestsRpcServer,
                                           PMqttRpcServer cancelRequestsRpcServer)
    : MqttClient(mqttClient),
      RequestsRpcServer(requestsRpcServer),
      CancelRequestsRpcServer(cancelRequestsRpcServer),
      Boots(GetBoots()),
      CancelLoading(false),
      BootTime(GetBootTime())
{
    RequestsRpcServer->RegisterMethod("logs",
                                      "List",
                                      std::bind(&TMQTTJournaldGateway::List, this, std::placeholders::_1));
    RequestsRpcServer->RegisterMethod("logs",
                                      "Load",
                                      std::bind(&TMQTTJournaldGateway::Load, this, std::placeholders::_1));
    CancelRequestsRpcServer->RegisterMethod("logs",
                                            "CancelLoad",
                                            std::bind(&TMQTTJournaldGateway::CancelLoad, this, std::placeholders::_1));
}

Json::Value TMQTTJournaldGateway::List(const Json::Value& /*params*/)
{
    LOG(Debug) << "Run RPC List()";
    Json::Value res;
    try {
        res["boots"] = Boots;
        res["services"] = GetServices();
    } catch (const std::exception& e) {
        LOG(Error) << e.what();
    }
    return res;
}

Json::Value TMQTTJournaldGateway::Load(const Json::Value& params)
{
    LOG(Debug) << "Run RPC Load()";
    try {
        CancelLoading = false;
        return GetLogs(params, CancelLoading, BootTime);
    } catch (const std::exception& e) {
        LOG(Error) << e.what();
        throw;
    }
}

Json::Value TMQTTJournaldGateway::CancelLoad(const Json::Value& params)
{
    LOG(Debug) << "Run RPC CancelLoad()";
    CancelLoading = true;
    return Json::Value();
}
