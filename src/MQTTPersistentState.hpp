#pragma once

#include "Enums.hpp"
#include "Forward.hpp"
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <optional>

namespace nioev {

class MQTTPersistentState {
public:
    using ScriptName = std::string;
    void addSubscription(MQTTClientConnection& conn, std::string topic, QoS qos, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback);
    void addSubscription(std::string scriptName, std::string topic, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback);
    void deleteSubscription(MQTTClientConnection& conn, const std::string& topic);
    void deleteAllSubscriptions(MQTTClientConnection& conn);

    struct Subscription {
        std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> subscriber;
        std::string topic;
        std::vector<std::string> topicSplit; // only used for wildcard subscriptions
        std::optional<QoS> qos;
        Subscription(std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> conn, std::string topic, std::vector<std::string>&& topicSplit, std::optional<QoS> qos)
        : subscriber(std::move(conn)), topic(std::move(topic)), topicSplit(std::move(topicSplit)), qos(qos) {

        }
    };

    void forEachSubscriber(const std::string& topic, std::function<void(Subscription&)> callback);
    void retainMessage(std::string&& topic, std::vector<uint8_t>&& payload);
private:

    void addSubscriptionInternal(std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> subscriber, std::string topic, std::optional<QoS> qos, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback);

    std::unordered_multimap<std::string, Subscription> mSimpleSubscriptions;
    std::vector<Subscription> mWildcardSubscriptions;
    std::shared_mutex mMutex;
    struct RetainedMessage {
        std::vector<uint8_t> payload;
    };
    std::unordered_map<std::string, RetainedMessage> mRetainedMessages;
};

}
