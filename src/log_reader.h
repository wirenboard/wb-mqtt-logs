#pragma once

#include <wblib/mqtt.h>
#include <wblib/rpc.h>

class TMQTTJournaldGateway
{
public:
    TMQTTJournaldGateway(WBMQTT::PMqttClient    mqttClient,
                         WBMQTT::PMqttRpcServer rpcServer);
private:
    Json::Value Load(const Json::Value& params);
    Json::Value List(const Json::Value& /*params*/);

    WBMQTT::PMqttClient              MqttClient;
    WBMQTT::PMqttRpcServer           RpcServer;
    Json::Value                      Boots;
};
