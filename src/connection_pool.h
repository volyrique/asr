#ifndef CONNECTION_POOL_H

#define CONNECTION_POOL_H

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/log/trivial.hpp>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace asio = boost::asio;
namespace http = boost::beast::http;
namespace ssl = asio::ssl;

#define HTTP_PROTOCOL "http"
#define HTTPS_PROTOCOL "https"
#define PROTOCOL_END "://"

static const char resource_delimiter = '/';

typedef http::response<http::vector_body<char>> http_response;

class connection;

class connection_pool {
		typedef std::tuple<bool,
				   std::string,
				   std::string,
				   std::function<void(http_response *)>,
				   std::function<void(void)>>
		    request;

		std::unordered_map<std::string, std::list<std::shared_ptr<connection>>> connections;
		std::unordered_map<std::string, size_t> num_connections;
		std::unordered_map<std::string, std::list<request>> requests;
		asio::ip::tcp::resolver resolver;
		ssl::context tls_context;
		asio::io_context * const io = nullptr;
		size_t sequence_number = 0;

	public:
		typedef std::function<void(void)> on_error_callback;
		typedef std::function<void(http_response *)> on_read_callback;

		connection_pool(asio::io_context *io_ctx) :
		    resolver(*io_ctx), tls_context(ssl::context::tlsv12_client), io(io_ctx)
		{
			boost::system::error_code ec;

			tls_context.set_default_verify_paths(ec);
			tls_context.set_verify_mode(ssl::verify_peer);

			if (ec)
				BOOST_LOG_TRIVIAL(error)
				    << "Unable to set the default paths for TLS verification.";
		}

		void get(bool is_https,
			 const std::string_view& host,
			 const std::string_view& resource,
			 const on_read_callback& on_read,
			 const on_error_callback& on_error,
			 size_t retry_number = 0);
		bool get(const std::string_view& url,
			 const on_read_callback& on_read,
			 const on_error_callback& on_error,
			 size_t retry_number = 0);

		static bool parse_url(const std::string_view& url,
				      bool *is_https,
				      std::string_view *host,
				      std::string_view *resource);
};

#endif // CONNECTION_POOL_H
