# ── Chat Project Makefile ──────────────────────────────────────────────────────
# Requirements:
#   sudo apt install protobuf-compiler libprotobuf-dev

CXX      := g++
# Use pkg-config for protobuf flags (includes abseil deps)
PROTOBUF_CFLAGS := $(shell pkg-config --cflags protobuf)
PROTOBUF_LIBS   := $(shell pkg-config --libs protobuf)

CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread $(PROTOBUF_CFLAGS)
PROTOC   := protoc

PROTO_DIR := protos
GEN_DIR   := gen
INC_FLAGS := -I$(GEN_DIR) -Iinclude
LIBS      := $(PROTOBUF_LIBS) -lpthread

SERVER_SRC := src/server/server.cpp
CLIENT_SRC := src/client/client.cpp

.PHONY: all server client protos clean

# ── Default: generate protos first, then build in a sub-make ─────────────────
all: protos
	$(MAKE) server client

# ── Generate all protobuf files into gen/ ────────────────────────────────────
protos:
	mkdir -p $(GEN_DIR)
	$(PROTOC) -I$(PROTO_DIR) --cpp_out=$(GEN_DIR) \
		$(PROTO_DIR)/common.proto \
		$(PROTO_DIR)/client-side/change_status.proto \
		$(PROTO_DIR)/client-side/get_user_info.proto \
		$(PROTO_DIR)/client-side/list_users.proto \
		$(PROTO_DIR)/client-side/message_dm.proto \
		$(PROTO_DIR)/client-side/message_general.proto \
		$(PROTO_DIR)/client-side/quit.proto \
		$(PROTO_DIR)/client-side/register.proto \
		$(PROTO_DIR)/server-side/all_users.proto \
		$(PROTO_DIR)/server-side/broadcast_messages.proto \
		$(PROTO_DIR)/server-side/for_dm.proto \
		$(PROTO_DIR)/server-side/get_user_info_response.proto \
		$(PROTO_DIR)/server-side/server_response.proto

# ── Server (run after protos) ─────────────────────────────────────────────────
server:
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) \
		$(SERVER_SRC) \
		$(GEN_DIR)/*.pb.cc \
		$(GEN_DIR)/client-side/*.pb.cc \
		$(GEN_DIR)/server-side/*.pb.cc \
		$(LIBS) -o server

# ── Client (run after protos) ─────────────────────────────────────────────────
client:
	$(CXX) $(CXXFLAGS) $(INC_FLAGS) \
		$(CLIENT_SRC) \
		$(GEN_DIR)/*.pb.cc \
		$(GEN_DIR)/client-side/*.pb.cc \
		$(GEN_DIR)/server-side/*.pb.cc \
		$(LIBS) -o client

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(GEN_DIR) server client
