#include <fstream>
#include "ApplicationState.hpp"
#include "scripting/ScriptContainer.hpp"

namespace nioev {

ApplicationState::ApplicationState()
: mClientManager(*this, 5), mWorkerThread([this]{workerThreadFunc();}) {
    mTimers.addPeriodicTask(std::chrono::seconds(2), [this] () mutable {
        cleanup();
    });
}
ApplicationState::~ApplicationState() {
    mShouldRun = false;
    mWorkerThread.join();
}
void ApplicationState::workerThreadFunc() {
    pthread_setname_np(pthread_self(), "app-state");
    auto processInternalQueue = [this] {
        std::unique_lock<std::shared_mutex> lock{mMutex};
        while(!mQueueInternal.empty()) {
            // don't call executeChangeRequest because that function acquires a lock
            std::visit(*this, std::move(std::move(mQueueInternal.front())));
            mQueueInternal.pop();
        }
    };
    while(mShouldRun) {
        processInternalQueue();
        while(!mQueue.was_empty()) {
            processInternalQueue();
            executeChangeRequest(mQueue.pop());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ApplicationState::requestChange(ChangeRequest&& changeRequest, ApplicationState::RequestChangeMode mode) {
    if(mode == RequestChangeMode::SYNC_WHEN_IDLE) {
        executeChangeRequest(std::move(changeRequest));
    } else if(mode == RequestChangeMode::ASYNC) {
        mQueue.push(std::move(changeRequest));
    } else if(mode == RequestChangeMode::SYNC) {
        // TODO smarter algorithm here
        executeChangeRequest(std::move(changeRequest));
    } else {
        assert(false);
    }
}

void ApplicationState::executeChangeRequest(ChangeRequest&& changeRequest) {
    std::unique_lock<std::shared_mutex> lock{mMutex};
    std::visit(*this, std::move(changeRequest));
}
void ApplicationState::operator()(ChangeRequestSubscribe&& req) {
    if(req.subType == SubscriptionType::WILDCARD) {
        auto& sub = mWildcardSubscriptions.emplace_back(req.subscriber, req.topic, std::move(req.topicSplit), req.qos);
        for(auto& retainedMessage: mRetainedMessages) {
            if(util::doesTopicMatchSubscription(retainedMessage.first, sub.topicSplit)) {
                sub.subscriber->publish(retainedMessage.first, retainedMessage.second.payload, req.qos, Retained::Yes);
            }
        }

    } else if(req.subType == SubscriptionType::SIMPLE) {
        mSimpleSubscriptions.emplace(std::piecewise_construct,
                                     std::make_tuple(req.topic),
                                     std::make_tuple(req.subscriber, req.topic, std::vector<std::string>{}, req.qos));
        auto retainedMessage = mRetainedMessages.find(req.topic);
        if(retainedMessage != mRetainedMessages.end()) {
            req.subscriber->publish(retainedMessage->first, retainedMessage->second.payload, req.qos, Retained::Yes);
        }
    } else if(req.subType == SubscriptionType::OMNI) {
        auto& sub = mOmniSubscriptions.emplace_back(req.subscriber, req.topic, std::move(req.topicSplit), req.qos);
        for(auto& retainedMessage: mRetainedMessages) {
            sub.subscriber->publish(retainedMessage.first, retainedMessage.second.payload, req.qos, Retained::Yes);
        }
    } else {
        assert(false);
    }
    auto subscriberAsMQTTConn = std::dynamic_pointer_cast<MQTTClientConnection>(req.subscriber);
    if(subscriberAsMQTTConn) {
        auto[state, stateLock] = subscriberAsMQTTConn->getPersistentState();
        if(state && state->cleanSession == CleanSession::No) {
            state->subscriptions.emplace_back(PersistentClientState::PersistentSubscription{ std::move(req.topic), req.qos });
        }
    }
}
void ApplicationState::operator()(ChangeRequestUnsubscribe&& req) {
    auto[start, end] = mSimpleSubscriptions.equal_range(req.topic);
    if(start != end) {
        for(auto it = start; it != end;) {
            if(it->second.subscriber == req.subscriber) {
                it = mSimpleSubscriptions.erase(it);
            } else {
                it++;
            }
        }
    } else {
        erase_if(mWildcardSubscriptions, [&req](auto& sub) {
            return sub.subscriber == req.subscriber && sub.topic == req.topic;
        });
    }
    auto subscriberAsMQTTConn = std::dynamic_pointer_cast<MQTTClientConnection>(req.subscriber);
    if(subscriberAsMQTTConn) {
        auto[state, stateLock] = subscriberAsMQTTConn->getPersistentState();
        if(state->cleanSession == CleanSession::No) {
            for(auto it = state->subscriptions.begin(); it != state->subscriptions.end(); ++it) {
                if(it->topic == req.topic) {
                    it = state->subscriptions.erase(it);
                } else {
                    it++;
                }
            }
        }
    }
}
void ApplicationState::operator()(ChangeRequestRetain&& req) {
    mRetainedMessages.emplace(std::move(req.topic), RetainedMessage{std::move(req.payload)});
}
void ApplicationState::operator()(ChangeRequestCleanup&& req) {
    cleanup();
}
void ApplicationState::cleanup() {
    std::unique_lock<std::shared_mutex> lock{mMutex};
    for(auto it = mClients.begin(); it != mClients.end(); ++it) {
        if(it->get()->getLastDataRecvTimestamp() + (int64_t)it->get()->getKeepAliveIntervalSeconds() * 2'000'000'000 <= std::chrono::steady_clock::now().time_since_epoch().count()) {
            // timeout
            logoutClient(*it->get());
        }
    }
    lock.unlock();
    // delete disconnected clients
    mClientManager.suspendAllThreads();
    lock.lock();
    for(auto it = mClients.begin(); it != mClients.end();) {
        if((*it)->isLoggedOut()) {
            it = mClients.erase(it);
        } else {
            it++;
        }
    }
    mClientManager.resumeAllThreads();
}
void ApplicationState::operator()(ChangeRequestDisconnectClient&& req) {
    logoutClient(*req.client);
}
void ApplicationState::operator()(ChangeRequestLoginClient&& req) {
    constexpr char AVAILABLE_RANDOM_CHARS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";

    decltype(mPersistentClientStates.begin()) existingSession;
    if(req.clientId.empty()) {
        assert(req.cleanSession == CleanSession::Yes);
        // generate random client id
        std::string randomId = req.client->getTcpClient().getRemoteIp() + ":" + std::to_string(req.client->getTcpClient().getRemotePort());
        auto start = randomId.size();
        existingSession = mPersistentClientStates.find(randomId);
        while(existingSession != mPersistentClientStates.end()) {
            std::ifstream urandom{"/dev/urandom"};
            randomId.resize(start + 16);
            for(size_t i = start; i < randomId.size(); ++i) {
                randomId.at(i) = AVAILABLE_RANDOM_CHARS[urandom.get() % strlen(AVAILABLE_RANDOM_CHARS)];
            }
            existingSession = mPersistentClientStates.find(randomId);
        }
        req.clientId = std::move(randomId);
    } else {
        existingSession = mPersistentClientStates.find(req.clientId);
    }


    SessionPresent sessionPresent = SessionPresent::No;
    auto createNewSession = [&] {
        auto newState = mPersistentClientStates.emplace_hint(existingSession, std::piecewise_construct, std::make_tuple(req.clientId), std::make_tuple());
        req.client->setClientId(req.clientId);
        newState->second.clientId = std::move(req.clientId);
        newState->second.currentClient = req.client.get();
        newState->second.cleanSession = req.cleanSession;
        req.client->setPersistentState(&newState->second);
        sessionPresent = SessionPresent::No;
    };

    if(existingSession != mPersistentClientStates.end()) {
        // disconnect existing client
        auto existingClient = existingSession->second.currentClient;
        if(existingClient) {
            spdlog::warn("[{}] Already logged in, closing old connection", req.clientId);
            logoutClient(*existingClient);
        }
        if(req.cleanSession == CleanSession::Yes || existingSession->second.cleanSession == CleanSession::Yes) {
            createNewSession();
        } else {
            sessionPresent = SessionPresent::Yes;
            req.client->setClientId(req.clientId);
            existingSession->second.currentClient = req.client.get();
            existingSession->second.cleanSession = req.cleanSession;
            req.client->setPersistentState(&existingSession->second);
            for(auto& sub: existingSession->second.subscriptions) {
                mQueueInternal.emplace(ChangeRequestSubscribe{req.client, sub.topic, util::splitTopics(sub.topic), util::hasWildcard(sub.topic) ? SubscriptionType::WILDCARD : SubscriptionType::SIMPLE, sub.qos});
            }
        }
    } else {
        // no session exists
        createNewSession();
    }

    // send CONNACK now
    // we need to do it here because only here we now the value of the session present flag
    // we could use callbacks, but that seems too complicated
    spdlog::info("[{}] Logged in from [{}:{}]", req.client->getClientId(), req.client->getTcpClient().getRemoteIp(), req.client->getTcpClient().getRemotePort());
    util::BinaryEncoder response;
    response.encodeByte(static_cast<uint8_t>(MQTTMessageType::CONNACK) << 4);
    response.encodeByte(2); // remaining packet length
    response.encodeByte(sessionPresent == SessionPresent::Yes ? 1 : 0);
    response.encodeByte(0); // everything okay


    req.client->sendData(response.moveData());
    req.client->setState(MQTTClientConnection::ConnectionState::CONNECTED);
}
void ApplicationState::operator()(ChangeRequestAddScript&& req) {
    auto existingScript = mScripts.find(req.name);
    if(existingScript != mScripts.end()) {
        deleteScript(existingScript);
    }
    auto script = mScripts.emplace(req.name, req.constructor());
    script.first->second->init(std::move(req.statusOutput));
}
void ApplicationState::operator()(ChangeRequestPublish&& req) {
    publishWithoutAcquiringMutex(std::move(req.topic), std::move(req.payload), req.qos, req.retain);
}
void ApplicationState::deleteScript(std::unordered_map<std::string, std::shared_ptr<ScriptContainer>>::iterator it) {
    if(it == mScripts.end())
        return;
    it->second->forceQuit();
    deleteAllSubscriptions(*it->second);
    mScripts.erase(it);
}
void ApplicationState::logoutClient(MQTTClientConnection& client) {
    if(client.isLoggedOut())
        return;
    mClientManager.removeClientConnection(client);
    {
        // perform will
        auto willMsg = client.moveWill();
        if(willMsg) {
            publishWithoutAcquiringMutex(std::move(willMsg->topic), std::move(willMsg->msg), willMsg->qos, willMsg->retain);
        }
    }
    deleteAllSubscriptions(client);
    {
        // detach persistent state
        auto[state, stateLock] = client.getPersistentState();
        if(!state) {
            return;
        }
        if(state->cleanSession == CleanSession::Yes) {
            mPersistentClientStates.erase(state->clientId);
        } else {
            state->currentClient = nullptr;
            state->lastDisconnectTime = std::chrono::steady_clock::now().time_since_epoch().count();
        }
        static_assert(std::is_reference<decltype(state)>::value);
        state = nullptr;
    }
    spdlog::info("[{}] Logged out", client.getClientId());
    client.getTcpClient().close();
    client.notifyLoggedOut();
}
void ApplicationState::publish(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain) {
    std::shared_lock<std::shared_mutex> lock{ mMutex };
    publishWithoutAcquiringMutex(std::move(topic), std::move(msg), qos, retain);
}
void ApplicationState::publishWithoutAcquiringMutex(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain) {
#ifndef NDEBUG
    if(topic != LOG_TOPIC) {
        std::string dataAsStr{msg.begin(), msg.end()};
        spdlog::info("Publishing on '{}' data '{}'", topic, dataAsStr);
    }
#endif
    // first check for publish to $NIOEV
    if(util::startsWith(topic, "$NIOEV")) {
        //performSystemAction(topic, msg);
    }
    // second run scripts
    // TODO optimize forEachSubscriberThatIsOfT so that it skips other subscriptions automatically, avoiding unnecessary work checking for all matches
    auto action = SyncAction::Continue;
    forEachSubscriberThatIsOfT<ScriptContainer>(topic, [&topic, &msg, &action](Subscription& sub) {
        sub.subscriber->publish(topic, msg, *sub.qos, Retained::No);
        // TODO reimplement sync scripts
        // if(runScriptWithPublishedMessage(std::get<MQTTPersistentState::ScriptName>(sub.subscriber), topic, msg, Retained::No) == SyncAction::AbortPublish) {
        //    action = SyncAction::AbortPublish;
        // }
    });
    if(action == SyncAction::AbortPublish) {
        return;
    }
    // then send to clients
    // this order is neccessary to allow the scripts to abort the message delivery to clients
    forEachSubscriberThatIsNotOfT<ScriptContainer>(
        topic, [&topic, &msg](Subscription& sub) { sub.subscriber->publish(topic, msg, *sub.qos, Retained::No); });
    if(retain == Retain::Yes) {
        mQueueInternal.emplace(ChangeRequestRetain{std::move(topic), std::move(msg)});
    }
}
void ApplicationState::handleNewClientConnection(TcpClientConnection&& conn) {
    std::lock_guard<std::shared_mutex> lock{mMutex};
    spdlog::info("New connection from [{}:{}]", conn.getRemoteIp(), conn.getRemotePort());
    auto newClient = mClients.emplace_back(std::make_shared<MQTTClientConnection>(std::move(conn)));
    mClientManager.addClientConnection(*newClient);
}
void ApplicationState::deleteAllSubscriptions(Subscriber& sub) {
    for(auto it = mSimpleSubscriptions.begin(); it != mSimpleSubscriptions.end();) {
        if(it->second.subscriber.get() == &sub) {
            it = mSimpleSubscriptions.erase(it);
        } else {
            it++;
        }
    }
    erase_if(mWildcardSubscriptions, [&sub](auto& subscription) {
        return subscription.subscriber.get() == &sub;
    });
}
ApplicationState::ScriptsInfo ApplicationState::getScriptsInfo() {
    std::shared_lock<std::shared_mutex> lock{mMutex};
    ScriptsInfo ret;
    for(auto& script: mScripts) {
        ScriptsInfo::ScriptInfo scriptInfo;
        scriptInfo.name = script.first;
        scriptInfo.code = script.second->getCode();
        ret.scripts.emplace_back(std::move(scriptInfo));
    }
    return ret;
}
}
