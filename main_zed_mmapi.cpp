// Plan A: orinvideosender3 -- Jetson Multimedia API direct path.
//
// Drop-in TCP wire-protocol replacement for orinvideosender2 (PICO sees the
// same OPEN_CAMERA / CLOSE_CAMERA handshake and the same 4-byte-length-prefixed
// H.264 packets). The internal stack is completely different:
//
//   orinvideosender2 (Phase 1):
//     ZED -> CPU BGRx -> GStreamer (appsrc -> nvvidconv -> NVMM/NV12
//            -> nvv4l2h264enc -> h264parse -> appsink) -> TCP
//
//   orinvideosender3 (Plan A):
//     ZED -> CPU BGRA -> NvBufSurface (BGRA, NVBUF_MEM_SURFACE_ARRAY)
//          -> NvBufSurfTransform (VIC, BGRA -> NV12)
//          -> NvBufSurface (NV12) -> memcpy -> NvVideoEncoder output plane
//          -> NVENC (V4L2 M2M) -> capture plane DQ callback -> TCP send
//
// Removed vs orinvideosender2: GStreamer, OpenCV, ZMQ, videoconvert. Only the
// network protocol stays compatible.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <linux/videodev2.h>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <NvBuffer.h>
#include <NvVideoEncoder.h>
#include <nvbufsurface.h>
#include <nvbufsurftransform.h>
#include <sl/Camera.hpp>

#include "network_helper.hpp"

// ---------------------------------------------------------------------------
// Network protocol (matches orinvideosender2 / PICO XRoboToolkit client).
// ---------------------------------------------------------------------------
struct CameraRequestData {
  int width = 0;
  int height = 0;
  int fps = 0;
  int bitrate = 0;
  int enableMvHevc = 0;
  int renderMode = 0;
  int port = 0;
  std::string camera;
  std::string ip;
};

struct NetworkDataProtocol {
  std::string command;
  std::vector<uint8_t> data;
};

namespace {

int32_t readInt32(const std::vector<uint8_t> &d, size_t off) {
  return static_cast<int32_t>((d[off]) | (d[off + 1] << 8) | (d[off + 2] << 16) |
                               (d[off + 3] << 24));
}

std::string readCompactString(const std::vector<uint8_t> &d, size_t &off) {
  if (off >= d.size()) throw std::out_of_range("compact str len OOB");
  uint8_t len = d[off++];
  if (len == 0) return std::string();
  if (off + len > d.size()) throw std::out_of_range("compact str body OOB");
  std::string s(reinterpret_cast<const char *>(&d[off]), len);
  off += len;
  return s;
}

CameraRequestData deserializeCameraRequest(const std::vector<uint8_t> &d) {
  if (d.size() < 10) throw std::invalid_argument("camera request too small");
  if (d[0] != 0xCA || d[1] != 0xFE) throw std::invalid_argument("bad magic");
  if (d[2] != 1) throw std::invalid_argument("bad version");
  if (d.size() < 3 + 28) throw std::invalid_argument("camera request truncated");
  CameraRequestData r;
  size_t off = 3;
  r.width = readInt32(d, off);
  r.height = readInt32(d, off + 4);
  r.fps = readInt32(d, off + 8);
  r.bitrate = readInt32(d, off + 12);
  r.enableMvHevc = readInt32(d, off + 16);
  r.renderMode = readInt32(d, off + 20);
  r.port = readInt32(d, off + 24);
  off += 28;
  r.camera = readCompactString(d, off);
  r.ip = readCompactString(d, off);
  return r;
}

NetworkDataProtocol deserializeNetworkData(const std::vector<uint8_t> &b) {
  if (b.size() < 8) throw std::invalid_argument("protocol buf too small");
  size_t off = 0;
  int32_t cmdLen = readInt32(b, off);
  off += 4;
  if (cmdLen < 0 || off + cmdLen > b.size())
    throw std::invalid_argument("bad cmd length");
  NetworkDataProtocol r;
  if (cmdLen > 0) {
    r.command.assign(reinterpret_cast<const char *>(&b[off]), cmdLen);
    size_t nul = r.command.find('\0');
    if (nul != std::string::npos) r.command = r.command.substr(0, nul);
  }
  off += cmdLen;
  if (off + 4 > b.size()) throw std::invalid_argument("bad data length pos");
  int32_t dataLen = readInt32(b, off);
  off += 4;
  if (dataLen < 0 || off + dataLen > b.size())
    throw std::invalid_argument("bad data length");
  if (dataLen > 0) r.data.assign(b.begin() + off, b.begin() + off + dataLen);
  return r;
}

std::string toLowerCopy(std::string v) {
  std::transform(v.begin(), v.end(), v.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return v;
}

bool isSupportedZedCameraName(const std::string &camera) {
  std::string n = toLowerCopy(camera);
  return n == "zed" || n == "zedmini" || n == "zed_mini" || n == "zed-mini";
}

// ---------------------------------------------------------------------------
// Global runtime state.
// ---------------------------------------------------------------------------
std::atomic<bool> stop_requested{false};
std::atomic<bool> streaming_active{false};
std::atomic<bool> send_enabled{false};

std::string zed_view_mode = "sbs";    // "sbs" or "left"
int idr_interval = 15;
int stats_interval = 120;
int bitrate_override = 0;             // 0 = use what PICO requests
std::string dump_h264_path;           // empty => no dump
std::ofstream dump_h264_stream;
std::mutex dump_h264_mutex;

std::unique_ptr<std::thread> listen_thread;
std::unique_ptr<std::thread> streaming_thread;
std::mutex config_mutex;
std::mutex streaming_mutex;
CameraRequestData current_camera_config;

std::unique_ptr<TCPClient> sender_ptr;
std::unique_ptr<TCPServer> server_ptr;
std::string send_to_server;
int send_to_port = 0;
std::mutex sender_mutex;

// ---------------------------------------------------------------------------
// Timing stats.
// ---------------------------------------------------------------------------
using SteadyClock = std::chrono::steady_clock;

struct Stats {
  uint64_t grabbed = 0;
  uint64_t encoded = 0;
  uint64_t tcp_sent = 0;
  double grab_ms = 0, retrieve_ms = 0, bgra_copy_ms = 0, vic_ms = 0,
         enc_push_ms = 0, tcp_send_ms = 0;
  SteadyClock::time_point last_log = SteadyClock::now();
  uint64_t grabbed_at_last_log = 0;
};
std::mutex stats_mutex;
Stats stats;

double elapsedMs(const SteadyClock::time_point &a,
                 const SteadyClock::time_point &b) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(b -
                                                                                a)
      .count();
}

void recordGrabFrame(double grab, double retr, double copy, double vic,
                     double push) {
  std::lock_guard<std::mutex> g(stats_mutex);
  stats.grabbed++;
  stats.grab_ms += grab;
  stats.retrieve_ms += retr;
  stats.bgra_copy_ms += copy;
  stats.vic_ms += vic;
  stats.enc_push_ms += push;
}

void recordEncoded(double tcp_ms, bool sent) {
  std::lock_guard<std::mutex> g(stats_mutex);
  stats.encoded++;
  if (sent) {
    stats.tcp_sent++;
    stats.tcp_send_ms += tcp_ms;
  }
}

void maybeLogStats() {
  std::lock_guard<std::mutex> g(stats_mutex);
  if (stats_interval <= 0 || stats.grabbed == 0 ||
      stats.grabbed - stats.grabbed_at_last_log <
          static_cast<uint64_t>(stats_interval)) {
    return;
  }
  SteadyClock::time_point now = SteadyClock::now();
  double dt = std::chrono::duration_cast<std::chrono::duration<double>>(
                  now - stats.last_log)
                  .count();
  double fps = dt > 0 ? (stats.grabbed - stats.grabbed_at_last_log) / dt : 0.0;
  double g_avg = stats.grabbed ? stats.grab_ms / stats.grabbed : 0;
  double r_avg = stats.grabbed ? stats.retrieve_ms / stats.grabbed : 0;
  double c_avg = stats.grabbed ? stats.bgra_copy_ms / stats.grabbed : 0;
  double v_avg = stats.grabbed ? stats.vic_ms / stats.grabbed : 0;
  double p_avg = stats.grabbed ? stats.enc_push_ms / stats.grabbed : 0;
  double t_avg = stats.tcp_sent ? stats.tcp_send_ms / stats.tcp_sent : 0;

  std::cout.setf(std::ios::fixed);
  std::cout.precision(2);
  std::cout << "[stats] fps=" << fps
            << " frames=" << stats.grabbed
            << " grab=" << g_avg << "ms"
            << " retrieve=" << r_avg << "ms"
            << " bgra_copy=" << c_avg << "ms"
            << " vic=" << v_avg << "ms"
            << " enc_push=" << p_avg << "ms"
            << " tcp_send=" << t_avg << "ms"
            << " encoded=" << stats.encoded
            << " tcp_sent=" << stats.tcp_sent << std::endl;
  stats.last_log = now;
  stats.grabbed_at_last_log = stats.grabbed;
}

// ---------------------------------------------------------------------------
// Encoder ownership + capture-plane callback (TCP-send).
// ---------------------------------------------------------------------------
struct EncoderState {
  NvVideoEncoder *enc = nullptr;
  std::atomic<bool> got_error{false};
  std::atomic<bool> got_eos{false};
};
EncoderState enc_state;  // singleton -- DQ callback needs a stable pointer

bool capturePlaneDqCallback(struct v4l2_buffer *v4l2_buf, NvBuffer *buffer,
                            NvBuffer *shared_buffer, void *arg) {
  (void)shared_buffer;
  EncoderState *st = static_cast<EncoderState *>(arg);
  if (!v4l2_buf) {
    std::cerr << "capture plane: null v4l2_buf" << std::endl;
    st->got_error.store(true);
    return false;
  }

  if (buffer->planes[0].bytesused > 0 && send_enabled.load()) {
    const uint8_t *data = buffer->planes[0].data;
    size_t size = buffer->planes[0].bytesused;
    std::vector<uint8_t> pkt(4 + size);
    pkt[0] = (size >> 24) & 0xFF;
    pkt[1] = (size >> 16) & 0xFF;
    pkt[2] = (size >> 8) & 0xFF;
    pkt[3] = (size) & 0xFF;
    std::memcpy(pkt.data() + 4, data, size);

    // Optional: write the raw H.264 bytes (no length prefix) to disk so we
    // can verify the stream is well-formed via ffplay independently of PICO.
    if (dump_h264_stream.is_open()) {
      std::lock_guard<std::mutex> dg(dump_h264_mutex);
      dump_h264_stream.write(reinterpret_cast<const char *>(data),
                             static_cast<std::streamsize>(size));
    }

    SteadyClock::time_point t0 = SteadyClock::now();
    bool sent = false;
    try {
      std::lock_guard<std::mutex> lg(sender_mutex);
      if (sender_ptr && sender_ptr->isConnected()) {
        sender_ptr->sendData(pkt);
        sent = true;
      }
    } catch (const TCPException &e) {
      std::cerr << "TCP send failed: " << e.what() << std::endl;
      streaming_active.store(false);
    }
    recordEncoded(elapsedMs(t0, SteadyClock::now()), sent);
  }

  if (st->enc->capture_plane.qBuffer(*v4l2_buf, nullptr) < 0) {
    std::cerr << "Re-queue capture-plane buffer failed" << std::endl;
    st->got_error.store(true);
    return false;
  }

  if (buffer->planes[0].bytesused == 0) {
    st->got_eos.store(true);
    return false;
  }

  maybeLogStats();
  return true;
}

// ---------------------------------------------------------------------------
// NvBufSurface helpers (M1/M2 patterns).
// ---------------------------------------------------------------------------
int allocateBgraSurface(NvBufSurface **out, uint32_t w, uint32_t h) {
  NvBufSurfaceAllocateParams p;
  std::memset(&p, 0, sizeof(p));
  p.params.width = w;
  p.params.height = h;
  p.params.colorFormat = NVBUF_COLOR_FORMAT_BGRA;
  p.params.layout = NVBUF_LAYOUT_PITCH;
  p.params.memType = NVBUF_MEM_SURFACE_ARRAY;
  p.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
  int ret = NvBufSurfaceAllocate(out, 1, &p);
  if (ret != 0 || !*out) return -1;
  (*out)->numFilled = 1;
  return 0;
}

int allocateNv12Surface(NvBufSurface **out, uint32_t w, uint32_t h) {
  NvBufSurfaceAllocateParams p;
  std::memset(&p, 0, sizeof(p));
  p.params.width = w;
  p.params.height = h;
  p.params.colorFormat = NVBUF_COLOR_FORMAT_NV12;
  p.params.layout = NVBUF_LAYOUT_PITCH;
  p.params.memType = NVBUF_MEM_SURFACE_ARRAY;
  p.memtag = NvBufSurfaceTag_VIDEO_ENC;
  int ret = NvBufSurfaceAllocate(out, 1, &p);
  if (ret != 0 || !*out) return -1;
  (*out)->numFilled = 1;
  return 0;
}

int copyBgraIntoSurface(NvBufSurface *surf, const uint8_t *src, uint32_t src_pitch,
                        uint32_t w, uint32_t h) {
  if (NvBufSurfaceMap(surf, 0, 0, NVBUF_MAP_READ_WRITE) != 0) return -1;
  NvBufSurfaceSyncForCpu(surf, 0, 0);
  NvBufSurfaceParams &p = surf->surfaceList[0];
  uint8_t *dst = static_cast<uint8_t *>(p.mappedAddr.addr[0]);
  const uint32_t dst_pitch = p.planeParams.pitch[0];
  const uint32_t row_bytes = w * 4;
  for (uint32_t y = 0; y < h; ++y) {
    std::memcpy(dst + static_cast<size_t>(y) * dst_pitch,
                src + static_cast<size_t>(y) * src_pitch, row_bytes);
  }
  NvBufSurfaceSyncForDevice(surf, 0, 0);
  NvBufSurfaceUnMap(surf, 0, 0);
  return 0;
}

int copyNv12IntoEncoderBuffer(NvBufSurface *src_surf, NvBuffer *dst_buffer,
                              uint32_t *y_bytes, uint32_t *uv_bytes) {
  if (NvBufSurfaceMap(src_surf, 0, -1, NVBUF_MAP_READ_WRITE) != 0) return -1;
  NvBufSurfaceSyncForCpu(src_surf, 0, -1);
  NvBufSurfaceParams &p = src_surf->surfaceList[0];
  for (uint32_t pl = 0;
       pl < p.planeParams.num_planes && pl < dst_buffer->n_planes; ++pl) {
    const uint8_t *sp = static_cast<uint8_t *>(p.mappedAddr.addr[pl]);
    const uint32_t s_pitch = p.planeParams.pitch[pl];
    const uint32_t bytes_per_row =
        p.planeParams.width[pl] * p.planeParams.bytesPerPix[pl];
    const uint32_t h = p.planeParams.height[pl];
    NvBuffer::NvBufferPlane &dp = dst_buffer->planes[pl];
    uint8_t *dest = dp.data;
    const uint32_t d_pitch = dp.fmt.stride;
    for (uint32_t y = 0; y < h; ++y) {
      std::memcpy(dest + static_cast<size_t>(y) * d_pitch,
                  sp + static_cast<size_t>(y) * s_pitch, bytes_per_row);
    }
    dp.bytesused = bytes_per_row * h;
    if (pl == 0 && y_bytes) *y_bytes = dp.bytesused;
    if (pl == 1 && uv_bytes) *uv_bytes = dp.bytesused;
  }
  NvBufSurfaceUnMap(src_surf, 0, -1);
  NvBufSurface *enc_surf = nullptr;
  if (NvBufSurfaceFromFd(dst_buffer->planes[0].fd, (void **)&enc_surf) == 0 &&
      enc_surf) {
    NvBufSurfaceSyncForDevice(enc_surf, -1, -1);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Streaming thread.
// ---------------------------------------------------------------------------

bool initializeSender() {
  for (int retry = 0; retry < 10 && !stop_requested.load(); ++retry) {
    try {
      std::lock_guard<std::mutex> lg(sender_mutex);
      sender_ptr.reset(new TCPClient(send_to_server, send_to_port));
      std::cout << "Connecting back to " << send_to_server << ":"
                << send_to_port << std::endl;
      sender_ptr->connect();
      return true;
    } catch (const TCPException &e) {
      std::cerr << "TCP connect failed: " << e.what() << " (retry " << retry
                << ")" << std::endl;
      std::lock_guard<std::mutex> lg(sender_mutex);
      sender_ptr.reset();
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  return false;
}

void streamingThreadFunction() {
  std::cout << "Streaming thread started" << std::endl;

  CameraRequestData cfg;
  {
    std::lock_guard<std::mutex> lg(config_mutex);
    cfg = current_camera_config;
  }
  if (bitrate_override > 0) cfg.bitrate = bitrate_override;
  if (cfg.bitrate <= 0) cfg.bitrate = 4 * 1024 * 1024;
  if (cfg.fps <= 0) cfg.fps = 60;

  // Map width/height to ZED resolution.
  sl::Camera zed;
  sl::InitParameters init;
  init.depth_mode = sl::DEPTH_MODE::NONE;
  init.camera_fps = cfg.fps;
  if ((cfg.width == 3840 || cfg.width == 1920) && cfg.height == 1080) {
    init.camera_resolution = sl::RESOLUTION::HD1080;
  } else if ((cfg.width == 4416 || cfg.width == 2208) && cfg.height == 1242) {
    init.camera_resolution = sl::RESOLUTION::HD2K;
  } else {
    init.camera_resolution = sl::RESOLUTION::HD720;
  }
  if (zed.open(init) != sl::ERROR_CODE::SUCCESS) {
    std::cerr << "ZED open failed" << std::endl;
    return;
  }

  sl::VIEW view = zed_view_mode == "left" ? sl::VIEW::LEFT
                                            : sl::VIEW::SIDE_BY_SIDE;
  std::cout << "ZED view: " << (view == sl::VIEW::LEFT ? "LEFT" : "SIDE_BY_SIDE")
            << std::endl;

  // Capture one frame to learn effective dimensions (SBS doubles width).
  sl::Mat zed_image;
  uint32_t width = 0, height = 0;
  for (int attempt = 0; attempt < 120 && streaming_active.load() &&
                          !stop_requested.load();
       ++attempt) {
    if (zed.grab() == sl::ERROR_CODE::SUCCESS) {
      if (zed.retrieveImage(zed_image, view, sl::MEM::CPU) ==
          sl::ERROR_CODE::SUCCESS) {
        width = zed_image.getWidth();
        height = zed_image.getHeight();
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (width == 0 || height == 0) {
    std::cerr << "Failed to retrieve initial frame" << std::endl;
    zed.close();
    return;
  }
  std::cout << "Effective stream: " << width << "x" << height << "@" << cfg.fps
            << " bitrate=" << cfg.bitrate << std::endl;

  // Allocate NvBufSurfaces.
  NvBufSurface *bgra = nullptr;
  NvBufSurface *nv12 = nullptr;
  if (allocateBgraSurface(&bgra, width, height) != 0 ||
      allocateNv12Surface(&nv12, width, height) != 0) {
    std::cerr << "NvBufSurface alloc failed" << std::endl;
    zed.close();
    return;
  }

  // VIC transform params (full-frame, no crop).
  NvBufSurfTransformRect src_rect = {0, 0, width, height};
  NvBufSurfTransformRect dst_rect = {0, 0, width, height};
  NvBufSurfTransformParams tparams;
  std::memset(&tparams, 0, sizeof(tparams));
  tparams.transform_flag = static_cast<NvBufSurfTransform_Transform_Flag>(
      NVBUFSURF_TRANSFORM_FILTER | NVBUFSURF_TRANSFORM_FLIP);
  tparams.transform_flip = NvBufSurfTransform_None;
  tparams.transform_filter = NvBufSurfTransformInter_Nearest;
  tparams.src_rect = &src_rect;
  tparams.dst_rect = &dst_rect;

  // Set up encoder.
  enc_state.got_error.store(false);
  enc_state.got_eos.store(false);
  enc_state.enc = NvVideoEncoder::createVideoEncoder("enc0");
  if (!enc_state.enc) {
    std::cerr << "createVideoEncoder failed" << std::endl;
    NvBufSurfaceDestroy(bgra);
    NvBufSurfaceDestroy(nv12);
    zed.close();
    return;
  }
  uint32_t pixfmt =
      cfg.enableMvHevc ? V4L2_PIX_FMT_H265 : V4L2_PIX_FMT_H264;
  if (enc_state.enc->setCapturePlaneFormat(pixfmt, width, height,
                                           2 * 1024 * 1024) < 0 ||
      enc_state.enc->setOutputPlaneFormat(V4L2_PIX_FMT_NV12M, width, height) <
          0 ||
      enc_state.enc->setBitrate(cfg.bitrate) < 0 ||
      enc_state.enc->setProfile(
          cfg.enableMvHevc ? V4L2_MPEG_VIDEO_H265_PROFILE_MAIN
                            : V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) < 0 ||
      (cfg.enableMvHevc ? 0
                         : enc_state.enc->setLevel(
                               V4L2_MPEG_VIDEO_H264_LEVEL_5_0)) < 0 ||
      enc_state.enc->setFrameRate(cfg.fps, 1) < 0 ||
      enc_state.enc->setRateControlMode(V4L2_MPEG_VIDEO_BITRATE_MODE_CBR) <
          0 ||
      enc_state.enc->setIDRInterval(idr_interval) < 0 ||
      enc_state.enc->setIFrameInterval(idr_interval) < 0 ||
      enc_state.enc->setNumBFrames(0) < 0 ||
      enc_state.enc->setNumReferenceFrames(1) < 0 ||
      enc_state.enc->setInsertSpsPpsAtIdrEnabled(true) < 0 ||
      enc_state.enc->setInsertVuiEnabled(true) < 0 ||
      enc_state.enc->setMaxPerfMode(1) < 0) {
    std::cerr << "Encoder configuration failed" << std::endl;
    delete enc_state.enc;
    enc_state.enc = nullptr;
    NvBufSurfaceDestroy(bgra);
    NvBufSurfaceDestroy(nv12);
    zed.close();
    return;
  }
  if (enc_state.enc->output_plane.setupPlane(V4L2_MEMORY_MMAP, 6, true,
                                             false) < 0 ||
      enc_state.enc->capture_plane.setupPlane(V4L2_MEMORY_MMAP, 4, true,
                                              false) < 0 ||
      enc_state.enc->output_plane.setStreamStatus(true) < 0 ||
      enc_state.enc->capture_plane.setStreamStatus(true) < 0) {
    std::cerr << "Encoder plane setup/streamon failed" << std::endl;
    delete enc_state.enc;
    enc_state.enc = nullptr;
    NvBufSurfaceDestroy(bgra);
    NvBufSurfaceDestroy(nv12);
    zed.close();
    return;
  }
  enc_state.enc->capture_plane.setDQThreadCallback(capturePlaneDqCallback);
  enc_state.enc->capture_plane.startDQThread(&enc_state);
  for (uint32_t i = 0; i < enc_state.enc->capture_plane.getNumBuffers(); ++i) {
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];
    std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    std::memset(planes, 0, sizeof(planes));
    v4l2_buf.index = i;
    v4l2_buf.m.planes = planes;
    if (enc_state.enc->capture_plane.qBuffer(v4l2_buf, nullptr) < 0) {
      std::cerr << "Initial capture qBuffer failed" << std::endl;
    }
  }
  std::cout << "Encoder ready" << std::endl;

  // Force an IDR up front so PICO can decode immediately, regardless of when
  // it joins relative to the natural IDR cadence.
  if (enc_state.enc->forceIDR() < 0) {
    std::cerr << "forceIDR (initial) failed" << std::endl;
  }

  // Main streaming loop.
  send_enabled.store(true);
  uint64_t frame_id = 0;
  while (streaming_active.load() && !stop_requested.load() &&
         !enc_state.got_error.load()) {
    SteadyClock::time_point t0 = SteadyClock::now();
    if (zed.grab() != sl::ERROR_CODE::SUCCESS) continue;
    SteadyClock::time_point t1 = SteadyClock::now();
    if (zed.retrieveImage(zed_image, view, sl::MEM::CPU) !=
        sl::ERROR_CODE::SUCCESS)
      continue;
    SteadyClock::time_point t2 = SteadyClock::now();
    const uint8_t *src = zed_image.getPtr<sl::uchar1>(sl::MEM::CPU);
    if (!src || zed_image.getWidth() != width ||
        zed_image.getHeight() != height) {
      continue;
    }
    if (copyBgraIntoSurface(bgra, src,
                             static_cast<uint32_t>(zed_image.getStepBytes(
                                 sl::MEM::CPU)),
                             width, height) != 0) {
      break;
    }
    SteadyClock::time_point t3 = SteadyClock::now();
    if (NvBufSurfTransform(bgra, nv12, &tparams) !=
        NvBufSurfTransformError_Success) {
      std::cerr << "VIC failed" << std::endl;
      break;
    }
    SteadyClock::time_point t4 = SteadyClock::now();

    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];
    std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    std::memset(planes, 0, sizeof(planes));
    v4l2_buf.m.planes = planes;
    NvBuffer *enc_buffer = nullptr;
    if (frame_id < enc_state.enc->output_plane.getNumBuffers()) {
      enc_buffer = enc_state.enc->output_plane.getNthBuffer(frame_id);
      v4l2_buf.index = static_cast<uint32_t>(frame_id);
    } else {
      if (enc_state.enc->output_plane.dqBuffer(v4l2_buf, &enc_buffer, nullptr,
                                                -1) < 0) {
        std::cerr << "Output plane dqBuffer failed" << std::endl;
        break;
      }
    }
    uint32_t y_b = 0, uv_b = 0;
    if (copyNv12IntoEncoderBuffer(nv12, enc_buffer, &y_b, &uv_b) != 0) {
      std::cerr << "Copy NV12 into encoder failed" << std::endl;
      break;
    }
    v4l2_buf.m.planes[0].bytesused = y_b;
    v4l2_buf.m.planes[1].bytesused = uv_b;
    if (enc_state.enc->output_plane.qBuffer(v4l2_buf, nullptr) < 0) {
      std::cerr << "Output plane qBuffer failed" << std::endl;
      break;
    }
    SteadyClock::time_point t5 = SteadyClock::now();
    recordGrabFrame(elapsedMs(t0, t1), elapsedMs(t1, t2), elapsedMs(t2, t3),
                    elapsedMs(t3, t4), elapsedMs(t4, t5));
    frame_id++;
  }
  send_enabled.store(false);

  // Send EOS.
  {
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];
    std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    std::memset(planes, 0, sizeof(planes));
    v4l2_buf.m.planes = planes;
    NvBuffer *enc_buffer = nullptr;
    if (enc_state.enc->output_plane.dqBuffer(v4l2_buf, &enc_buffer, nullptr,
                                              1000) >= 0) {
      v4l2_buf.m.planes[0].bytesused = 0;
      v4l2_buf.m.planes[1].bytesused = 0;
      enc_state.enc->output_plane.qBuffer(v4l2_buf, nullptr);
    }
  }
  enc_state.enc->capture_plane.waitForDQThread(2000);

  std::cout << "Streaming loop exited; tearing down encoder/ZED" << std::endl;
  delete enc_state.enc;
  enc_state.enc = nullptr;
  NvBufSurfaceDestroy(bgra);
  NvBufSurfaceDestroy(nv12);
  zed.close();
  std::cout << "Streaming thread finished" << std::endl;
}

void startStreamingThread() {
  std::lock_guard<std::mutex> lg(streaming_mutex);
  if (streaming_thread && streaming_thread->joinable()) return;
  streaming_active.store(true);
  streaming_thread.reset(new std::thread(streamingThreadFunction));
}

void stopStreamingThread() {
  std::lock_guard<std::mutex> lg(streaming_mutex);
  streaming_active.store(false);
  send_enabled.store(false);
  {
    std::lock_guard<std::mutex> sg(sender_mutex);
    if (sender_ptr && sender_ptr->isConnected()) sender_ptr->disconnect();
    sender_ptr.reset();
  }
  if (streaming_thread && streaming_thread->joinable()) {
    streaming_thread->join();
    streaming_thread.reset();
  }
}

// ---------------------------------------------------------------------------
// PICO protocol handlers (mirrors orinvideosender2).
// ---------------------------------------------------------------------------
void handleOpenCamera(const std::vector<uint8_t> &data) {
  try {
    CameraRequestData cfg = deserializeCameraRequest(data);
    std::cout << "OPEN_CAMERA " << cfg.width << "x" << cfg.height << "@"
              << cfg.fps << " bitrate=" << cfg.bitrate << " ip=" << cfg.ip
              << " port=" << cfg.port << " camera=" << cfg.camera << std::endl;
    if (!isSupportedZedCameraName(cfg.camera)) {
      std::cerr << "Unsupported camera: " << cfg.camera << std::endl;
      return;
    }
    {
      std::lock_guard<std::mutex> lg(config_mutex);
      current_camera_config = cfg;
    }
    send_to_server = cfg.ip;
    send_to_port = cfg.port;

    if (!initializeSender()) {
      std::cerr << "Could not connect back to PICO; aborting OPEN_CAMERA"
                << std::endl;
      return;
    }
    startStreamingThread();
  } catch (const std::exception &e) {
    std::cerr << "OPEN_CAMERA parse failed: " << e.what() << std::endl;
  }
}

void handleCloseCamera(const std::vector<uint8_t> & /*data*/) {
  std::cout << "CLOSE_CAMERA" << std::endl;
  stopStreamingThread();
}

void onDataCallback(const std::string &command) {
  std::vector<uint8_t> bin(command.begin(), command.end());
  if (bin.size() < 4) return;
  uint32_t bodyLen = (static_cast<uint32_t>(bin[0]) << 24) |
                     (static_cast<uint32_t>(bin[1]) << 16) |
                     (static_cast<uint32_t>(bin[2]) << 8) |
                     static_cast<uint32_t>(bin[3]);
  if (4 + bodyLen > bin.size()) return;
  std::vector<uint8_t> body(bin.begin() + 4, bin.begin() + 4 + bodyLen);
  try {
    NetworkDataProtocol pkt = deserializeNetworkData(body);
    if (pkt.command == "OPEN_CAMERA") {
      handleOpenCamera(pkt.data);
    } else if (pkt.command == "CLOSE_CAMERA") {
      handleCloseCamera(pkt.data);
    } else {
      std::cout << "Unknown protocol command: " << pkt.command << std::endl;
    }
  } catch (const std::exception &e) {
    std::cerr << "Protocol parse failed: " << e.what() << std::endl;
  }
}

void onDisconnectCallback() {
  std::cout << "PICO disconnected; stopping streaming" << std::endl;
  stopStreamingThread();
}

void listenThreadFunction(const std::string &addr) {
  std::cout << "Listen thread on " << addr << std::endl;
  while (!stop_requested.load()) {
    try {
      server_ptr.reset(new TCPServer(addr));
      server_ptr->setDataCallback(onDataCallback);
      server_ptr->setDisconnectCallback(onDisconnectCallback);
      server_ptr->start();
      std::cout << "TCPServer listening on " << addr << std::endl;
      while (!stop_requested.load() && server_ptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (server_ptr) {
        server_ptr->stop();
        server_ptr.reset();
      }
    } catch (const std::exception &e) {
      std::cerr << "Listen thread error: " << e.what() << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }
}

void handleSigint(int) {
  std::cout << "\nSIGINT" << std::endl;
  stop_requested.store(true);
  stopStreamingThread();
  if (server_ptr) {
    server_ptr->stop();
    server_ptr.reset();
  }
}

}  // namespace

int main(int argc, char *argv[]) {
  std::signal(SIGINT, handleSigint);

  bool listen_enabled = false;
  std::string listen_address;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--listen" && i + 1 < argc) {
      listen_enabled = true;
      listen_address = argv[++i];
    } else if (a == "--view" && i + 1 < argc) {
      std::string v = toLowerCopy(argv[++i]);
      if (v == "sbs" || v == "side-by-side" || v == "side_by_side")
        zed_view_mode = "sbs";
      else if (v == "left" || v == "mono")
        zed_view_mode = "left";
      else {
        std::cerr << "Invalid --view: " << v << std::endl;
        return -1;
      }
    } else if (a == "--idr-interval" && i + 1 < argc) {
      idr_interval = std::max(1, std::atoi(argv[++i]));
    } else if (a == "--stats-interval" && i + 1 < argc) {
      stats_interval = std::max(0, std::atoi(argv[++i]));
    } else if (a == "--bitrate" && i + 1 < argc) {
      bitrate_override = std::atoi(argv[++i]);
    } else if (a == "--dump-h264" && i + 1 < argc) {
      dump_h264_path = argv[++i];
    } else if (a == "--help") {
      std::cout
          << "Usage: " << argv[0] << " [options]\n"
          << "  --listen IP:PORT      Accept PICO control connection\n"
          << "  --view sbs|left       ZED view (default: sbs)\n"
          << "  --idr-interval N      IDR/I-frame interval (default: 15)\n"
          << "  --stats-interval N    Log every N frames (default: 120)\n"
          << "  --bitrate BPS         Override H.264 bitrate (default: from PICO)\n"
          << "  --dump-h264 PATH      Also write the H.264 byte stream to PATH for debugging\n"
          << "  --help                This text\n"
          << "\n"
          << "Wire-protocol compatible with orinvideosender2; internal encode is\n"
          << "the Jetson Multimedia API (NvBufSurface + VIC + NvVideoEncoder) -\n"
          << "no GStreamer, no OpenCV, no ZMQ.\n";
      return 0;
    }
  }

  if (!listen_enabled) {
    std::cerr << "Error: --listen IP:PORT required (use --help)" << std::endl;
    return -1;
  }

  if (!dump_h264_path.empty()) {
    dump_h264_stream.open(dump_h264_path, std::ios::binary);
    if (!dump_h264_stream) {
      std::cerr << "Warning: could not open dump file " << dump_h264_path
                << std::endl;
    } else {
      std::cout << "Dumping H.264 stream to " << dump_h264_path << std::endl;
    }
  }

  listen_thread.reset(
      new std::thread(listenThreadFunction, listen_address));
  std::cout << "orinvideosender3 ready. Ctrl+C to stop." << std::endl;
  while (!stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (listen_thread && listen_thread->joinable()) listen_thread->join();
  std::cout << "Exit." << std::endl;
  return 0;
}
