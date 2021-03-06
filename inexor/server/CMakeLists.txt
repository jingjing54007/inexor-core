prepend(SERVER_SOURCES_ENGINE ${SOURCE_DIR}/engine
    server.cpp command.cpp worldio.cpp)

prepend(SERVER_SOURCES_FPSGAME ${SOURCE_DIR}/fpsgame
    server.cpp entities.cpp)

set(SERVER_SOURCES
  ${SHARED_MODULE_SOURCES}
  ${SERVER_SOURCES_FPSGAME}
  ${SERVER_SOURCES_ENGINE}
  CACHE INTERNAL "")

# Set Binary name
set(SERVER_BINARY_NAME inexor-core-server CACHE INTERNAL "Server binary name.")

add_definitions(-DSERVER -DSTANDALONE)

add_app(${SERVER_BINARY_NAME} ${SERVER_SOURCES})

require_threads(${SERVER_BINARY_NAME})
require_zlib(${SERVER_BINARY_NAME})
require_network(${SERVER_BINARY_NAME} "SERVER STANDALONE SERVMODE")
require_util(${SERVER_BINARY_NAME})
require_crashreporter(${SERVER_BINARY_NAME})
require_filesystem(${SERVER_BINARY_NAME})
