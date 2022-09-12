#include <algorithm>
#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>
#include <functional>
#include <string>
#include <utility>

#ifdef __APPLE__

#include <fcntl.h>

#endif // __APPLE__

#include "stream_writer.h"

void stream_writer::add_media_initialization_section(bool is_https,
						     const std::string_view& host,
						     const std::string_view& resource)
{
	if (first_segment) {
		// Insert a placeholder element.
		media_initialization_section.push_back(0);
		pool->get(is_https,
			  host,
			  resource,
			  std::bind(&stream_writer::on_media_initialization_section_receive,
				    this,
				    std::placeholders::_1),
			  std::bind(&stream_writer::on_media_initialization_section_error, this));
	}
}

void stream_writer::add_media_initialization_section(const std::string_view& url)
{
	if (first_segment) {
		// Insert a placeholder element.
		media_initialization_section.push_back(0);

		if (!pool->get(
			url,
			std::bind(&stream_writer::on_media_initialization_section_receive,
				  this,
				  std::placeholders::_1),
			std::bind(&stream_writer::on_media_initialization_section_error, this)))
			on_media_initialization_section_error();
	}
}

void stream_writer::add_segment(size_t sequence_number,
				bool is_https,
				const std::string_view& host,
				const std::string_view& resource)
{
	if (sequence_number > last_downloaded_sequence_number || first_segment) {
		first_segment = false;
		last_downloaded_sequence_number = sequence_number;
		segments_in_progress.insert(sequence_number);
		pool->get(is_https,
			  host,
			  resource,
			  std::bind(&stream_writer::on_segment_receive,
				    this,
				    sequence_number,
				    std::placeholders::_1),
			  std::bind(&stream_writer::on_segment_error, this, sequence_number));
	}
}

void stream_writer::add_segment(size_t sequence_number, const std::string_view& url)
{
	if (sequence_number > last_downloaded_sequence_number || first_segment) {
		first_segment = false;
		last_downloaded_sequence_number = sequence_number;
		segments_in_progress.insert(sequence_number);

		if (!pool->get(url,
			       std::bind(&stream_writer::on_segment_receive,
					 this,
					 sequence_number,
					 std::placeholders::_1),
			       std::bind(&stream_writer::on_segment_error, this, sequence_number)))
			on_segment_error(sequence_number);
	}
}

void stream_writer::media_initialization_section_write_handler(const boost::system::error_code& ec,
							       size_t size)
{
	if (ec || size != media_initialization_section.size())
		BOOST_LOG_TRIVIAL(error) << "Failed to write media initialization section: " << size
					 << " Error code: " << ec.what();
	else
		BOOST_LOG_TRIVIAL(trace) << "Wrote media initialization section.";

	write_in_progress = false;
	media_initialization_section = std::vector<char> {0};
	write_segment();
}

void stream_writer::on_media_initialization_section_error()
{
	BOOST_LOG_TRIVIAL(error) << "Failed to get the media initialization section.";
	media_initialization_section.clear();
	write_segment();
}

void stream_writer::on_media_initialization_section_receive(http_response *response)
{
	if (response->result() == http::status::ok) {
		BOOST_LOG_TRIVIAL(trace)
		    << "Received media initialization section: size = " << response->body().size();
		media_initialization_section = std::move(response->body());
		write_in_progress = true;
		asio::async_write(
		    output,
		    asio::buffer(media_initialization_section),
		    std::bind(&stream_writer::media_initialization_section_write_handler,
			      this,
			      std::placeholders::_1,
			      std::placeholders::_2));
	}
	else {
		BOOST_LOG_TRIVIAL(error) << "Invalid " << response->result_int()
					 << " media initialization section response.";
		on_media_initialization_section_error();
	}
}

void stream_writer::on_segment_error(size_t sequence_number)
{
	segments_in_progress.erase(sequence_number);
	write_segment();
}

void stream_writer::on_segment_receive(size_t sequence_number, http_response *response)
{
	if (response->result() == http::status::ok) {
		BOOST_LOG_TRIVIAL(trace) << "Received media segment " << sequence_number
					 << ": size = " << response->body().size();
		segments_in_progress.erase(sequence_number);
		segments.push(std::make_pair(sequence_number, std::move(response->body())));
		write_segment();
	}
	else {
		BOOST_LOG_TRIVIAL(error)
		    << "Invalid " << response->result_int()
		    << " media segment response: sequence_number = " << sequence_number;
		on_segment_error(sequence_number);
	}
}

bool stream_writer::open(const std::string& name)
{
	boost::system::error_code ec;
	bool ret = false;

#ifdef __APPLE__
	const int fd = ::open(name.c_str(), O_APPEND | O_CLOEXEC | O_CREAT | O_WRONLY);

	if (fd >= 0) {
		output.assign(fd, ec);
		ret = !ec;
	}
#else
	output.open(name,
		    asio::stream_file::append | asio::stream_file::create |
			asio::stream_file::write_only,
		    ec);
	ret = !ec;
#endif

	if (!ret)
		BOOST_LOG_TRIVIAL(fatal) << "Failed to open output file: " << name;

	return ret;
}

void stream_writer::write_handler(const boost::system::error_code& ec, size_t size)
{
	if (ec || size != segments.top().second.size())
		BOOST_LOG_TRIVIAL(error) << "Failed to write media segment " << segments.top().first
					 << ": " << size << " Error code: " << ec.what();
	else
		BOOST_LOG_TRIVIAL(trace) << "Wrote media segment " << segments.top().first << ".";

	last_written_sequence_number = segments.top().first;
	write_in_progress = false;
	segments.pop();
	write_segment();
}

void stream_writer::write_segment()
{
	if (write_in_progress || segments.empty() || !media_initialization_section.empty())
		return;

	const auto& minimum =
	    std::min_element(segments_in_progress.cbegin(), segments_in_progress.cend());
	auto& segment = segments.top();

	if (minimum != segments_in_progress.cend() && segment.first > *minimum)
		return;

	const size_t seq_number_diff = segment.first - last_written_sequence_number;

	if (seq_number_diff > 1 && last_written_sequence_number) {
		if (seq_number_diff == 2)
			BOOST_LOG_TRIVIAL(error) << "Dropped media segment: " << segment.first - 1;
		else
			BOOST_LOG_TRIVIAL(error)
			    << "Dropped media segments: " << last_written_sequence_number + 1
			    << " - " << segment.first - 1;
	}

	write_in_progress = true;
	asio::async_write(
	    output,
	    asio::buffer(segment.second),
	    std::bind(
		&stream_writer::write_handler, this, std::placeholders::_1, std::placeholders::_2));
}
