#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#define MAX_UDP_PAYLOAD 1400

enum PacketType : uint8_t { EXTRADATA = 0, FRAME = 1 };

struct FrameBuffer {
  std::vector<uint8_t> data;
  size_t total_chunks = 0;
  size_t received_chunks = 0;
  size_t actual_size = 0;
  std::chrono::steady_clock::time_point last_updated;
};
