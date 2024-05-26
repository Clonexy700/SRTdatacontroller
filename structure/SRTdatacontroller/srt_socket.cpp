#include <thread>
#include <iostream>
#include <iterator> // std::ostream_iterator

// submodules
#include "spdlog/spdlog.h"

// srtdatacontroller
#include "srt_socket.hpp"
#include "misc.hpp"

// srt utils
#include "verbose.hpp"
#include "socketoptions.hpp"
#include "apputil.hpp"

// nlohmann_jsonq
#include <nlohmann/json.hpp>

using namespace std;
using namespace srtdatacontroller;
using shared_srt = shared_ptr<socket::srt>;


#define LOG_SOCK_SRT "SOCKET::SRT "


socket::srt::srt(const UriParser &src_uri)
	: m_host(src_uri.host())
	, m_port(src_uri.portno())
	, m_options(src_uri.parameters())
{
	m_bind_socket = srt_create_socket();
	if (m_bind_socket == SRT_INVALID_SOCK)
		throw socket::exception(srt_getlasterror_str());

	if (m_options.count("blocking"))
	{
		m_blocking_mode = !false_names.count(m_options.at("blocking"));
		m_options.erase("blocking");
	}

	assert_options_valid();

	// configure_pre(..) determines connection mode (m_mode).
	if (SRT_SUCCESS != configure_pre(m_bind_socket))
		throw socket::exception(srt_getlasterror_str());

	if (!m_blocking_mode)
	{
		m_epoll_connect = srt_epoll_create();
		if (m_epoll_connect == -1)
			throw socket::exception(srt_getlasterror_str());

		const bool to_accept = m_mode == connection_mode::LISTENER;
		int modes = SRT_EPOLL_ERR | (to_accept ? SRT_EPOLL_IN : SRT_EPOLL_OUT);
		if (SRT_ERROR == srt_epoll_add_usock(m_epoll_connect, m_bind_socket, &modes))
			throw socket::exception(srt_getlasterror_str());

		m_epoll_io = srt_epoll_create();
		modes      = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
		if (SRT_ERROR == srt_epoll_add_usock(m_epoll_io, m_bind_socket, &modes))
			throw socket::exception(srt_getlasterror_str());
	}
	
	// Do binding after PRE options are configured in the above call.
	handle_hosts();
}

socket::srt::srt(const int sock, bool blocking)
	: m_bind_socket(sock)
	, m_blocking_mode(blocking)
{
	if (!m_blocking_mode)
	{
		m_epoll_io = srt_epoll_create();
		int modes  = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;
		if (SRT_ERROR == srt_epoll_add_usock(m_epoll_io, m_bind_socket, &modes))
			throw socket::exception(srt_getlasterror_str());
	}
}

socket::srt::~srt()
{
	if (!m_blocking_mode)
	{
		spdlog::debug(LOG_SOCK_SRT "@{} Releasing epolls before closing", m_bind_socket);
		if (m_epoll_connect != -1)
			srt_epoll_release(m_epoll_connect);
		srt_epoll_release(m_epoll_io);
	}
	spdlog::debug(LOG_SOCK_SRT "@{} Closing", m_bind_socket);
	srt_close(m_bind_socket);
}

void socket::srt::listen()
{
	int         num_clients = 2;
	int res = srt_listen(m_bind_socket, num_clients);
	if (res == SRT_ERROR)
	{
		srt_close(m_bind_socket);
		raise_exception("listen");
	}

	spdlog::debug(LOG_SOCK_SRT "@{} (srt://{}:{:d}) Listening", m_bind_socket, m_host, m_port);
	res = configure_post(m_bind_socket);
	if (res == SRT_ERROR)
		raise_exception("listen::configure_post");
}

shared_srt socket::srt::accept()
{
	spdlog::debug(LOG_SOCK_SRT "@{} (srt://{}:{:d}) {} Waiting for incoming connection",
		m_bind_socket, m_host, m_port, m_blocking_mode ? "SYNC" : "ASYNC");
	
	// Wait for REAL connected state if nonblocking mode
	if (!m_blocking_mode)
	{
		// Socket readiness to accept a new connection is notified with READ event.
		// See also: https://github.com/Haivision/srt/pull/1831
		constexpr int timeout_ms = -1;
		int           len        = 2;
		SRTSOCKET     ready[2];
		if (srt_epoll_wait(m_epoll_connect, ready, &len, 0, 0, timeout_ms, 0, 0, 0, 0) == -1)
		{
			// if (srt_getlasterror(nullptr) == SRT_ETIMEOUT)
			//	continue;

			raise_exception("accept::epoll_wait");
		}

		spdlog::debug(LOG_SOCK_SRT "@{} (srt://{}:{:d}) {} ready, [0]: 0x{:X}",
			m_bind_socket, m_host, m_port, len, ready[0]);
	}

	sockaddr_in scl;
	int         sclen = sizeof scl;
	const SRTSOCKET sock = srt_accept(m_bind_socket, (sockaddr *)&scl, &sclen);
	if (sock == SRT_INVALID_SOCK)
	{
		raise_exception("accept");
	}


	const int res = configure_post(sock);
	if (res == SRT_ERROR)
		raise_exception("accept::configure_post");

	spdlog::info(LOG_SOCK_SRT "@{} (srt://{}:{:d}) Accepted connection @{}. {}.",
		m_bind_socket, m_host, m_port, sock, print_negotiated_config(sock));

	return make_shared<srt>(sock, m_blocking_mode);
}

void socket::srt::raise_exception(const string &&place) const
{
	const int    udt_result = srt_getlasterror(nullptr);
	const string message = srt_getlasterror_str();
	spdlog::debug(LOG_SOCK_SRT "@{} {} ERROR {} {}", m_bind_socket, place, udt_result, message);
	throw socket::exception(place + ": " + message);
}

void socket::srt::raise_exception(const string &&place, const string &&reason) const
{
	spdlog::debug(LOG_SOCK_SRT "@{} {}. ERROR: {}.", m_bind_socket, place, reason);
	throw socket::exception(place + ": " + reason);
}

shared_srt socket::srt::connect()
{
	netaddr_any sa;
	try
	{
		sa = create_addr(m_host, m_port);
	}
	catch (const std::invalid_argument &e)
	{
		raise_exception("connect::create_addr", e.what());
	}

	spdlog::debug(LOG_SOCK_SRT "@{} {} Connecting to srt://{}:{:d}",
		m_bind_socket, m_blocking_mode ? "SYNC" : "ASYNC", m_host, m_port);

	{
		const int res = srt_connect(m_bind_socket, sa.get(), sa.size());
		if (res == SRT_ERROR)
		{
			// srt_getrejectreason() added in v1.3.4
			const auto reason = srt_getrejectreason(m_bind_socket);
			srt_close(m_bind_socket);
			raise_exception("connect failed", string(srt_getlasterror_str()) + ". Reject reason: " + srt_rejectreason_str(reason));
		}
	}

	// Wait for REAL connected state if nonblocking mode
	if (!m_blocking_mode)
	{
		// Socket readiness for connection is checked by polling on WRITE allowed sockets.
		int       len = 2;
		SRTSOCKET ready[2];
		if (srt_epoll_wait(m_epoll_connect, 0, 0, ready, &len, -1, 0, 0, 0, 0) != -1)
		{
			const SRT_SOCKSTATUS state = srt_getsockstate(m_bind_socket);
			if (state != SRTS_CONNECTED)
			{
				const auto reason = srt_getrejectreason(m_bind_socket);
				raise_exception("connect failed", srt_rejectreason_str(reason));
			}
		}
		else
		{
			raise_exception("connect.epoll_wait");
		}
	}

	{
		const int res = configure_post(m_bind_socket);
		if (res == SRT_ERROR)
			raise_exception("connect::onfigure_post");
	}

	spdlog::info(LOG_SOCK_SRT "@{} {} Connected to srt://{}:{:d}. {}.",
		m_bind_socket, m_blocking_mode ? "SYNC" : "ASYNC", m_host, m_port,
		print_negotiated_config(m_bind_socket));

	return shared_from_this();
}

std::future<shared_srt> socket::srt::async_connect()
{
	auto self = shared_from_this();

	return async(std::launch::async, [self]() { return self->connect(); });
}

std::future<shared_srt> socket::srt::async_accept()
{
	listen();

	auto self = shared_from_this();
	return async(std::launch::async, [self]() { return self->accept(); });
}

std::future<shared_srt> socket::srt::async_read(std::vector<char> &buffer)
{
	return std::future<shared_srt>();
}

void socket::srt::assert_options_valid(const std::map<string, string>& options, const unordered_set<string>& extra)
{
#ifdef ENABLE_CXX17
	for (const auto& [key, val] : options)
	{
#else
	for (const auto& el : options)
	{
		const string& key = el.first;
		const string& val = el.second;
#endif
		bool opt_found = false;
		for (const auto& o : srt_options)
		{
			if (o.name != key)
				continue;

			opt_found = true;
			break;
		}
		
		if (opt_found || extra.count(key))
			continue;

		stringstream ss;
		ss << "Invalid URI query option '";
		ss << key << "=" << val << " (not recognized)!";
		throw socket::exception(ss.str());
	}
}

void socket::srt::assert_options_valid() const
{
	assert_options_valid(m_options, {"bind", "mode"});
}

int socket::srt::configure_pre(SRTSOCKET sock)
{
	int maybe  = m_blocking_mode ? 1 : 0;
	int result = srt_setsockopt(sock, 0, SRTO_RCVSYN, &maybe, sizeof maybe);
	if (result == -1)
		return result;

	// host is only checked for emptiness and depending on that the connection mode is selected.
	// Here we are not exactly interested with that information.
	std::vector<string> failures;

	// NOTE: here host = "", so the 'connmode' will be returned as LISTENER always,
	// but it doesn't matter here. We don't use 'connmode' for anything else than
	// checking for failures.
	SocketOption::Mode conmode = SrtConfigurePre(sock, m_host, m_options, &failures);

	if (conmode == SocketOption::FAILURE)
	{
		stringstream ss;
		ss << "Wrong value of option(s): ";
		copy(failures.begin(), failures.end(), ostream_iterator<string>(ss, ", "));
		throw socket::exception(ss.str());
	}

	m_mode = static_cast<connection_mode>(conmode);

	if (m_mode == connection_mode::RENDEZVOUS)
	{
		int yes = 1;
		result = srt_setsockopt(sock, 0, SRTO_RENDEZVOUS, &yes, sizeof yes);
		if (result == -1)
			return result;
	}

	return SRT_SUCCESS;
}

std::string socket::srt::print_negotiated_config(SRTSOCKET sock)
{
	static const map<int, const char*> cryptomodes = {
		{0, "AUTO"},
		{1, "AES-CTR"},
		{2, "AES-GCM"}
	};

	static const map<int, const char*> km_states = {
		{0, "UNSECURED"}, //No encryption
		{1, "SECURING"},  //Stream encrypted, exchanging Keying Material
		{2, "SECURED"},   //Stream encrypted, keying Material exchanged, decrypting ok.
		{3, "NOSECRET"},  //Stream encrypted and no secret to decrypt Keying Material
		{4, "BADSECRET"}  //Stream encrypted and wrong secret, cannot decrypt Keying Material        
	};

	auto convert = [](int v, const map<int, const char*>& values) {
		const auto m = values.find(v);
		if (m == values.end())
			return "INVALID";

		return m->second;
	};

	auto get_sock_value = [](int s, SRT_SOCKOPT sopt, const char* const sopt_str) {
		int ival = 0;
		int ilen = sizeof ival;
		const int res = srt_getsockflag(s, sopt, &ival, &ilen);
		if (res != SRT_SUCCESS)
		{
			spdlog::error(LOG_SOCK_SRT "Failed to get sockopt {}.", sopt_str);
			return -1;
		}
		return ival;
	};

#define VAL_AND_STR(X) X, #X
	const int pbkeylen     = get_sock_value(sock, VAL_AND_STR(SRTO_PBKEYLEN));
	const int km_state     = get_sock_value(sock, VAL_AND_STR(SRTO_KMSTATE));
	const int km_state_rcv = get_sock_value(sock, VAL_AND_STR(SRTO_RCVKMSTATE));
	const int km_state_snd = get_sock_value(sock, VAL_AND_STR(SRTO_SNDKMSTATE));

	std::string latency_str;
	if (get_sock_value(sock, VAL_AND_STR(SRTO_TSBPDMODE)) > 0)
	{
		const int latency_rcv  = get_sock_value(sock, VAL_AND_STR(SRTO_RCVLATENCY));
		const int latency_peer = get_sock_value(sock, VAL_AND_STR(SRTO_PEERLATENCY));
		latency_str = fmt::format("Latency RCV {}ms, peer {}ms", latency_rcv, latency_peer);
	}
	else
	{
		latency_str = "off";
	}

#if ENABLE_AEAD_API_PREVIEW
	const int crypto_mode  = get_sock_value(sock, VAL_AND_STR(SRTO_CRYPTOMODE));
	const auto crypto_mode_str = fmt::format("{}", convert(crypto_mode, cryptomodes));
#else
	const string crypto_mode_str = "";
#endif
#undef VAL_AND_STR
	
	// Template lambdas are only available since C++20, have to duplicate the code.
	std::string streamid(512, '\0');
	int         streamid_len = (int) streamid.size();
	if (srt_getsockflag(sock, SRTO_STREAMID, (void*)streamid.data(), &streamid_len) != SRT_SUCCESS)
	{
		spdlog::error(LOG_SOCK_SRT "Failed to get sockopt SRTO_STREAMID.");
		streamid_len = 0;
	}

	return fmt::format("TSBPD {}. KM state {} (RCV {}, SND {}). PB key length: {}. Cryptomode {}. Stream ID: {}",
					   latency_str,
					   convert(km_state, km_states),
					   convert(km_state_rcv, km_states),
					   convert(km_state_snd, km_states),
					   pbkeylen, crypto_mode_str,
					   streamid_len > 0 ? streamid : "not set");
}

int socket::srt::configure_post(SRTSOCKET sock) const
{
	int is_blocking = m_blocking_mode ? 1 : 0;

	int result = srt_setsockopt(sock, 0, SRTO_SNDSYN, &is_blocking, sizeof is_blocking);
	if (result == -1)
		return result;
	result = srt_setsockopt(sock, 0, SRTO_RCVSYN, &is_blocking, sizeof is_blocking);
	if (result == -1)
		return result;

	// host is only checked for emptiness and depending on that the connection mode is selected.
	// Here we are not exactly interested with that information.
	vector<string> failures;

	SrtConfigurePost(sock, m_options, &failures);

	if (!failures.empty())
	{
		stringstream ss;
		copy(failures.begin(), failures.end(), ostream_iterator<string>(ss, ", "));
		spdlog::warn(LOG_SOCK_SRT "failed to set options: {}.", ss.str());
	}

	return 0;
}

void socket::srt::handle_hosts()
{
	const auto bind_me = [&](const sockaddr* sa) {
		const int bind_res = srt_bind(m_bind_socket, sa, sizeof * sa);
		if (bind_res < 0)
		{
			srt_close(m_bind_socket);
			raise_exception("srt::bind");
		}
	};

	if (m_options.count("bind"))
	{
		string bindipport = m_options.at("bind");
		transform(bindipport.begin(), bindipport.end(), bindipport.begin(), [](char c) { return tolower(c); });
		const size_t idx = bindipport.find(":");
		const string bindip = bindipport.substr(0, idx);
		const int bindport = idx != string::npos
			? stoi(bindipport.substr(idx + 1, bindipport.size() - (idx + 1)))
			: m_port;
		m_options.erase("bind");

		netaddr_any sa_bind;
		try
		{
			sa_bind = create_addr(bindip, bindport);
		}
		catch (const std::invalid_argument&)
		{
			throw socket::exception("create_addr_inet failed");
		}

		bind_me(reinterpret_cast<const sockaddr*>(&sa_bind));
		spdlog::info(LOG_SOCK_SRT "srt://{}:{:d}: bound to '{}:{}'.",
			m_host, m_port, bindip, bindport);
	}
	else if (m_mode == connection_mode::RENDEZVOUS)
	{
		// Implicitely bind to the same port as remote.
		netaddr_any sa;
		try
		{
			sa = create_addr("", m_port);
		}
		catch (const std::invalid_argument& e)
		{
			raise_exception("create_addr", e.what());
		}
		bind_me(reinterpret_cast<const sockaddr*>(&sa));
		spdlog::info(LOG_SOCK_SRT "srt://{}:{:d}: bound to '0.0.0.0:{}' (rendezvous default).",
			m_host, m_port, m_port);
	}
	else if (m_mode == connection_mode::LISTENER)
	{
		netaddr_any sa;
		try
		{
			sa = create_addr(m_host, m_port);
		}
		catch (const std::invalid_argument& e)
		{
			raise_exception("create_addr", e.what());
		}
		bind_me(reinterpret_cast<const sockaddr*>(&sa));
		spdlog::info(LOG_SOCK_SRT "srt://{0}:{1:d}: bound to '{0}:{1:d}'.",
			m_host, m_port);
	}
}

size_t socket::srt::read(const mutable_buffer &buffer, int timeout_ms)
{
	if (!m_blocking_mode)
	{
		int ready[2] = {SRT_INVALID_SOCK, SRT_INVALID_SOCK};
		int len      = 2;

		const int epoll_res = srt_epoll_wait(m_epoll_io, ready, &len, nullptr, nullptr, timeout_ms, 0, 0, 0, 0);
		if (epoll_res == SRT_ERROR)
		{
			if (srt_getlasterror(nullptr) == SRT_ETIMEOUT)
				return 0;

			raise_exception("read::epoll");
		}
	}

	const int res = srt_recvmsg2(m_bind_socket, static_cast<char *>(buffer.data()), (int)buffer.size(), nullptr);
	if (SRT_ERROR == res)
	{
		if (srt_getlasterror(nullptr) != SRT_EASYNCRCV)
			raise_exception("read::recv");

		spdlog::trace(LOG_SOCK_SRT "recvmsg error 6002: try again (spurious read-ready)");
		return 0;
	}

	return static_cast<size_t>(res);
}

int socket::srt::write(const const_buffer &buffer, int timeout_ms)
{
	if (!m_blocking_mode)
	{
		int ready[2] = {SRT_INVALID_SOCK, SRT_INVALID_SOCK};
		int len      = 2;
		int rready[2] = {SRT_INVALID_SOCK, SRT_INVALID_SOCK};
		int rlen      = 2;
		// TODO: check error fds
		const int res = srt_epoll_wait(m_epoll_io, rready, &rlen, ready, &len, timeout_ms, 0, 0, 0, 0);
		if (res == SRT_ERROR)
			raise_exception("write::epoll");
	}

	const int res = srt_sendmsg2(m_bind_socket, static_cast<const char*>(buffer.data()), static_cast<int>(buffer.size()), nullptr);
	if (res == SRT_ERROR)
	{
		if (srt_getlasterror(nullptr) == SRT_EASYNCSND)
			return 0;

		raise_exception("write::send", srt_getlasterror_str());
	}

	return res;
}

socket::srt::connection_mode socket::srt::mode() const
{
	return m_mode;
}

int socket::srt::statistics(SRT_TRACEBSTATS& stats, bool instant)
{
	return srt_bstats(m_bind_socket, &stats, instant);
}

const string socket::srt::stats_to_csv(int socketid, const SRT_TRACEBSTATS& stats, bool print_header)
{
	std::ostringstream output;

#define HAS_PKT_REORDER_TOL (SRT_VERSION_MAJOR >= 1) && (SRT_VERSION_MINOR >= 4) && (SRT_VERSION_PATCH > 0)
// pktSentUnique, pktRecvUnique were added in SRT v1.4.2
#define HAS_UNIQUE_PKTS (SRT_VERSION_MAJOR == 1) && ((SRT_VERSION_MINOR > 4) || ((SRT_VERSION_MINOR == 4) && (SRT_VERSION_PATCH >= 2)))

	if (print_header)
	{
#ifdef HAS_PUT_TIME
		output << "Timepoint,";
#endif
		output << "Time,SocketID,weight,pktFlowWindow,pktCongestionWindow,pktFlightSize,";
		output << "msRTT,mbpsBandwidth,mbpsMaxBW,pktSent,";
#if HAS_UNIQUE_PKTS
		output << "pktSentUnique,";
#endif
		output << "pktSndLoss,pktSndDrop,pktRetrans,byteSent,";
		output << "byteAvailSndBuf,byteSndDrop,mbpsSendRate,usPktSndPeriod,msSndBuf,pktRecv,";
#if HAS_UNIQUE_PKTS
		output << "pktRecvUnique,";
#endif
		output << "pktRcvLoss,pktRcvDrop,pktRcvUndecrypt,pktRcvRetrans,pktRcvBelated,";
		output << "byteRecv,byteAvailRcvBuf,byteRcvLoss,byteRcvDrop,mbpsRecvRate,msRcvBuf,msRcvTsbPdDelay";
#if HAS_PKT_REORDER_TOL
		output << ",pktReorderTolerance";
#endif
		output << endl;
		return output.str();
	}

#ifdef HAS_PUT_TIME
	output << print_timestamp_now() << ',';
#endif // HAS_PUT_TIME

	output << stats.msTimeStamp << ',';
	output << socketid << ',';
	output << 0 << ','; // weight
	output << stats.pktFlowWindow << ',';
	output << stats.pktCongestionWindow << ',';
	output << stats.pktFlightSize << ',';

	output << stats.msRTT << ',';
	output << stats.mbpsBandwidth << ',';
	output << stats.mbpsMaxBW << ',';
	output << stats.pktSent << ',';
#if HAS_UNIQUE_PKTS
	output << stats.pktSentUnique << ",";
#endif
	output << stats.pktSndLoss << ',';
	output << stats.pktSndDrop << ',';

	output << stats.pktRetrans << ',';
	output << stats.byteSent << ',';
	output << stats.byteAvailSndBuf << ',';
	output << stats.byteSndDrop << ',';
	output << stats.mbpsSendRate << ',';
	output << stats.usPktSndPeriod << ',';
	output << stats.msSndBuf << ',';

	output << stats.pktRecv << ',';
#if HAS_UNIQUE_PKTS
	output << stats.pktRecvUnique << ",";
#endif
	output << stats.pktRcvLoss << ',';
	output << stats.pktRcvDrop << ',';
	output << stats.pktRcvUndecrypt << ",";
	output << stats.pktRcvRetrans << ',';
	output << stats.pktRcvBelated << ',';

	output << stats.byteRecv << ',';
	output << stats.byteAvailRcvBuf << ',';
	output << stats.byteRcvLoss << ',';
	output << stats.byteRcvDrop << ',';
	output << stats.mbpsRecvRate << ',';
	output << stats.msRcvBuf << ',';
	output << stats.msRcvTsbPdDelay;

#if	HAS_PKT_REORDER_TOL
	output << "," << stats.pktReorderTolerance;
#endif

	output << endl;

	return output.str();

#undef HAS_PUT_TIME
#undef HAS_UNIQUE_PKTS
}

const nlohmann::json socket::srt::stats_to_json(int socketid, const SRT_TRACEBSTATS& stats)
{
	nlohmann::json root;

#define HAS_PKT_REORDER_TOL (SRT_VERSION_MAJOR >= 1) && (SRT_VERSION_MINOR >= 4) && (SRT_VERSION_PATCH > 0)
// pktSentUnique, pktRecvUnique were added in SRT v1.4.2
#define HAS_UNIQUE_PKTS (SRT_VERSION_MAJOR == 1) && ((SRT_VERSION_MINOR > 4) || ((SRT_VERSION_MINOR == 4) && (SRT_VERSION_PATCH >= 2)))

#ifdef HAS_PUT_TIME
  root["Timepoint"] = print_timestamp_now();
#endif

	root["Time"] = stats.msTimeStamp;
	root["SocketID"] = socketid;
	root["pktFlowWindow"] = stats.pktFlowWindow;
	root["pktCongestionWindow"] = stats.pktCongestionWindow;
	root["pktFlightSize"] = stats.pktFlightSize;
	root["msRTT"] = stats.msRTT;
	root["mbpsBandwidth"] = stats.mbpsBandwidth;
	root["mbpsMaxBW"] = stats.mbpsMaxBW;
	root["pktSent"] = stats.pktSent;
	root["pktSndLoss"] = stats.pktSndLoss;
	root["pktSndDrop"] = stats.pktSndDrop;
	root["pktRetrans"] = stats.pktRetrans;
	root["byteSent"] = stats.byteSent;
	root["byteAvailSndBuf"] = stats.byteAvailSndBuf;
	root["byteSndDrop"] = stats.byteSndDrop;
	root["mbpsSendRate"] = stats.mbpsSendRate;
	root["usPktSndPeriod"] = stats.usPktSndPeriod;
	root["msSndBuf"] = stats.msSndBuf;
	root["pktRecv"] = stats.pktRecv;
	root["pktRcvLoss"] = stats.pktRcvLoss;
	root["pktRcvDrop"] = stats.pktRcvDrop;
	root["pktRcvUndecrypt"] = stats.pktRcvUndecrypt;
	root["mbpsRecvRate"] = stats.mbpsRecvRate;
	root["msRcvBuf"] = stats.msRcvBuf;
	root["msRcvTsbPdDelay"] = stats.msRcvTsbPdDelay;

#if	HAS_PKT_REORDER_TOL
	root["pktReorderTolerance"] = stats.pktReorderTolerance;
#endif

#if	HAS_UNIQUE_PKTS
	root["pktSentUnique"] = stats.pktSentUnique;
	root["pktRecvUnique"] = stats.pktRecvUnique;
#endif

	return root;
}

const string socket::srt::get_statistics(string stats_format, bool print_header) const
{
	SRT_TRACEBSTATS stats;
	if (SRT_ERROR == srt_bstats(m_bind_socket, &stats, true))
		raise_exception("statistics");

	if(stats_format == "json")
	{
		if(print_header){
			// JSON format doesn't have header. Return empty string
			return "";
		}
		nlohmann::json root;
		root["ConnStats"] = stats_to_json(m_bind_socket, stats);
		root["LinksStats"] = nullptr; // No need for the array of links because only one link exists.
		return root.dump() + "\n";
	}
	else
	{
		if(stats_format != "csv")
		{
			spdlog::warn(LOG_SOCK_SRT "{} format is not supported. csv format will be used instead", stats_format);
		}
		return stats_to_csv(m_bind_socket, stats, print_header);
	}
}
