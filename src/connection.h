#ifndef CONNECTION_H

#define CONNECTION_H

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/log/trivial.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/ssl.h>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = asio::ssl;
using tcp = boost::asio::ip::tcp;

static const std::string http_port = "80";
static const std::string https_port = "443";
static const unsigned http_version = 11;
static const char port_delimiter = ':';
static const std::chrono::seconds timeout {30};
static const char user_agent[] =
    "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:69.0) Gecko/20100101 Firefox/69.0";

class connection : public std::enable_shared_from_this<connection> {
	public:
		typedef http::response<http::vector_body<char>> http_response;
		typedef std::function<void(const std::string&)> on_error_callback;
		typedef std::function<void(const std::shared_ptr<connection>&, http_response *)>
		    on_receive_callback;

	private:
		beast::flat_buffer buffer;
		on_receive_callback on_receive_cb;
		http::request<http::empty_body> request;
		http_response response;
		tcp::resolver * const resolver = nullptr;
		size_t sequence_number = 0;
		bool connected = false;

		virtual void async_read()
		{
			auto& stream = get_tcp_stream();

			stream.expires_after(timeout);
			do_async_read(stream);
		}

		virtual void async_write()
		{
			auto& stream = get_tcp_stream();

			stream.expires_after(timeout);
			do_async_write(stream);
		}

		virtual const std::string& get_default_port() const noexcept
		{
			return http_port;
		}

		virtual beast::tcp_stream& get_tcp_stream() = 0;

		void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
		{
			if (ec) {
				BOOST_LOG_TRIVIAL(error) << "Failed to connect to: " << host
							 << " Error code: " << ec.what();
				on_error(host);
			}
			else
				post_connect();
		}

		void on_read(beast::error_code ec, size_t)
		{
			if (ec)
				on_error(host);
			else
				on_receive_cb(shared_from_this(), &response);
		}

		void on_resolve(beast::error_code ec, tcp::resolver::results_type results)
		{
			if (ec) {
				BOOST_LOG_TRIVIAL(error) << "Failed to resolve: " << host
							 << " Error code: " << ec.what();
				on_error(host);
			}
			else {
				BOOST_LOG_TRIVIAL(trace) << "Establishing connection "
							 << sequence_number << " to: " << host;

				if (pre_connect()) {
					auto& stream = get_tcp_stream();

					stream.expires_after(timeout);
					stream.async_connect(
					    results,
					    beast::bind_front_handler(&connection::on_connect,
								      shared_from_this()));
				}
				else {
					BOOST_LOG_TRIVIAL(error)
					    << "Failed to connect to: " << host;
					on_error(host);
				}
			}
		}

		void on_write(beast::error_code ec, size_t)
		{
			if (ec)
				on_error(host);
			else {
				response = http::response<http::vector_body<char>> {};
				async_read();
			}
		}

		virtual bool pre_connect()
		{
			return true;
		}

	protected:
		std::string host;
		on_error_callback on_error;
		std::string_view::size_type port_pos = 0;

		connection(size_t sequence_number,
			   const std::string_view& h,
			   tcp::resolver *resolver) :
		    resolver(resolver),
		    sequence_number(sequence_number), host(h)
		{
			request.version(http_version);
			request.set(http::field::user_agent, user_agent);

			auto pos = host.find(port_delimiter);

			if (pos == std::string::npos) {
				const auto d = port_delimiter;

				pos = host.size() + 1;
				host.append(&d, 1);
				host.append(get_default_port());
			}

			port_pos = pos;
			request.set(http::field::host, host);
		}

		connection(const connection&) = default;
		connection(connection&&) = default;
		virtual ~connection() = default;

		template<typename stream> void do_async_read(stream& s)
		{
			http::async_read(
			    s,
			    buffer,
			    response,
			    beast::bind_front_handler(&connection::on_read, shared_from_this()));
		}

		template<typename stream> void do_async_write(stream& s)
		{
			http::async_write(
			    s,
			    request,
			    beast::bind_front_handler(&connection::on_write, shared_from_this()));
		}

		virtual void post_connect()
		{
			connected = true;
			async_write();
		}

	public:
		void get(const std::string_view& resource,
			 on_receive_callback&& on_receive_fn,
			 on_error_callback&& on_error_cb)
		{
			request.method(http::verb::get);
			request.target(resource);
			on_error = std::move(on_error_cb);
			on_receive_cb = std::move(on_receive_fn);

			if (connected)
				async_write();
			else {
				const std::string_view h {host};

				resolver->async_resolve(
				    h.substr(0, port_pos),
				    h.substr(port_pos + 1),
				    beast::bind_front_handler(&connection::on_resolve,
							      shared_from_this()));
			}
		}

		const std::string& get_host() const noexcept
		{
			return host;
		}
};

class http_connection : public virtual connection {
		beast::tcp_stream stream;

		beast::tcp_stream& get_tcp_stream() override
		{
			return stream;
		}

	public:
		http_connection(size_t sequence_number,
				const std::string_view& h,
				asio::io_context *io,
				tcp::resolver *resolver) :
		    connection(sequence_number, h, resolver),
		    stream(*io)
		{
		}
};

class https_connection : public virtual connection {
		beast::ssl_stream<beast::tcp_stream> stream;

		void async_read() override
		{
			get_tcp_stream().expires_after(timeout);
			do_async_read(stream);
		}

		void async_write() override
		{
			get_tcp_stream().expires_after(timeout);
			do_async_write(stream);
		}

		const std::string& get_default_port() const noexcept override
		{
			return https_port;
		}

		beast::tcp_stream& get_tcp_stream() override
		{
			return beast::get_lowest_layer(stream);
		}

		void on_handshake(beast::error_code ec)
		{
			if (ec) {
				BOOST_LOG_TRIVIAL(error) << "Failed TLS handshake with: " << host
							 << " Error code: " << ec.what();
				on_error(host);
			}
			else
				connection::post_connect();
		}

		void post_connect() override
		{
			auto c = std::dynamic_pointer_cast<https_connection>(shared_from_this());

			get_tcp_stream().expires_after(timeout);
			stream.async_handshake(
			    asio::ssl::stream_base::client,
			    beast::bind_front_handler(&https_connection::on_handshake, c));
		}

		bool pre_connect() override
		{
			const std::string h {host.substr(0, host.find(port_delimiter))};

			return !!SSL_set_tlsext_host_name(stream.native_handle(), h.c_str());
		}

	public:
		https_connection(size_t sequence_number,
				 const std::string_view& h,
				 asio::io_context *io,
				 tcp::resolver *resolver,
				 ssl::context *tls_context) :
		    connection(sequence_number, h, resolver),
		    stream(*io, *tls_context)
		{
		}
};

#endif // CONNECTION_H
