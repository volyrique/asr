#ifndef STREAM_WRITER_H

#define STREAM_WRITER_H

#include <boost/asio.hpp>
#include <boost/asio/stream_file.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "connection_pool.h"

namespace asio = boost::asio;

class stream_writer {
		typedef std::pair<size_t, std::vector<char>> media_segment;
#ifdef __APPLE__
		typedef asio::posix::stream_descriptor output_file;
#else
		typedef asio::stream_file output_file;
#endif // __APPLE__

		std::vector<char> media_initialization_section;
		output_file output;
		std::priority_queue<media_segment,
				    std::deque<media_segment>,
				    std::greater<media_segment>>
		    segments;
		std::set<size_t> segments_in_progress;
		size_t last_downloaded_sequence_number = 0;
		size_t last_written_sequence_number = 0;
		connection_pool * const pool = nullptr;
		bool first_segment = true;
		bool write_in_progress = false;

		void media_initialization_section_write_handler(const boost::system::error_code& ec,
								size_t size);
		void on_media_initialization_section_error();
		void on_media_initialization_section_receive(http_response *response);
		void on_segment_error(size_t sequence_number);
		void on_segment_receive(size_t sequence_number, http_response *response);
		void write_handler(const boost::system::error_code& ec, size_t size);
		void write_segment();

	public:
		stream_writer(asio::io_context *io_ctx, connection_pool *pool) :
		    media_initialization_section(0), output(*io_ctx), pool(pool)
		{
		}

		void add_media_initialization_section(bool is_https,
						      const std::string_view& host,
						      const std::string_view& resource);
		void add_media_initialization_section(const std::string_view& url);
		void add_segment(size_t sequence_number,
				 bool is_https,
				 const std::string_view& host,
				 const std::string_view& resource);
		void add_segment(size_t sequence_number, const std::string_view& url);
		bool open(const std::string& name);
};

#endif // STREAM_WRITER_H
