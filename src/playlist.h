#ifndef PLAYLIST_H

#define PLAYLIST_H

#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <string_view>

#include "connection_pool.h"
#include "stream_writer.h"

namespace asio = boost::asio;

class playlist {
		std::string_view host;
		std::string_view resource;
		asio::steady_timer timer;
		std::string url;
		stream_writer writer;
		size_t period = 0;
		connection_pool * const pool = nullptr;
		std::string_view::size_type resource_prefix_len = 0;
		bool is_https = false;

		void on_error() noexcept;
		void on_initial_playlist_read(http_response *response);
		void parse_playlist(http_response *response);
		void timer_handler(const boost::system::error_code& ec);

	public:
		playlist(asio::io_context *io, connection_pool *p) :
		    timer(*io), writer(io, p), pool(p)
		{
		}

		bool record(const std::string_view& u);
};

#endif // PLAYLIST_H
