#include "forward.h"
#include "srt_node.hpp"

#include "spdlog/spdlog.h"

using namespace std;
using namespace srtdatacontroller::forward;

#define LOG_SC_FORWARD "[FORWARD] "

shared_ptr<SrtNode> createNode(const config& cfg, const char* uri, bool isCaller) {
    UriParser urlParser(uri);
    urlParser["mode"] = isCaller ? "caller" : "listener";

    if (cfg.planck) {
        urlParser["transtype"] = "file";
        urlParser["messageapi"] = "true";

        if (!urlParser["sndbuf"].exists())
            urlParser["sndbuf"] = to_string(3 * (cfg.message_size * 1472 / 1456 + 1472));
        if (!urlParser["rcvbuf"].exists())
            urlParser["rcvbuf"] = to_string(3 * (cfg.message_size * 1472 / 1456 + 1472));
    }

    return make_shared<SrtNode>(urlParser);
}

void forwardMessage(shared_ptr<SrtNode> src, shared_ptr<SrtNode> dst, SRTSOCKET dstSock, const config& cfg, const string& description, const atomic_bool& forceBreak) {
    vector<char> messageReceived(cfg.message_size);

    while (!forceBreak) {
        int connectionId = 0;
        const int recvRes = src->Receive(messageReceived.data(), messageReceived.size(), &connectionId);
        if (recvRes <= 0) {
            if (recvRes == 0 && connectionId == 0)
                break;

            spdlog::error(LOG_SC_FORWARD "ERROR: Receiving message resulted in {} on connection ID {}. {}",
                          recvRes, connectionId, srt_getlasterror_str());

            break;
        }

        if (recvRes > static_cast<int>(messageReceived.size())) {
            spdlog::error(LOG_SC_FORWARD "ERROR: Size of the received message {} exceeds the buffer size {} on connection ID {}",
                          recvRes, messageReceived.size(), connectionId);
            break;
        }

        if (recvRes < 50) {
            spdlog::debug(LOG_SC_FORWARD "RECEIVED MESSAGE on connection ID {}: {}", connectionId, string(messageReceived.data(), recvRes).c_str());
        }
        else if (messageReceived[0] >= '0' && messageReceived[0] <= 'z') {
            spdlog::debug(LOG_SC_FORWARD "RECEIVED MESSAGE length on connection ID {} (first character): {}", dst->GetBindSocket());
        }

        spdlog::debug(LOG_SC_FORWARD "Forwarding message to {} (first character): {}", recvRes, connectionId, messageReceived[0]);

        const int sendRes = dst->Send(messageReceived.data(), recvRes, dstSock);
        if (sendRes <= 0) {
            spdlog::error(LOG_SC_FORWARD "ERROR: Sending message resulted in {} on connection ID {}. Error message: {}",
                          sendRes, dst->GetBindSocket(), srt_getlasterror_str());

            break;
        }
    }

    if (forceBreak)
        spdlog::debug(LOG_SC_FORWARD "Breaking on request.");
    else
        spdlog::debug(LOG_SC_FORWARD "Force reconnection.");

    src->Close();
    dst->Close();
}

int startForwarding(const config& cfg, const char* srcUri, const char* dstUri, const atomic_bool& forceBreak) {
    shared_ptr<SrtNode> dst = createNode(cfg, dstUri, true);
    if (!dst) {
        spdlog::error(LOG_SC_FORWARD "ERROR! Failed to create destination node.");
        return 1;
    }

    shared_ptr<SrtNode> src = createNode(cfg, srcUri, false);
    if (!src) {
        spdlog::error(LOG_SC_FORWARD "ERROR! Failed to create source node.");
        return 1;
    }

    const int sockDst = dst->Connect();
    if (sockDst == SRT_INVALID_SOCK) {
        spdlog::error(LOG_SC_FORWARD "ERROR! While setting up a caller.");
        return 1;
    }

    if (src->Listen(1) != 0) {
        spdlog::error(LOG_SC_FORWARD "ERROR! While setting up a listener: {}.", srt_getlasterror_str());
        return 1;
    }

    auto futureSrcSocket = src->AcceptConnection(forceBreak);
    const SRTSOCKET sockSrc = futureSrcSocket.get();
    if (sockSrc == SRT_ERROR) {
        spdlog::error(LOG_SC_FORWARD "Wait for source connection canceled");
        return 0;
    }

    thread thSrcToDst(forwardMessage, src, dst, dst->GetBindSocket(), cfg, "[SRC->DST] ", std::ref(forceBreak));
    thread thDstToSrc(forwardMessage, dst, src, sockSrc, cfg, "[DST->SRC] ", std::ref(forceBreak));

    thSrcToDst.join();
    thDstToSrc.join();

    auto waitUndelivered = [](shared_ptr<SrtNode> node, int waitMs, const string& desc) {
        const int undelivered = node->WaitUndelivered(waitMs);
        if (undelivered == -1) {
            spdlog::error(LOG_SC_FORWARD "ERROR: waiting undelivered data resulted in {}", srt_getlasterror_str());
        }
        if (undelivered) {
            spdlog::error(LOG_SC_FORWARD "ERROR: still has {} bytes undelivered", undelivered);
        }

        node.reset();
        return undelivered;
    };

    std::future<int> srcUndelivered = async(launch::async, waitUndelivered, src, 3000, "[SRC] ");
    std::future<int> dstUndelivered = async(launch::async, waitUndelivered, dst, 3000, "[DST] ");

    srcUndelivered.wait();
    dstUndelivered.wait();

    return 0;
}

void srtdatacontroller::forward::run(const string& src, const string& dst, const config& cfg, const atomic_bool& forceBreak) {
    while (!forceBreak) {
        startForwarding(cfg, src.c_str(), dst.c_str(), forceBreak);
    }
}

CLI::App* srtdatacontroller::forward::addSubcommand(CLI::App& app, config& cfg, string& srcUrl, string& dstUrl) {
    CLI::App* scForward = app.add_subcommand("forward", "Bidirectional file forwarding. srt://:<src_port> srt://<dst_ip>:<dst_port>");
    scForward->add_option("src", srcUrl, "Source URI");
    scForward->add_option("dst", dstUrl, "Destination URI");
    scForward->add_flag("--oneway", cfg.one_way, "Forward only from SRT to DST");
    scForward->add_flag("--planck", cfg.planck, "Apply default config for SRT Planck use case");

    return scForward;
}