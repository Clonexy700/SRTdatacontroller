#pragma once
#include <atomic>
#include <string>

// Third party libraries
#include "CLI/CLI.hpp"

// srtdatacontroller
#include "misc.hpp"

namespace srtdatacontroller
{
namespace generate
{

struct config : public stats_config
{
	int         sendrate       = 0;
	int         num_messages   = -1;
	int         duration       = 0;
	int         message_size   = 1316; ////8 * 1024 * 1024;
	bool        two_way        = false;
	bool        reconnect      = false;
	bool        close_listener = false;
	bool        enable_metrics = false;
	bool        spin_wait      = false;
	std::string playback_csv;
	double      lossRate       = 0.0;
};

void run(const std::vector<std::string>& dst_urls, const config& cfg, const std::atomic_bool& force_break);

CLI::App* add_subcommand(CLI::App& app, config& cfg, std::vector<std::string>& dst_urls);
} // namespace generate
} // namespace srtdatacontroller
