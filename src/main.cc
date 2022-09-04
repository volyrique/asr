#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>
#include <cstdlib>

#include "connection_pool.h"
#include "playlist.h"

int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;

	if (argc < 2) {
		BOOST_LOG_TRIVIAL(info) << "Usage: " << *argv << " <playlist URL>";
		return EXIT_SUCCESS;
	}

	boost::asio::io_context io;
	connection_pool pool {&io};
	playlist playlist {&io, &pool};

	if (playlist.record(argv[1])) {
		io.run();
		ret = EXIT_SUCCESS;
	}

	return ret;
}
