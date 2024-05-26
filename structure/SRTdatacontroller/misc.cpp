#include <thread>
#include "misc.hpp"
#include "socket_stats.hpp"
#include "srt_socket_group.hpp"

// submodules
#include "spdlog/spdlog.h"

using namespace std;
using namespace std::chrono;

namespace srtdatacontroller {

#define LOG_SC_CONN "CONN "

shared_sock_t create_connection(const vector<UriParser>& parsedUrls, shared_sock_t& listeningSock)
{
    if (parsedUrls.empty())
    {
        throw socket::exception("No URL was provided");
    }

    if (parsedUrls.size() > 1 || parsedUrls[0].parameters().count("grouptype"))
    {
#if ENABLE_BONDING
        const bool isListening = !!listeningSock;
        if (!isListening)
            listeningSock = make_shared<socket::srt_group>(parsedUrls);
        socket::srt_group* s = dynamic_cast<socket::srt_group*>(listeningSock.get());
        const bool accept = s->mode() == socket::srt_group::LISTENER;
        if (accept) {
            s->listen();
        }
        shared_sock_t connection = accept ? s->accept() : s->connect();

        if (s->mode() != socket::srt_group::LISTENER)
            listeningSock.reset();

        return connection;
#else
        throw socket::exception("Use -DENABLE_BONDING=ON to enable socket groups!");
#endif // ENABLE_BONDING
    }

    const UriParser& uri = parsedUrls[0];

    if (uri.type() == UriParser::UDP)
    {
        return make_shared<socket::udp>(uri);
    }

    if (uri.type() == UriParser::SRT)
    {
        const bool isListening = !!listeningSock;
        if (!isListening)
            listeningSock = make_shared<socket::srt>(uri);
        socket::srt* s = dynamic_cast<socket::srt*>(listeningSock.get());
        const bool accept = s->mode() == socket::srt::LISTENER;
        if (accept && !isListening)
            s->listen();
        shared_sock_t connection;

        try {
            connection = accept ? s->accept() : s->connect();
        }
        catch (const socket::exception& e)
        {
            listeningSock.reset();
            throw e;
        }

        if (s->mode() != socket::srt::LISTENER)
            listeningSock.reset();

        return connection;
    }

    throw socket::exception(fmt::format("Unknown protocol '{}'.", uri.proto()));
}

void common_run(const vector<string>& urls, const stats_config& cfg, bool reconnect, bool closeListener, const atomic_bool& forceBreak,
                processing_fn_t& processingFn)
{
    if (urls.empty())
    {
        spdlog::error(LOG_SC_CONN "URL was not provided");
        return;
    }

    const bool writeStats = !cfg.stats_file.empty() && cfg.stats_freq_ms > 0;
    unique_ptr<socket::stats_writer> stats;

    if (writeStats)
    {
        try {
            stats = make_unique<socket::stats_writer>(
                cfg.stats_file, cfg.stats_format, milliseconds(cfg.stats_freq_ms));
        }
        catch (const socket::exception& e)
        {
            spdlog::error(LOG_SC_CONN "{}", e.what());
            return;
        }
    }

    vector<UriParser> parsedUrls;
    for (const string& url : urls)
    {
        parsedUrls.emplace_back(url);
    }

    shared_sock_t listeningSock;
    steady_clock::time_point nextReconnect = steady_clock::now();

    do {
        try
        {
            const auto tnow = steady_clock::now();
            if (tnow < nextReconnect)
                this_thread::sleep_until(nextReconnect);

            nextReconnect = tnow + seconds(1);
            shared_sock_t conn = create_connection(parsedUrls, listeningSock);

            if (!conn)
            {
                spdlog::error(LOG_SC_CONN "Failed to create a connection to '{}'.", urls[0]);
                return;
            }

            if (closeListener)
                listeningSock.reset();

            if (stats)
                stats->add_socket(conn);

            processingFn(conn, forceBreak);

            if (stats)
                stats->remove_socket(conn->id());
        }
        catch (const socket::exception& e)
        {
            spdlog::warn(LOG_SC_CONN "{}", e.what());
        }
    } while (reconnect && !forceBreak);
}

netaddr_any create_addr(const string& name, unsigned short port, int prefFamily)
{
    if (name.empty())
    {
        netaddr_any result(prefFamily == AF_INET6 ? prefFamily : AF_INET);
        result.hport(port);
        return result;
    }

    bool first6 = prefFamily != AF_INET;
    int families[2] = {AF_INET6, AF_INET};
    if (!first6)
    {
        families[0] = AF_INET;
        families[1] = AF_INET6;
    }

    for (int i = 0; i < 2; ++i)
    {
        int family = families[i];
        netaddr_any result(family);
        if (inet_pton(family, name.c_str(), result.get_addr()) == 1)
        {
            result.hport(port);
            return result;
        }
    }

    netaddr_any result;
    addrinfo fo = {
        0,
        prefFamily,
        0, 0,
        0, 0,
        nullptr, nullptr
    };

    addrinfo* val = nullptr;
    int erc = getaddrinfo(name.c_str(), nullptr, &fo, &val);
    if (erc == 0)
    {
        result.set(val->ai_addr);
        result.len = result.size();
        result.hport(port);
    }
    freeaddrinfo(val);

    return result;
}

} // namespace srtdatacontroller