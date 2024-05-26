#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <future>
#include <iomanip>
#include <memory>
#include <thread>
#include <vector>

// submodules
#include "spdlog/spdlog.h"

// srtdatacontroller
#include "socket_stats.hpp"
#include "misc.hpp"
#include "receive.hpp"
#include "metrics.hpp"

// OpenSRT
#include "apputil.hpp"
#include "uriparser.hpp"

using namespace std;
using namespace srtdatacontroller;
using namespace srtdatacontroller::receive;
using namespace std::chrono;

using SharedSrt = std::shared_ptr<socket::srt>;
using SharedSock = std::shared_ptr<socket::isocket>;

#define LOG_SC_RECEIVE "RECEIVE "

void traceMessage(const size_t bytes, const vector<char>& buffer, SOCKET connId)
{
    cout << "RECEIVED MESSAGE length " << bytes << " on conn ID " << connId;

#if 0
    if (bytes < 50)
    {
        cout << ":\n";
        cout << string(buffer.data(), bytes).c_str();
    }
    else if (buffer[0] >= '0' && buffer[0] <= 'z')
    {
        cout << " (first character):";
        cout << buffer[0];
    }
#endif
    cout << endl;

    // CHandShake hs;
    // if (hs.load_from(buffer.data(), buffer.size()) < 0)
    //    return;
    //
    // cout << "SRT HS: " << hs.show() << endl;
}

void metricsWritingLoop(ofstream& metricsFile,
                        metrics::validator& validator,
                        mutex& mtx,
                        const chrono::milliseconds& freq,
                        const atomic_bool& forceBreak)
{
    auto statTime = steady_clock::now();
    while (!forceBreak)
    {
        const auto tnow = steady_clock::now();
        if (tnow >= statTime)
        {
            if (metricsFile.is_open())
            {
                lock_guard<mutex> lck(mtx);
                metricsFile << validator.stats_csv(false);
            }
            else
            {
                lock_guard<mutex> lck(mtx);
                const auto statsStr = validator.stats();
                spdlog::info(LOG_SC_RECEIVE "{}", statsStr);
            }
            statTime += freq;
        }

        this_thread::sleep_until(statTime);
    }
}

void runPipe(SharedSock src, const config& cfg, const atomic_bool& forceBreak)
{
    socket::isocket& sock = *src.get();

    vector<char> buffer(cfg.message_size);
    metrics::validator validator;

    atomic_bool metricsStop(false);
    mutex metricsMtx;
    future<void> metricsTh;
    ofstream metricsFile;
    if (cfg.enable_metrics && cfg.metrics_freq_ms > 0)
    {
        if (!cfg.metrics_file.empty())
        {
            metricsFile.open(cfg.metrics_file, ofstream::out);
            if (!metricsFile)
            {
                spdlog::error(LOG_SC_RECEIVE "Failed to open metrics file {} for output", cfg.metrics_file);
                return;
            }
            metricsFile << validator.stats_csv(true);
        }

        metricsTh = async(launch::async,
                          metricsWritingLoop,
                          ref(metricsFile),
                          ref(validator),
                          ref(metricsMtx),
                          chrono::milliseconds(cfg.metrics_freq_ms),
                          ref(metricsStop));
    }

    try
    {
        while (!forceBreak)
        {
            const size_t bytes = sock.read(mutable_buffer(buffer.data(), buffer.size()), -1);

            if (bytes == 0)
            {
                spdlog::debug(LOG_SC_RECEIVE "sock::read() returned 0 bytes (spurious read ready?). Retrying.");
                continue;
            }

            if (cfg.print_notifications)
                traceMessage(bytes, buffer, sock.id());
            if (cfg.enable_metrics)
            {
                lock_guard<mutex> lck(metricsMtx);
                validator.validate_packet(const_buffer(buffer.data(), bytes));
            }

            if (cfg.send_reply)
            {
                const string outMessage("Message received");
                sock.write(const_buffer(outMessage.data(), outMessage.size()));

                if (cfg.print_notifications)
                    spdlog::error(LOG_SC_RECEIVE "Reply sent on conn ID {}", sock.id());
            }
        }
    }
    catch (const socket::exception& e)
    {
        spdlog::warn(LOG_SC_RECEIVE "{}", e.what());
    }

    metricsStop = true;
    if (metricsTh.valid())
        metricsTh.get();

    if (forceBreak)
    {
        spdlog::info(LOG_SC_RECEIVE "interrupted by request!");
    }
}

void srtdatacontroller::receive::run(const vector<string>& srcUrls,
                             const config& cfg,
                             const atomic_bool& forceBreak)
{
    using namespace std::placeholders;
    processing_fn_t processFn = bind(runPipe, _1, cfg, _2);
    common_run(srcUrls, cfg, cfg.reconnect, cfg.close_listener, forceBreak, processFn);
}

CLI::App* srtdatacontroller::receive::add_subcommand(CLI::App& app, config& cfg, vector<string>& srcUrls)
{
    const map<string, int> toMs{{"s", 1000}, {"ms", 1}};

    CLI::App* scReceive = app.add_subcommand("receive", "Receive data (SRT, UDP)")->fallthrough();
    scReceive->add_option("-i,--input,src", srcUrls, "Source URI");
    scReceive->add_option("--msgsize", cfg.message_size, fmt::format("Size of the buffer to receive message payload (default {})", cfg.message_size));
    scReceive->add_option("--statsfile", cfg.stats_file, "Output stats report filename");
    scReceive->add_option("--statsformat", cfg.stats_format, "Output stats report format (csv - default, json)");
    scReceive->add_option("--statsfreq", cfg.stats_freq_ms, fmt::format("Output stats report frequency, ms (default {})", cfg.stats_freq_ms))
        ->transform(CLI::AsNumberWithUnit(toMs, CLI::AsNumberWithUnit::CASE_SENSITIVE));
    scReceive->add_flag("--printmsg", cfg.print_notifications, "Print message to stdout");
    scReceive->add_flag("--reconnect,!--no-reconnect", cfg.reconnect, "Reconnect automatically");
    scReceive->add_flag("--close-listener,!--no-close-listener", cfg.close_listener, "Close listener once connection is established");
    scReceive->add_flag("--enable-metrics", cfg.enable_metrics, "Enable checking metrics: jitter, latency, etc.");
    scReceive->add_option("--metricsfile", cfg.metrics_file, "Metrics output filename (default stdout)");
    scReceive->add_option("--metricsfreq", cfg.metrics_freq_ms, fmt::format("Metrics report frequency, ms (default {})", cfg.metrics_freq_ms))
        ->transform(CLI::AsNumberWithUnit(toMs, CLI::AsNumberWithUnit::CASE_SENSITIVE));
    scReceive->add_flag("--twoway", cfg.send_reply, "Both send and receive data");

    return scReceive;
}