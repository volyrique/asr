#include <boost/beast.hpp>
#include <boost/log/trivial.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "connection.h"
#include "connection_pool.h"

static const size_t max_connections = 4;

void connection_pool::get(bool is_https,
			  const std::string_view& host,
			  const std::string_view& resource,
			  const on_receive_callback& on_receive,
			  const on_error_callback& on_error,
			  size_t retry_number)
{
	std::shared_ptr<connection> c;
	std::string h {host};

	if (host.find(port_delimiter) == std::string_view::npos) {
		const auto d = port_delimiter;

		h.append(&d, 1);
		h.append(is_https ? https_port : http_port);
	}

	if (connections[h].empty()) {
		if (num_connections[h] >= max_connections) {
			requests[h].emplace_back(
			    std::make_tuple(is_https, h, resource, on_receive, on_error));
			return;
		}

		c = is_https
			? std::static_pointer_cast<connection>(std::make_shared<https_connection>(
			      sequence_number, h, io, &resolver, &tls_context))
			: std::static_pointer_cast<connection>(
			      std::make_shared<http_connection>(sequence_number, h, io, &resolver));
		num_connections[h]++;
		sequence_number++;
	}
	else {
		c = connections[h].back();
		connections[h].pop_back();
		retry_number++;
	}

	auto on_error_wrapper =
	    [is_https, on_receive, on_error, resource = std::string {resource}, retry_number, this](
		const std::string& host) {
		    num_connections[host]--;

		    if (retry_number)
			    get(is_https, host, resource, on_receive, on_error, retry_number - 1);
		    else {
			    BOOST_LOG_TRIVIAL(error)
				<< "Failed to get: " << (is_https ? HTTPS_PREFIX : HTTP_PREFIX)
				<< host << resource;
			    on_error();
		    }

		    if (!requests[host].empty()) {
			    const auto r = requests[host].front();

			    requests[host].pop_front();
			    get(std::get<0>(r),
				std::get<1>(r),
				std::get<2>(r),
				std::get<3>(r),
				std::get<4>(r));
		    }
	    };
	auto on_receive_wrapper = [on_receive, this](const std::shared_ptr<connection>& connection,
						     http_response *response) {
		const auto& host = connection->get_host();

		on_receive(response);
		connections[host].push_back(connection);

		if (!requests[host].empty()) {
			const auto r = requests[host].front();

			requests[host].pop_front();
			get(std::get<0>(r),
			    std::get<1>(r),
			    std::get<2>(r),
			    std::get<3>(r),
			    std::get<4>(r));
		}
	};

	c->get(resource, on_receive_wrapper, on_error_wrapper);
}

bool connection_pool::get(const std::string_view& url,
			  const on_receive_callback& on_receive,
			  const on_error_callback& on_error,
			  size_t retry_number)
{
	std::string_view host;
	std::string_view resource;
	bool is_https;
	bool ret = false;

	if (parse_url(url, &is_https, &host, &resource)) {
		get(is_https, host, resource, on_receive, on_error, retry_number);
		ret = true;
	}
	else
		BOOST_LOG_TRIVIAL(error) << "Invalid URL: " << url;

	return ret;
}

bool connection_pool::parse_url(const std::string_view& url,
				bool *is_https,
				std::string_view *host,
				std::string_view *resource)
{
	const auto pos = url.find(PROTOCOL_END);

	const auto& protocol = url.substr(0, pos);

	*is_https = protocol == HTTPS_PROTOCOL;

	if (!*is_https && protocol != HTTP_PROTOCOL)
		return false;

	const auto host_pos = pos + sizeof(PROTOCOL_END) - 1;

	if (url.size() <= host_pos)
		return false;

	const auto res_pos = url.find(resource_delimiter, host_pos);

	if (res_pos == std::string_view::npos)
		return false;

	*host = url.substr(host_pos, res_pos - host_pos);
	*resource = url.substr(res_pos);
	return true;
}
