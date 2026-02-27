extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "../include/protocol.h"
#include <cstring>
#include <iostream>

#define TARGET_WIDTH 3840
#define TARGET_HEIGHT 2160
#define FPS 120
#define TARGET_OUTPUTS 4

#define IP "127.0.0.1"

struct Target {
  std::string ip;
  uint16_t port;
};

bool parse_target(const std::string &s, Target &out) {
  auto pos = s.find(':');
  if (pos == std::string::npos) {
    return false;
  }
  out.ip = s.substr(0, pos);
  int port;
  try {
    port = std::stoi(s.substr(pos + 1));
  } catch (...) {
    return false;
  }
  if (port < 0 || port > 65535) {
    return false;
  }
  out.port = static_cast<uint16_t>(port);
  return true;
}

struct Config {
  bool show_help = false;
  bool use_file = false;

  std::string input_source;
  std::string input_format;
  std::string target_encoder;

  std::vector<std::string> target_ips;
  std::vector<Target> targets;
};

bool parse_args(int argc, char *argv[], Config &cfg) {
#ifdef _WIN32
  cfg.input_source = "desktop";
  cfg.input_format = "gdigrab";
#else
  cfg.input_source = ":0.0";
  cfg.input_format = "x11grab";
#endif

  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
      cfg.show_help = true;
      return true;
    }
    if (!std::strcmp(argv[i], "-f") && i + 1 < argc) {
      cfg.use_file = true;
      cfg.input_source = argv[i + 1];
    }
  }

  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "-f")) {
      ++i;
    } else if (!std::strcmp(argv[i], "-t")) {
      if (cfg.use_file) {
        std::cerr << "-t is invalid when using -f\n";
        return false;
      }
      if (i + 1 >= argc)
        return false;
      cfg.input_format = argv[++i];
    } else if (!std::strcmp(argv[i], "-i")) {
      if (cfg.use_file) {
        std::cerr << "-i is invalid when using -f\n";
        return false;
      }
      if (i + 1 >= argc)
        return false;
      cfg.input_source = argv[++i];
    } else if (!std::strcmp(argv[i], "-c:v")) {
      if (i + 1 >= argc)
        return false;
      cfg.target_encoder = argv[++i];
    } else if (argv[i][0] == '-') {
      std::cerr << "Unknown option: " << argv[i] << "\n";
      return false;
    } else {
      Target t;
      if (!parse_target(argv[i], t)) {
        std::cerr << "Invalid target: " << argv[i] << "\n";
        return false;
      }
      cfg.targets.push_back(t);
    }
  }
  if (cfg.target_ips.empty()) {
    for (uint16_t i = 5000; i < 5004; i++) {
      cfg.targets.push_back(Target{"127.0.0.1", i});
    }
  }
  while (cfg.targets.size() < 4) {
    cfg.targets.push_back(cfg.targets.back());
  }

  return true;
}

void print_help(char name[]) {
  std::cout << "Usage: " << name << " [options] [ip1] [ip2] [ip3] [ip4]\n"
            << "Options:\n"
            << "  -h, --help        Show this help message\n"
            << "  -f <file>         Use a video file as input (overrides "
               "-t and -i)\n"
            << "  -t <format>       Input format (default: x11grab/gdigrab). "
               "e.g., v4l2, x11grab\n"
            << "  -c:v <encoder>    Set target encoder (e.g., nvenc(default), "
               "libx264)\n"
            << "  -i <source>       Input source (default: :0.0). e.g., "
               "/dev/video0, :99.0\n"
            << "Example for Xvfb:   " << name
            << " -t x11grab -i :99.0 127.0.0.1\n"
            << "Example for File:   " << name << " -f test.mkv 192.168.1.100\n";
}

struct NetworkContext {
  int sock = -1;
  std::vector<sockaddr_in> destinations;
};

bool setup_network(const Config &cfg, NetworkContext &net_ctx) {
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

  net_ctx.destinations.resize(TARGET_OUTPUTS);

  for (int i = 0; i < TARGET_OUTPUTS; ++i) {
    net_ctx.destinations[i] = {};
    net_ctx.destinations[i].sin_family = AF_INET;
    std::cout << "Target " << i << ": " << cfg.targets[i].ip << ":"
              << cfg.targets[i].port << std::endl;
    net_ctx.destinations[i].sin_port = htons(cfg.targets[i].port);

    if (inet_pton(AF_INET, cfg.targets[i].ip.c_str(),
                  &net_ctx.destinations[i].sin_addr) <= 0) {
      std::cerr << "Invalid IP address: " << cfg.targets[i].ip << "\n";
      return false;
    }
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
  AVFormatContext *fmt_ctx = nullptr;
  AVCodecContext *dec_ctx = nullptr;
  int video_stream_idx = -1;
};

bool setup_decoder(const Config &cfg, DecoderContext &dec_ctx) {
  const AVInputFormat *iformat = nullptr;
  AVDictionary *options = nullptr;

  if (!cfg.use_file) {
    iformat = av_find_input_format(cfg.input_format.c_str());
    if (!iformat) {
      std::cerr << "Input format '" << cfg.input_format << "' not found.\n";
      return false;
    }

    if (cfg.input_format == "x11grab" || cfg.input_format == "v4l2" ||
        cfg.input_format == "video4linux2") {
      av_dict_set(&options, "video_size", "3840x2160", 0);
      av_dict_set(&options, "framerate", "120", 0);
    }
  }

  std::cout << "Opening input source: " << cfg.input_source
            << " (File: " << (cfg.use_file ? "yes" : "no") << ")\n";

  if (avformat_open_input(&dec_ctx.fmt_ctx, cfg.input_source.c_str(), iformat,
                          &options) < 0) {
    std::cerr << "Could not open input source.\n";
    return false;
  }
  avformat_find_stream_info(dec_ctx.fmt_ctx, nullptr);

  dec_ctx.video_stream_idx = -1;
  for (unsigned int i = 0; i < dec_ctx.fmt_ctx->nb_streams; i++) {
    if (dec_ctx.fmt_ctx->streams[i]->codecpar->codec_type ==
        AVMEDIA_TYPE_VIDEO) {
      dec_ctx.video_stream_idx = i;
      break;
    }
  }
  if (dec_ctx.video_stream_idx == -1) {
    std::cerr << "No video stream\n";
    return false;
  }

  const AVCodec *dec_codec = avcodec_find_decoder(
      dec_ctx.fmt_ctx->streams[dec_ctx.video_stream_idx]->codecpar->codec_id);
  dec_ctx.dec_ctx = avcodec_alloc_context3(dec_codec);
  avcodec_parameters_to_context(
      dec_ctx.dec_ctx,
      dec_ctx.fmt_ctx->streams[dec_ctx.video_stream_idx]->codecpar);
  if (avcodec_open2(dec_ctx.dec_ctx, dec_codec, nullptr) < 0) {
    std::cerr << "Failed to open decoder\n";
    return false;
  }
  return true;
}

void cleanup_decoder(DecoderContext &dec_ctx) {
  if (dec_ctx.dec_ctx)
    avcodec_free_context(&dec_ctx.dec_ctx);
  if (dec_ctx.fmt_ctx)
    avformat_close_input(&dec_ctx.fmt_ctx);
}

struct EncoderContext {
  AVCodecContext *enc_ctx[4] = {nullptr, nullptr, nullptr, nullptr};
};

bool setup_encoders(const Config &cfg, EncoderContext &enc_ctx) {
  std::vector<std::string> encoder_priority = {"av1_nvenc",  "hevc_nvenc",
                                               "h264_qsv",   "h264_amf",
                                               "h264_vaapi", "libx264"};
  if (!cfg.target_encoder.empty()) {
    encoder_priority.insert(encoder_priority.begin(), cfg.target_encoder);
  }

  const AVCodec *enc_codec = nullptr;
  std::string final_encoder_name = "";

  for (const auto &enc_name : encoder_priority) {
    enc_codec = avcodec_find_encoder_by_name(enc_name.c_str());
    if (enc_codec) {
      final_encoder_name = enc_name;
      std::cout << "Selected Encoder: " << final_encoder_name << "\n";
      break;
    }
  }

  if (!enc_codec) {
    std::cerr << "Fatal: Could not find any suitable video encoders!\n";
    return false;
  }

  for (int i = 0; i < 4; i++) {
    enc_ctx.enc_ctx[i] = avcodec_alloc_context3(enc_codec);
    enc_ctx.enc_ctx[i]->width = TARGET_WIDTH / 2;
    enc_ctx.enc_ctx[i]->height = TARGET_HEIGHT / 2;
    enc_ctx.enc_ctx[i]->time_base = {1, FPS};
    enc_ctx.enc_ctx[i]->framerate = {FPS, 1};
    enc_ctx.enc_ctx[i]->pix_fmt = AV_PIX_FMT_NV12;
    enc_ctx.enc_ctx[i]->bit_rate = 10000000;
    enc_ctx.enc_ctx[i]->gop_size = 20;
    enc_ctx.enc_ctx[i]->max_b_frames = 0;
    enc_ctx.enc_ctx[i]->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // av_opt_set(enc_ctx.enc_ctx[i]->priv_data, "preset", "p1", 0);
    av_opt_set(enc_ctx.enc_ctx[i]->priv_data, "tune", "ull", 0);
    av_opt_set(enc_ctx.enc_ctx[i]->priv_data, "tune", "zerolatency", 0);
    av_opt_set(enc_ctx.enc_ctx[i]->priv_data, "profile", "main", 0);
    av_opt_set(enc_ctx.enc_ctx[i]->priv_data, "rc", "cbr", 0);
    av_opt_set(enc_ctx.enc_ctx[i]->priv_data, "delay", "0", 0);

    if (avcodec_open2(enc_ctx.enc_ctx[i], enc_codec, nullptr) < 0) {
      std::cerr << "Failed to open encoder " << i << " (" << final_encoder_name
                << ")\n";
      return false;
    }
  }
  return true;
}

void cleanup_encoders(EncoderContext &enc_ctx) {
  for (int i = 0; i < 4; i++) {
    if (enc_ctx.enc_ctx[i])
      avcodec_free_context(&enc_ctx.enc_ctx[i]);
  }
}

void send_quad_to_client(const NetworkContext &net_ctx, int quad_idx,
                         const uint8_t *buf, size_t len) {
  sendto(net_ctx.sock, (const char *)buf, len, 0,
         (sockaddr *)&net_ctx.destinations[quad_idx],
         sizeof(net_ctx.destinations[quad_idx]));
}

void send_extradata(const NetworkContext &net_ctx,
                    const EncoderContext &enc_ctx) {
  for (int q = 0; q < 4; q++) {
    if (enc_ctx.enc_ctx[q]->extradata &&
        enc_ctx.enc_ctx[q]->extradata_size > 0) {
      size_t total_chunks =
          (enc_ctx.enc_ctx[q]->extradata_size + MAX_UDP_PAYLOAD - 1) /
          MAX_UDP_PAYLOAD;
      for (size_t i = 0; i < total_chunks; i++) {
        size_t offset = i * MAX_UDP_PAYLOAD;
        size_t chunk_size = std::min<size_t>(
            MAX_UDP_PAYLOAD, enc_ctx.enc_ctx[q]->extradata_size - offset);
        uint8_t buf[MAX_UDP_PAYLOAD + 12];
        buf[0] = EXTRADATA;
        uint32_t net_frame_id = htonl(0);
        uint16_t net_packet_id = htons(i);
        uint16_t net_total = htons(total_chunks);
        memcpy(buf + 1, &net_frame_id, 4);
        memcpy(buf + 5, &net_packet_id, 2);
        memcpy(buf + 7, &net_total, 2);
        memcpy(buf + 9, enc_ctx.enc_ctx[q]->extradata + offset, chunk_size);
        send_quad_to_client(net_ctx, q, buf, chunk_size + 9);
      }
    }
  }
}

void server_loop(const NetworkContext &net_ctx, DecoderContext &dec_ctx,
                 EncoderContext &enc_ctx) {
  AVFrame *in_frame = av_frame_alloc();
  AVFrame *quad_frame[4] = {nullptr, nullptr, nullptr, nullptr};
  AVPacket *in_pkt = av_packet_alloc();
  AVPacket *out_pkt = av_packet_alloc();
  uint32_t output_frame_id = 0;

  while (av_read_frame(dec_ctx.fmt_ctx, in_pkt) >= 0) {
    if (in_pkt->stream_index == dec_ctx.video_stream_idx) {
      if (avcodec_send_packet(dec_ctx.dec_ctx, in_pkt) == 0) {
        while (avcodec_receive_frame(dec_ctx.dec_ctx, in_frame) == 0) {
#ifdef _WIN32
          Sleep(1000 / FPS);
#else
          usleep(1000000 / FPS);
#endif

          AVFrame *process_frame = in_frame;
          AVFrame *sw_frame = nullptr;

          if (in_frame->hw_frames_ctx) {
            sw_frame = av_frame_alloc();
            if (!sw_frame ||
                av_hwframe_transfer_data(sw_frame, in_frame, 0) < 0) {
              if (sw_frame)
                av_frame_free(&sw_frame);
              continue;
            }
            process_frame = sw_frame;
          }

          AVFrame *nv12_frame = process_frame;
          AVFrame *converted_frame = nullptr;
          struct SwsContext *sws = nullptr;

          if (process_frame->format != AV_PIX_FMT_NV12) {
            converted_frame = av_frame_alloc();
            converted_frame->format = AV_PIX_FMT_NV12;
            converted_frame->width = process_frame->width;
            converted_frame->height = process_frame->height;
            av_frame_get_buffer(converted_frame, 32);

            sws = sws_getContext(process_frame->width, process_frame->height,
                                 (AVPixelFormat)process_frame->format,
                                 process_frame->width, process_frame->height,
                                 AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr,
                                 nullptr, nullptr);
            if (sws) {
              sws_scale(sws, process_frame->data, process_frame->linesize, 0,
                        process_frame->height, converted_frame->data,
                        converted_frame->linesize);
              nv12_frame = converted_frame;
            } else {
              av_frame_free(&converted_frame);
              if (sw_frame)
                av_frame_free(&sw_frame);
              continue;
            }
          }

          if (quad_frame[0] == nullptr) {
            for (int i = 0; i < 4; i++) {
              quad_frame[i] = av_frame_alloc();
              quad_frame[i]->format = AV_PIX_FMT_NV12;
              quad_frame[i]->width = TARGET_WIDTH / 2;
              quad_frame[i]->height = TARGET_HEIGHT / 2;
              av_frame_get_buffer(quad_frame[i], 32);
            }
          }

          int half_w = nv12_frame->width / 2;
          int half_h = nv12_frame->height / 2;

          for (int q = 0; q < 4; q++) {
            int start_x = (q % 2) * half_w;
            int start_y = (q / 2) * half_h;

            quad_frame[q]->pts = output_frame_id;
            av_frame_make_writable(quad_frame[q]);

            for (int y = 0; y < half_h; y++) {
              memcpy(quad_frame[q]->data[0] + y * quad_frame[q]->linesize[0],
                     nv12_frame->data[0] +
                         (start_y + y) * nv12_frame->linesize[0] + start_x,
                     half_w);
            }

            for (int y = 0; y < half_h / 2; y++) {
              memcpy(quad_frame[q]->data[1] + y * quad_frame[q]->linesize[1],
                     nv12_frame->data[1] +
                         (start_y / 2 + y) * nv12_frame->linesize[1] + start_x,
                     half_w);
            }

            if (avcodec_send_frame(enc_ctx.enc_ctx[q], quad_frame[q]) == 0) {
              while (avcodec_receive_packet(enc_ctx.enc_ctx[q], out_pkt) == 0) {
                size_t total_chunks =
                    (out_pkt->size + MAX_UDP_PAYLOAD - 1) / MAX_UDP_PAYLOAD;
                for (size_t i = 0; i < total_chunks; i++) {
                  size_t offset = i * MAX_UDP_PAYLOAD;
                  size_t chunk_size =
                      std::min<size_t>(MAX_UDP_PAYLOAD, out_pkt->size - offset);
                  uint8_t buf[MAX_UDP_PAYLOAD + 12];
                  buf[0] = FRAME;
                  uint32_t net_frame_id = htonl(output_frame_id);
                  uint16_t net_packet_id = htons(i);
                  uint16_t net_total = htons(total_chunks);
                  memcpy(buf + 1, &net_frame_id, 4);
                  memcpy(buf + 5, &net_packet_id, 2);
                  memcpy(buf + 7, &net_total, 2);
                  memcpy(buf + 9, out_pkt->data + offset, chunk_size);

                  send_quad_to_client(net_ctx, q, buf, chunk_size + 9);
                }
                av_packet_unref(out_pkt);
              }
            }
          }
          output_frame_id++;
          std::cout << "Sent 4 quadrants for frame " << output_frame_id << "\r"
                    << std::flush;

          if (converted_frame)
            av_frame_free(&converted_frame);
          if (sw_frame)
            av_frame_free(&sw_frame);
          if (sws)
            sws_freeContext(sws);
        }
      }
    }
    av_packet_unref(in_pkt);
  }

  std::cout << "\nFinished streaming file.\n";

  av_frame_free(&in_frame);
  for (int i = 0; i < 4; i++) {
    if (quad_frame[i])
      av_frame_free(&quad_frame[i]);
  }
  av_packet_free(&in_pkt);
  av_packet_free(&out_pkt);
}

int main(int argc, char *argv[]) {
  Config cfg;
  avdevice_register_all();

  if (!parse_args(argc, argv, cfg)) {
    return 1;
  }

  if (cfg.show_help) {
    print_help(argv[0]);
    return 0;
  }

  NetworkContext net_ctx;
  if (!setup_network(cfg, net_ctx))
    return 1;

  DecoderContext dec_ctx;
  if (!setup_decoder(cfg, dec_ctx)) {
    cleanup_network(net_ctx);
    return 1;
  }

  EncoderContext enc_ctx;
  if (!setup_encoders(cfg, enc_ctx)) {
    cleanup_decoder(dec_ctx);
    cleanup_network(net_ctx);
    return 1;
  }

  send_extradata(net_ctx, enc_ctx);
  server_loop(net_ctx, dec_ctx, enc_ctx);

  cleanup_encoders(enc_ctx);
  cleanup_decoder(dec_ctx);
  cleanup_network(net_ctx);

  return 0;
}
