#pragma once

#include <variant>
#include <string>
#include "../Forward.hpp"
#include "../Enums.hpp"
#include <vector>
#include <cstdint>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace nioev {

struct ScriptActionSubscribe {
    std::string scriptName;
    std::string topic;
};
struct ScriptActionUnsubscribe {
    std::string scriptName;
    std::string topic;
};

struct ScriptActionPublish {
    std::string scriptName;
    std::string topic;
    std::vector<uint8_t> payload;
    QoS qos;
    Retain retain;
};

struct ScriptActionListen {
    std::string scriptName;
    std::string listenIdentifier;
};

struct ScriptActionSendToClient {
    std::string scriptName;
    int fd;
    std::vector<uint8_t> data;
    Compression compression;
};

using ScriptAction = std::variant<ScriptActionPublish, ScriptActionSubscribe, ScriptActionUnsubscribe, ScriptActionListen, ScriptActionSendToClient>;


class ScriptActionPerformer final {
public:
    explicit ScriptActionPerformer(Application& app);
    ~ScriptActionPerformer();
    void enqueueAction(ScriptAction&&);
private:
    void actionsPerformerThreadFunc();

    Application& mApp;

    std::queue<ScriptAction> mActions;
    std::mutex mActionsMutex;
    std::condition_variable mActionsCV;
    std::atomic<bool> mShouldRun = true;
    std::thread mActionsPerformerThread;
};

}
