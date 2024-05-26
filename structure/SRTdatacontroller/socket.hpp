#pragma once
#include <map>

#if !defined(_WIN32)
#include <sys/ioctl.h>
typedef int SOCKET;
#define INVALID_SOCKET ((SOCKET)-1)
#define closesocket close
#endif

// srtdatacontroller
#include "buffer.hpp"

// OpenSRT
#include "uriparser.hpp"

namespace srtdatacontroller
{
namespace socket
{

class exception : public std::exception
{
public:
	explicit exception(const std::string &&err)
		: m_error_msg(std::move(err)) // note "short string optimization" (SSO)
	{
	}

public:
	virtual const char *what() const throw() { return m_error_msg.c_str(); }

private:
	const std::string m_error_msg;
};

class isocket
{

public:
	virtual bool is_caller() const = 0;

public:
	/** Read data from socket.
	 *
	 * @returns The number of bytes read.
	 *
	 * @throws socket::exception Thrown on failure.
	 */
	virtual size_t read(const mutable_buffer &buffer, int timeout_ms = -1) = 0;

	/** Write data to socket.
	 *
	 * @returns The number of bytes written.
	 *
	 * @throws socket::exception Thrown on failure.
	 */
	virtual int write(const const_buffer &buffer, int timeout_ms = -1) = 0;

public:
	/** Check if statistics is supported by a socket implementation.
	 *
	 * @returns true if statistics is supported, false otherwise.
	 *
	 */
	virtual bool supports_statistics() const { return false; }

	/** Retrieve statistics on a socket.
	 *
	 * @returns The number of bytes received.
	 *
	 * @throws socket::exception Thrown on failure.
	 */
	virtual const std::string get_statistics(std::string statistic_format, bool print_header) const { return std::string(); }


	virtual SOCKET id() const = 0;
};

} // namespace socket
} // namespace srtdatacontroller
