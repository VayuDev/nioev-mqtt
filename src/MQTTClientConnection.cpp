#include "MQTTClientConnection.hpp"
#include "ApplicationState.hpp"
#include "nioev/lib/Util.hpp"
#include "MQTTPublishPacketBuilder.hpp"

namespace nioev::mqtt {

using namespace nioev::lib;

void MQTTClientConnection::publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, const PropertyList& properties, MQTTPublishPacketBuilder& packetBuilder) {
    if(qos == QoS::QoS0) {
        sendData(InTransitEncodedPacket{packetBuilder.getPacket(qos, 0, mMQTTVersion)});
        return;
    }
    // FIXME choose packet id in closer accordance to spec. Concretely, this means that we must always pick an unused id
    uint16_t packetId = mPacketIdCounter++;
    EncodedPacket packet = packetBuilder.getPacket(qos, packetId, mMQTTVersion);
    sendData(InTransitEncodedPacket{packet});
    if(qos == QoS::QoS1) {
        auto[persistentState, lock] = getPersistentState();
        persistentState->highQoSSendingPackets.emplace(packetId, HighQoSRetainStorage{std::move(packet), qos});
    } else if(qos == QoS::QoS2) {
        auto[persistentState, lock] = getPersistentState();
        persistentState->highQoSSendingPackets.emplace(packetId, HighQoSRetainStorage{std::move(packet), qos});
    }
}

void MQTTClientConnection::sendData(EncodedPacket packet) {
    sendData(InTransitEncodedPacket{std::move(packet)});
}
void MQTTClientConnection::sendData(InTransitEncodedPacket packet) {
    try {
        uint totalBytesSent = 0;
        uint bytesSent = 0;

        std::unique_lock<std::timed_mutex> lock{mSendMutex};

        // TODO make configurable
        /*if(type == SendDataType::PUBLISH && mSendTasks.size() > 1000) {
            spdlog::warn("[{}] Dropping packet due to large queue depth", mClientId);
            return false;
        }*/
        if(mSendTasks.empty()) {
            getTcpClient().sendScatter(&packet, 1);
        }
        if(!packet.isDone()) {
            mSendTasks.emplace_back(std::move(packet));
        }
    } catch(std::exception& e) {
        spdlog::error("[{}] Error while sending data: {}", getClientId(), e.what());
        // we aren't allowed to enqueue a change request here, because we could be inside ApplicationState::publish, where a shared lock is held.
        // that's why we just set a flag which causes the logout to be enequeued later on
        mSendError = true;
    }
}

}