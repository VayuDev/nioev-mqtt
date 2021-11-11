#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <variant>
#include <functional>
#include "../Enums.hpp"

namespace nioev {

enum class ScriptRunType {
    Sync,
    Async
};

struct ScriptInitReturn {
    ScriptRunType runType = ScriptRunType::Async;
};

struct ScriptRunArgsMqttMessage {
    std::string topic;
    std::vector<uint8_t> payload;
};
using ScriptInputArgs = std::variant<ScriptRunArgsMqttMessage>;

enum class SyncAction {
    Continue,
    AbortPublish
};

struct ScriptOutputArgs {
    std::function<void(const std::string& topic, std::vector<uint8_t>&& payload, QoS qos, Retain retain)> publish;
    std::function<void(const std::string& topic)> subscribe;
    std::function<void(const std::string& topic)> unsubscribe;
    std::function<void(const std::string& error)> error;
    std::function<void(SyncAction action)> syncAction;
};

struct ScriptInitOutputArgs {
    std::function<void(const std::string& reason)> error;
    std::function<void(const ScriptInitReturn&)> success;
};

class ScriptContainer {
public:
    virtual ~ScriptContainer() = default;
    virtual void init(const ScriptInitOutputArgs&) = 0;
    virtual void run(const ScriptInputArgs&, const ScriptOutputArgs&) = 0;
    [[nodiscard]] const ScriptInitReturn& getInitArgs() const {
        return mScriptInitReturn;
    }
    virtual void forceQuit() = 0;

protected:
    ScriptInitReturn mScriptInitReturn;
};

}