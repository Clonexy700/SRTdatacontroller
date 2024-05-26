#pragma once
#include <atomic>
#include <string>

// Third party libraries
#include "CLI/CLI.hpp"

// srtdatacontroller
#include "misc.hpp"

namespace srtdatacontroller
{
namespace receive
{

struct config : stats_config
{
	bool        print_notifications = false; // Print notifications about the messages received
	bool        send_reply          = false;
	bool        reconnect           = false;
	bool        close_listener      = false;
	bool        enable_metrics      = false;
	unsigned    metrics_freq_ms     = 1000;
	std::string metrics_file;
	int         max_connections = 1; // Maximum number of connections on a socket
	int         message_size    = 1316;
};

void run(const std::vector<std::string>& src_urls, const config& cfg, const std::atomic_bool& force_break);

CLI::App* add_subcommand(CLI::App& app, config& cfg, std::vector<std::string>& src_urls);

} // namespace receive
} // namespace srtdatacontroller
