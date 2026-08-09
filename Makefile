# ChatDB v2.1 - Makefile (no CMake required)
# Usage: make -j$(nproc)

CXX := g++
CC := gcc

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic     -DIXWEBSOCKET_USE_TLS     -DSPDLOG_COMPILED_LIB     -Iinclude     -Ithird_party/fmt-src/include     -Ithird_party/spdlog/include     -Ithird_party/nlohmann     -Ithird_party/httplib     -Ithird_party/ixwebsocket     -Ithird_party/hiredis     $(shell pkg-config --cflags sqlite3 openssl 2>/dev/null)

LDFLAGS := -pthread -ldl $(shell pkg-config --libs sqlite3 openssl 2>/dev/null) -lssl -lcrypto

# ixwebsocket
IXWS_SRC := $(wildcard third_party/ixwebsocket/ixwebsocket/*.cpp)
IXWS_OBJ := $(IXWS_SRC:.cpp=.o)

# hiredis
HIREDIS_SRC := third_party/hiredis/hiredis.c     third_party/hiredis/net.c     third_party/hiredis/sds.c     third_party/hiredis/async.c     third_party/hiredis/read.c     third_party/hiredis/alloc.c     third_party/hiredis/sockcompat.c
HIREDIS_OBJ := $(HIREDIS_SRC:.c=.o)

# ChatDB
CHATDB_SRC := src/config.cpp     src/sqlite_storage.cpp     src/sqlite_storage_backup.cpp     src/redis_client.cpp     src/ollama_client.cpp     src/message_processor.cpp     src/query_engine.cpp     src/chat_database.cpp     src/port_detector.cpp     src/http_server.cpp     src/onebot_v11_client.cpp     src/embedding_provider.cpp     src/memory_summarizer.cpp     src/process_guard.cpp     src/main.cpp
CHATDB_OBJ := $(CHATDB_SRC:.cpp=.o)

.PHONY: all clean

all: chatdb

chatdb: $(CHATDB_OBJ) $(IXWS_OBJ) $(HIREDIS_OBJ) libchatdb_deps.a
	$(CXX) $(CXXFLAGS) -o $@ $(CHATDB_OBJ) $(IXWS_OBJ) $(HIREDIS_OBJ) -L. -lchatdb_deps $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -Ithird_party/hiredis -c $< -o $@

clean:
	rm -f chatdb $(CHATDB_OBJ) $(IXWS_OBJ) $(HIREDIS_OBJ) libchatdb_deps.a
