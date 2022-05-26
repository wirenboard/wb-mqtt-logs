#pragma once

#include <wblib/mqtt.h>
#include <wblib/rpc.h>

class TMQTTJournaldGateway
{
public:
    TMQTTJournaldGateway(WBMQTT::PMqttClient    mqttClient,
                         WBMQTT::PMqttRpcServer requestsRpcServer,
                         WBMQTT::PMqttRpcServer cancelRequestsRpcServer);
private:
    Json::Value Load(const Json::Value& params);
    Json::Value List(const Json::Value& params);
    Json::Value CancelLoad(const Json::Value& params);

    WBMQTT::PMqttClient                   MqttClient;
    WBMQTT::PMqttRpcServer                RequestsRpcServer;
    WBMQTT::PMqttRpcServer                CancelRequestsRpcServer;
    Json::Value                           Boots;
    std::atomic_bool                      CancelLoading;
    std::chrono::system_clock::time_point BootTime;
};
