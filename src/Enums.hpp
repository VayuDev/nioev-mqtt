#pragma once

#include <cstdint>

namespace nioev {

enum class MQTTMessageType : uint8_t
{
    Invalid = 0,
    CONNECT = 1,
    CONNACK = 2,
    PUBLISH = 3,
    SUBSCRIBE = 8,
    SUBACK = 9,
    PINGREQ = 12,
    PINGRESP = 13,
    Count = 15
};

enum class QoS : uint8_t
{
    QoS0 = 0,
    QoS1 = 1,
    QoS2 = 2,
};

}