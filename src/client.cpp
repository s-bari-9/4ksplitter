extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

#include <SDL3/SDL.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

#include "../include/protocol.h"

#define PORT 5000
#define MAX_UDP_PAYLOAD 1400

struct NetworkContext {
  int sock = -1;
};

bool setup_network(int port, NetworkContext &net_ctx) {
#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    std::cerr << "WSAStartup failed.\n";
    return false;
  }
#endif

  net_ctx.sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (net_ctx.sock < 0) {
    perror("socket");
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;
  int opt = 1;
  setsockopt(net_ctx.sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt,
             sizeof(opt));

  int rcvbuf = 1048576 * 10; // 10MB
  setsockopt(net_ctx.sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf,
             sizeof(rcvbuf));

#ifdef _WIN32
  u_long mode = 1; // non-blocking socket
  ioctlsocket(net_ctx.sock, FIONBIO, &mode);
#endif

  if (bind(net_ctx.sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return false;
  }
  return true;
}

void cleanup_network(NetworkContext &net_ctx) {
#ifdef _WIN32
  if (net_ctx.sock >= 0)
    closesocket(net_ctx.sock);
  WSACleanup();
#else
  if (net_ctx.sock >= 0)
    close(net_ctx.sock);
#endif
}

struct DecoderContext {
  AVBufferRef *hw_device_ctx = nullptr;
  AVCodecContext *ctx = nullptr;
  AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
};

static AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                   const AVPixelFormat *pix_fmts) {
  DecoderContext *dec_ctx = (DecoderContext *)ctx->opaque;
  for (const AVPixelFormat *p = pix_fmts; *p != -1; p++) {
    if (*p == dec_ctx->hw_pix_fmt) {
      return *p;
    }
  }
  std::cerr << "Failed to get HW surface format.\n";
  return AV_PIX_FMT_NONE;
}

bool setup_decoder(DecoderContext &dec_ctx) {
  const AVCodec *codec = avcodec_find_decoder_by_name("av1");
  if (!codec) {
    std::cerr << "AV1 decoder not found.\n";
    return false;
  }

  dec_ctx.ctx = avcodec_alloc_context3(codec);
  if (!dec_ctx.ctx)
    return false;

  AVHWDeviceType hw_priority[] = {
      AV_HWDEVICE_TYPE_VDPAU, AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_VAAPI,
      AV_HWDEVICE_TYPE_QSV,   AV_HWDEVICE_TYPE_DXVA2,   AV_HWDEVICE_TYPE_CUDA,
      AV_HWDEVICE_TYPE_NONE};

  for (int i = 0; hw_priority[i] != AV_HWDEVICE_TYPE_NONE; i++) {
    AVHWDeviceType type = hw_priority[i];
    if (av_hwdevice_ctx_create(&dec_ctx.hw_device_ctx, type, nullptr, nullptr,
                               0) >= 0) {
      std::cout << "Successfully initialized hardware decoder: "
                << av_hwdevice_get_type_name(type) << "\n";

      for (int j = 0;; j++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, j);
        if (!config) {
          std::cerr << "Decoder " << codec->name
                    << " does not support device type "
                    << av_hwdevice_get_type_name(type) << "\n";
          break;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == type) {
          dec_ctx.hw_pix_fmt = config->pix_fmt;
          break;
        }
      }

      if (dec_ctx.hw_pix_fmt != AV_PIX_FMT_NONE) {
        dec_ctx.ctx->hw_device_ctx = av_buffer_ref(dec_ctx.hw_device_ctx);
        dec_ctx.ctx->opaque = &dec_ctx;
        dec_ctx.ctx->get_format = get_hw_format;
        break; // Successfully configured hardware decoding
      } else {
        std::cerr << "Failed to find supported hardware pixel format for "
                  << av_hwdevice_get_type_name(type) << "\n";
        av_buffer_unref(&dec_ctx.hw_device_ctx);
      }
    }
  }

  if (!dec_ctx.hw_device_ctx) {
    std::cerr << "Warning: Failed to create ANY hardware context. Falling back "
                 "to simple software decoding.\n";
  }

  if (avcodec_open2(dec_ctx.ctx, codec, nullptr) < 0) {
    std::cerr << "Failed to open AV1 decoder\n";
    return false;
  }
  return true;
}

void cleanup_decoder(DecoderContext &dec_ctx) {
  if (dec_ctx.ctx)
    avcodec_free_context(&dec_ctx.ctx);
  if (dec_ctx.hw_device_ctx)
    av_buffer_unref(&dec_ctx.hw_device_ctx);
}

struct DisplayContext {
  SDL_Window *window = nullptr;
  SDL_Renderer *renderer = nullptr;
  SDL_Texture *texture = nullptr;
};

bool setup_display(DisplayContext &disp_ctx) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::cerr << "SDL_Init Error: " << SDL_GetError() << "\n";
    return false;
  }

  disp_ctx.window = SDL_CreateWindow("4K120 Multiplexer Stream", 1920, 1080,
                                     SDL_WINDOW_RESIZABLE);
  if (!disp_ctx.window) {
    std::cerr << "SDL_CreateWindow Error: " << SDL_GetError() << "\n";
    return false;
  }

  disp_ctx.renderer = SDL_CreateRenderer(disp_ctx.window, nullptr);
  if (!disp_ctx.renderer) {
    std::cerr << "SDL_CreateRenderer Error: " << SDL_GetError() << "\n";
    return false;
  }
  return true;
}

void cleanup_display(DisplayContext &disp_ctx) {
  if (disp_ctx.texture)
    SDL_DestroyTexture(disp_ctx.texture);
  if (disp_ctx.renderer)
    SDL_DestroyRenderer(disp_ctx.renderer);
  if (disp_ctx.window)
    SDL_DestroyWindow(disp_ctx.window);
  SDL_Quit();
}

struct StreamState {
  bool quit = false;
  bool first_recv = true;
  uint32_t expected_frame_id = 0;
  uint32_t dropped_frames = 0;
  uint32_t frames_rendered = 0;
  std::chrono::steady_clock::time_point stat_start_time;

  std::vector<uint8_t> extradata_buffer;
  std::map<uint32_t, FrameBuffer> frames;

  AVFrame *frame = nullptr;
  AVFrame *sw_frame = nullptr;
  AVPacket *pkt = nullptr;
};

void run_client_loop(NetworkContext &net_ctx, DecoderContext &dec_ctx,
                     DisplayContext &disp_ctx, StreamState &state) {
  state.frame = av_frame_alloc();
  state.sw_frame = av_frame_alloc();
  state.pkt = av_packet_alloc();
  state.stat_start_time = std::chrono::steady_clock::now();

  while (!state.quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) {
        state.quit = true;
      }
    }

    uint8_t buf[MAX_UDP_PAYLOAD + 12];
    sockaddr_in sender{};
    socklen_t sender_len = sizeof(sender);

#ifdef _WIN32
    ssize_t n = recvfrom(net_ctx.sock, (char *)buf, sizeof(buf), 0,
                         (sockaddr *)&sender, &sender_len);
#else
    ssize_t n = recvfrom(net_ctx.sock, buf, sizeof(buf), MSG_DONTWAIT,
                         (sockaddr *)&sender, &sender_len);
#endif

    if (n > 0) {
      PacketType type = (PacketType)buf[0];
      uint32_t frame_id = ntohl(*((uint32_t *)(buf + 1)));
      uint16_t packet_id = ntohs(*((uint16_t *)(buf + 5)));
      uint16_t total_packets = ntohs(*((uint16_t *)(buf + 7)));

      if (frame_id > state.expected_frame_id && type == FRAME) {
        state.dropped_frames += (frame_id - state.expected_frame_id);
        state.expected_frame_id = frame_id;
      }

      if (type == EXTRADATA) {
        if (state.extradata_buffer.size() < total_packets * MAX_UDP_PAYLOAD)
          state.extradata_buffer.resize(total_packets * MAX_UDP_PAYLOAD);
        memcpy(state.extradata_buffer.data() + packet_id * MAX_UDP_PAYLOAD,
               buf + 9, n - 9);

        if (packet_id + 1 == total_packets) {
          if (dec_ctx.ctx->extradata)
            av_freep(&dec_ctx.ctx->extradata);
          dec_ctx.ctx->extradata_size = state.extradata_buffer.size();
          dec_ctx.ctx->extradata = (uint8_t *)av_mallocz(
              dec_ctx.ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
          memcpy(dec_ctx.ctx->extradata, state.extradata_buffer.data(),
                 dec_ctx.ctx->extradata_size);
          std::cout << "Extradata received (" << dec_ctx.ctx->extradata_size
                    << " bytes)\n";
        }
        continue;
      }

      if (state.first_recv && type == FRAME) {
        std::cout << "First frame chunk received! Reassembling...\n";
        state.first_recv = false;
      }

      FrameBuffer &fb = state.frames[frame_id];
      if (fb.data.size() < total_packets * MAX_UDP_PAYLOAD) {
        fb.data.resize(total_packets * MAX_UDP_PAYLOAD);
      }
      memcpy(fb.data.data() + packet_id * MAX_UDP_PAYLOAD, buf + 9, n - 9);
      fb.total_chunks = total_packets;
      fb.received_chunks++;
      fb.actual_size += n - 9;
      fb.last_updated = std::chrono::steady_clock::now();

      if (fb.received_chunks == fb.total_chunks) {
        state.pkt->data = fb.data.data();
        state.pkt->size = fb.actual_size;

        if (avcodec_send_packet(dec_ctx.ctx, state.pkt) == 0) {
          while (avcodec_receive_frame(dec_ctx.ctx, state.frame) == 0) {
            AVFrame *target_frame = state.frame;

            if (state.frame->hw_frames_ctx) {
              if (av_hwframe_transfer_data(state.sw_frame, state.frame, 0) ==
                  0) {
                target_frame = state.sw_frame;
              } else {
                std::cerr << "Hardware transfer failed\n";
                continue;
              }
            }

            if (!disp_ctx.texture) {
              disp_ctx.texture =
                  SDL_CreateTexture(disp_ctx.renderer, SDL_PIXELFORMAT_NV12,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    target_frame->width, target_frame->height);
            }

            if (disp_ctx.texture && target_frame->format == AV_PIX_FMT_NV12) {
              SDL_UpdateNVTexture(
                  disp_ctx.texture, nullptr, target_frame->data[0],
                  target_frame->linesize[0], target_frame->data[1],
                  target_frame->linesize[1]);

              SDL_RenderClear(disp_ctx.renderer);
              SDL_RenderTexture(disp_ctx.renderer, disp_ctx.texture, nullptr,
                                nullptr);
              SDL_RenderPresent(disp_ctx.renderer);

              state.frames_rendered++;
              state.expected_frame_id = frame_id + 1;

              auto now = std::chrono::steady_clock::now();
              auto elapsed =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - state.stat_start_time)
                      .count();
              if (elapsed >= 1000) {
                double fps = state.frames_rendered * 1000.0 / elapsed;
                std::cout << "\r[FPS: " << static_cast<int>(fps)
                          << "] [Drops since start: " << state.dropped_frames
                          << "]   " << std::flush;
                state.frames_rendered = 0;
                state.stat_start_time = now;
              }
            }
          }
        }
        state.frames.erase(frame_id);
      }
    } else {
      SDL_Delay(1);
    }

    auto now = std::chrono::steady_clock::now();
    for (auto it = state.frames.begin(); it != state.frames.end();) {
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              now - it->second.last_updated)
              .count() > 500) {
        it = state.frames.erase(it);
      } else {
        ++it;
      }
    }
  }

  av_frame_free(&state.sw_frame);
  av_frame_free(&state.frame);
  av_packet_free(&state.pkt);
}

int main(int argc, char *argv[]) {
  int port = PORT;
  if (argc > 1) {
    port = std::atoi(argv[1]);
  }

  NetworkContext net_ctx;
  if (!setup_network(port, net_ctx))
    return 1;

  DecoderContext dec_ctx;
  if (!setup_decoder(dec_ctx)) {
    cleanup_network(net_ctx);
    return 1;
  }

  DisplayContext disp_ctx;
  if (!setup_display(disp_ctx)) {
    cleanup_decoder(dec_ctx);
    cleanup_network(net_ctx);
    return 1;
  }

  std::cout << "Client listening on port " << port << " for 1080p stream...\n";

  StreamState state;
  run_client_loop(net_ctx, dec_ctx, disp_ctx, state);

  cleanup_display(disp_ctx);
  cleanup_decoder(dec_ctx);
  cleanup_network(net_ctx);

  return 0;
}
