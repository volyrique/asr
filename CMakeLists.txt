cmake_minimum_required(VERSION 3.18.0)
project(asr)
find_path(BOOST_INCLUDE boost/beast.hpp REQUIRED)
find_path(OPENSSL_INCLUDE openssl/ssl.h REQUIRED)
include_directories(src ${BOOST_INCLUDE} ${OPENSSL_INCLUDE})
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(BOOST_BEAST_USE_STD_STRING_VIEW)

if(${UNIX})

set(COMMON_OPTIONS -flto -pthread)
add_compile_options(-pedantic -Wall -Wextra ${COMMON_OPTIONS})
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_FORTIFY_SOURCE=2")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Ofast")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -Ofast")
find_library(BOOST_LOG_LIB libboost_log.a)

if(NOT BOOST_LOG_LIB)

add_compile_definitions(BOOST_LOG_DYN_LINK)

endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")

add_compile_definitions(BOOST_ASIO_DISABLE_EPOLL BOOST_ASIO_HAS_IO_URING)

endif()

elseif(${MSVC})

add_compile_definitions(_WIN32_WINNT=0x0A00)
add_compile_options(/W4)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /bigobj")

endif()

file(GLOB_RECURSE SOURCES "src/*.cc")
add_executable(${PROJECT_NAME} ${SOURCES})

if(${UNIX})

target_link_libraries(${PROJECT_NAME} ${COMMON_OPTIONS})
find_library(CRYPTO_LIB crypto REQUIRED)
find_library(SSL_LIB ssl REQUIRED)

if(BOOST_LOG_LIB)

# If Boost.Log is a static library, prefer static libraries.
find_library(BOOST_THREAD_LIB libboost_thread.a)
find_library(BOOST_THREAD_LIB boost_thread REQUIRED)
target_link_libraries(${PROJECT_NAME} ${BOOST_LOG_LIB} ${BOOST_THREAD_LIB})

else()

find_library(BOOST_LOG_LIB boost_log REQUIRED)
target_link_libraries(${PROJECT_NAME} ${BOOST_LOG_LIB})

endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")

find_library(URING_LIB uring REQUIRED)
target_link_libraries(${PROJECT_NAME} ${URING_LIB})

endif()

elseif(${MSVC})

find_library(CRYPTO_LIB libcrypto REQUIRED)
find_library(SSL_LIB libssl REQUIRED)

endif()

target_link_libraries(${PROJECT_NAME} ${SSL_LIB} ${CRYPTO_LIB})
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
