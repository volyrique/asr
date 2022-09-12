#include <algorithm>
#include <boost/beast.hpp>
#include <boost/log/trivial.hpp>
#include <cctype>
#include <charconv>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "connection_pool.h"
#include "playlist.h"

#define BANDWIDTH_ATTRIBUTE "BANDWIDTH="
#define DISCONTINUITY_TAG "#EXT-X-DISCONTINUITY"
#define END_LIST_TAG "#EXT-X-ENDLIST"
#define MAP_TAG "#EXT-X-MAP:"
#define MEDIA_SEQUENCE_TAG "#EXT-X-MEDIA-SEQUENCE:"
#define PLAYLIST_TYPE_VOD_TAG "#EXT-X-PLAYLIST-TYPE:VOD"
#define STREAM_INF_TAG "#EXT-X-STREAM-INF:"
#define TARGET_DURATION_TAG "#EXT-X-TARGETDURATION:"
#define URI_ATTRIBUTE "URI=\""

static const char carriage_return = '\r';
static const char extension_delimiter = '.';
static const std::string hls_content_type = "application/vnd.apple.mpegurl";
static const char line_feed = '\n';
static const size_t max_file_name_length = 32;
static const char query_delimiter = '?';
static const char tag_begin = '#';
static const std::string transport_stream_extension = ".ts";
static const char uri_delimiter = '"';

static bool is_url(const char *p, size_t len)
{
	return (len >= sizeof(HTTPS_PREFIX) - 1 &&
		std::equal(p, p + sizeof(HTTPS_PREFIX) - 1, HTTPS_PREFIX)) ||
	       (len >= sizeof(HTTP_PREFIX) - 1 &&
		std::equal(p, p + sizeof(HTTP_PREFIX) - 1, HTTP_PREFIX));
}

void playlist::on_error() noexcept
{
	period = 0;
}

void playlist::on_initial_playlist_receive(http_response *response)
{
	parse_playlist(response);

	if (period) {
		timer.expires_after(std::chrono::seconds(period));
		timer.async_wait(std::bind(&playlist::timer_handler, this, std::placeholders::_1));
	}
}

void playlist::parse_hls_playlist(const std::vector<char>& response_body)
{
	std::string_view final_stream_information;
	std::string_view stream_information;
	std::string_view u;
	const char *line_end;
	const char *iter = response_body.data();
	const char * const playlist_end = iter + response_body.size();
	size_t bandwidth = 0;
	size_t max_bandwidth = 0;
	size_t segment_number = 0;
	size_t sequence_number = 0;
	size_t target_duration = 0;
	bool end_list = false;
	bool is_line_url = false;
	bool master_playlist = true;

	// In case we have to deal with an empty line later, make sure that we can look at least one
	// character back.
	if (iter != playlist_end && *iter == line_feed)
		iter++;

	for (; iter < playlist_end; iter = line_end + 1) {
		line_end = std::find(iter, playlist_end, line_feed);

		const char * const e = line_end[-1] == carriage_return ? line_end - 1 : line_end;
		const size_t line_len = e - iter;

		if (!line_len) {
			if (line_end == playlist_end)
				break;

			continue;
		}

		if (line_len > sizeof(TARGET_DURATION_TAG) - 1 &&
		    std::equal(iter, iter + sizeof(TARGET_DURATION_TAG) - 1, TARGET_DURATION_TAG)) {
			std::from_chars(iter + sizeof(TARGET_DURATION_TAG) - 1, e, target_duration);
			master_playlist = false;
		}
		else if (line_len > sizeof(MEDIA_SEQUENCE_TAG) - 1 &&
			 std::equal(
			     iter, iter + sizeof(MEDIA_SEQUENCE_TAG) - 1, MEDIA_SEQUENCE_TAG))
			std::from_chars(iter + sizeof(MEDIA_SEQUENCE_TAG) - 1, e, sequence_number);
		else if (line_len >= sizeof(DISCONTINUITY_TAG) - 1 &&
			 std::equal(iter, iter + sizeof(DISCONTINUITY_TAG) - 1, DISCONTINUITY_TAG))
			BOOST_LOG_TRIVIAL(warning) << "Playlist discontinuity.";
		else if (line_len >= sizeof(END_LIST_TAG) - 1 &&
			 std::equal(iter, iter + sizeof(END_LIST_TAG) - 1, END_LIST_TAG))
			end_list = true;
		else if (line_len >= sizeof(PLAYLIST_TYPE_VOD_TAG) - 1 &&
			 std::equal(
			     iter, iter + sizeof(PLAYLIST_TYPE_VOD_TAG) - 1, PLAYLIST_TYPE_VOD_TAG))
			end_list = true;
		else if (line_len > sizeof(MAP_TAG) - 1 &&
			 std::equal(iter, iter + sizeof(MAP_TAG) - 1, MAP_TAG)) {
			auto uri_pos = std::search(iter + sizeof(MAP_TAG) - 1,
						   e,
						   URI_ATTRIBUTE,
						   &URI_ATTRIBUTE[sizeof(URI_ATTRIBUTE) - 1]);

			if (uri_pos != e) {
				uri_pos += sizeof(URI_ATTRIBUTE) - 1;

				const auto uri_end = std::find(uri_pos, e, uri_delimiter);

				if (uri_end != e) {
					const size_t uri_len = uri_end - uri_pos;

					if (is_url(uri_pos, uri_len))
						writer.add_media_initialization_section(
						    std::string_view {uri_pos, uri_len});
					else if (*uri_pos == resource_delimiter)
						writer.add_media_initialization_section(
						    is_https,
						    host,
						    std::string_view {uri_pos, uri_len});
					else {
						std::string r {
						    resource.substr(0, resource_prefix_len)};

						r.append(uri_pos, uri_len);
						writer.add_media_initialization_section(
						    is_https, host, r);
					}
				}
			}
		}
		else if (line_len > sizeof(STREAM_INF_TAG) - 1 &&
			 std::equal(iter, iter + sizeof(STREAM_INF_TAG) - 1, STREAM_INF_TAG)) {
			bandwidth = 0;
			stream_information =
			    std::string_view {iter + sizeof(STREAM_INF_TAG) - 1,
					      line_len + 1 - sizeof(STREAM_INF_TAG)};

			auto bandwidth_pos =
			    std::search(iter,
					e,
					BANDWIDTH_ATTRIBUTE,
					&BANDWIDTH_ATTRIBUTE[sizeof(BANDWIDTH_ATTRIBUTE) - 1]);

			if (bandwidth_pos != e) {
				bandwidth_pos += sizeof(BANDWIDTH_ATTRIBUTE) - 1;

				if (bandwidth_pos != e)
					std::from_chars(bandwidth_pos, e, bandwidth);
			}
		}
		else if (*iter != tag_begin) {
			const bool is_current_line_url = is_url(iter, line_len);

			if (master_playlist) {
				if (bandwidth > max_bandwidth) {
					max_bandwidth = bandwidth;
					is_line_url = is_current_line_url;
					final_stream_information = std::move(stream_information);
					u = std::string_view {iter, line_len};
				}
			}
			else if (is_current_line_url)
				writer.add_segment(sequence_number,
						   std::string_view {iter, line_len});
			else if (*iter == resource_delimiter)
				writer.add_segment(sequence_number,
						   is_https,
						   host,
						   std::string_view {iter, line_len});
			else {
				std::string r {resource.substr(0, resource_prefix_len)};

				r.append(iter, line_len);
				writer.add_segment(sequence_number, is_https, host, r);
			}

			segment_number++;
			sequence_number++;
		}

		if (line_end == playlist_end)
			break;
	}

	sequence_number = sequence_number - segment_number;

	if (master_playlist) {
		BOOST_LOG_TRIVIAL(trace) << "Received master playlist with stream information: "
					 << final_stream_information;
		target_duration = 0;

		if (is_line_url)
			url = u;
		else if (u[0] == resource_delimiter) {
			const std::string::size_type offset =
			    (is_https ? sizeof(HTTPS_PREFIX) : sizeof(HTTP_PREFIX)) - 1;

			url.resize(url.find(resource_delimiter, offset));
			url.append(u);
		}
		else {
			url.resize(url.rfind(resource_delimiter, url.find(query_delimiter)) + 1);
			url.append(u);
		}

		if (connection_pool::parse_url(url, &is_https, &host, &resource)) {
			BOOST_LOG_TRIVIAL(trace) << "Media playlist URL: " << url;
			resource_prefix_len =
			    resource.rfind(resource_delimiter, resource.find(query_delimiter)) + 1;
			pool->get(is_https,
				  host,
				  resource,
				  std::bind(&playlist::on_initial_playlist_receive,
					    this,
					    std::placeholders::_1),
				  std::bind(&playlist::on_error, this));
		}
		else
			BOOST_LOG_TRIVIAL(error) << "Invalid playlist URL: " << url;
	}
	else if (end_list) {
		BOOST_LOG_TRIVIAL(trace)
		    << "Received final playlist: sequence number = " << sequence_number
		    << " segments = " << segment_number;
		target_duration = 0;
	}
	else {
		BOOST_LOG_TRIVIAL(trace)
		    << "Received playlist: target duration = " << target_duration
		    << " sequence number = " << sequence_number << " segments = " << segment_number;

		if (target_duration > 1)
			target_duration = target_duration / 2;
		else
			target_duration = 1;
	}

	period = target_duration;
}

void playlist::parse_playlist(http_response *response)
{
	if (response->result() == http::status::ok) {
		const auto& content_type = response->base()[http::field::content_type];

		if (content_type.size() == hls_content_type.size() &&
		    std::equal(content_type.begin(),
			       content_type.end(),
			       hls_content_type.begin(),
			       [](const auto& x, const auto& y) { return std::tolower(x) == y; }))
			parse_hls_playlist(response->body());
		else {
			BOOST_LOG_TRIVIAL(error)
			    << "Invalid content type: " << content_type << " URL: " << url;
			on_error();
		}
	}
	else {
		BOOST_LOG_TRIVIAL(error)
		    << "Invalid " << response->result_int() << " response: " << url;
		on_error();
	}
}

bool playlist::record(const std::string_view& u)
{
	bool ret = false;

	url = u;

	if (connection_pool::parse_url(url, &is_https, &host, &resource)) {
		const auto query_pos = resource.find(query_delimiter);

		resource_prefix_len = resource.rfind(resource_delimiter, query_pos);

		if (resource_prefix_len++ == std::string_view::npos)
			BOOST_LOG_TRIVIAL(error) << "Invalid playlist URL: " << u;
		else {
			const auto& name =
			    resource.substr(resource_prefix_len, query_pos - resource_prefix_len);
			const auto extension_pos = name.find_last_of(extension_delimiter);
			std::string file_name {
			    name.substr(0, std::min(extension_pos, max_file_name_length))};

			file_name.append(transport_stream_extension);

			if (writer.open(file_name)) {
				pool->get(is_https,
					  host,
					  resource,
					  std::bind(&playlist::on_initial_playlist_receive,
						    this,
						    std::placeholders::_1),
					  std::bind(&playlist::on_error, this));
				ret = true;
			}
		}
	}
	else
		BOOST_LOG_TRIVIAL(error) << "Invalid playlist URL: " << u;

	return ret;
}

void playlist::timer_handler(const boost::system::error_code& ec)
{
	if (!ec) {
		if (period) {
			timer.expires_after(std::chrono::seconds(period));
			timer.async_wait(
			    std::bind(&playlist::timer_handler, this, std::placeholders::_1));
		}

		pool->get(is_https,
			  host,
			  resource,
			  std::bind(&playlist::parse_playlist, this, std::placeholders::_1),
			  std::bind(&playlist::on_error, this));
	}
}
