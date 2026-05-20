// Plan A milestone 3: ZED -> VIC NV12 -> NvVideoEncoder (MMAPI) -> H.264 file.
// No GStreamer, no nvv4l2h264enc plugin. Direct V4L2 M2M via the MMAPI
// NvVideoEncoder wrapper from /usr/src/jetson_multimedia_api/samples/common.
//
// Build (on the Orin):
//   make m3_zed_vic_nvenc
//
// Run (encodes 120 frames):
//   ./m3_zed_vic_nvenc
//
// Verify on the laptop:
//   scp unitree@<orin>:/tmp/m3_out.h264 ~/Downloads/
//   ffplay ~/Downloads/m3_out.h264
//
// Implementation matches the structure of samples/03_video_cuda_enc, minus
// the CUDA EGL rendering: encoder uses V4L2_MEMORY_MMAP buffers on both the
// output plane (we memcpy NV12 in) and the capture plane (we read H.264 out
// in a callback). Single ~3.7 MB CPU memcpy per frame from our VIC NvBufSurface
// into the encoder's mmap buffer; switching to V4L2_MEMORY_DMABUF (zero-copy)
// is a follow-up optimization.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <linux/videodev2.h>

#include <NvVideoEncoder.h>
#include <NvBuffer.h>
#include <nvbufsurface.h>
#include <nvbufsurftransform.h>
#include <sl/Camera.hpp>

namespace {

constexpr uint32_t kWidth = 2560;
constexpr uint32_t kHeight = 720;
constexpr uint32_t kFps = 60;
constexpr uint32_t kBitrate = 4 * 1024 * 1024;  // 4 Mbps
constexpr int kFramesToEncode = 120;
const char *kOutPath = "/tmp/m3_out.h264";

using SteadyClock = std::chrono::steady_clock;

double elapsedMs(const SteadyClock::time_point &start,
                 const SteadyClock::time_point &end) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
             end - start)
      .count();
}

struct EncoderCtx {
  NvVideoEncoder *enc = nullptr;
  std::ofstream out_file;
  std::atomic<bool> got_error{false};
  std::atomic<bool> got_eos{false};
  uint64_t encoded_bytes = 0;
  uint32_t encoded_frames = 0;
};

EncoderCtx g_ctx;  // simple singleton -- the dq-thread callback needs ctx

// Capture-plane dequeue callback. Fires on the encoder's internal dq-thread
// every time a new H.264 buffer comes out. Writes the NAL units to the output
// file and re-queues the buffer back to the encoder.
bool capture_plane_dq_callback(struct v4l2_buffer *v4l2_buf, NvBuffer *buffer,
                               NvBuffer *shared_buffer, void *arg) {
  (void)shared_buffer;
  EncoderCtx *ctx = static_cast<EncoderCtx *>(arg);
  if (!v4l2_buf) {
    std::cerr << "Capture plane dqBuffer returned null v4l2_buf" << std::endl;
    ctx->got_error.store(true);
    return false;
  }

  if (buffer->planes[0].bytesused > 0) {
    ctx->out_file.write(
        reinterpret_cast<const char *>(buffer->planes[0].data),
        buffer->planes[0].bytesused);
    ctx->encoded_bytes += buffer->planes[0].bytesused;
    ctx->encoded_frames++;
  }

  if (ctx->enc->capture_plane.qBuffer(*v4l2_buf, nullptr) < 0) {
    std::cerr << "Error re-queueing capture-plane buffer" << std::endl;
    ctx->got_error.store(true);
    return false;
  }

  // bytesused==0 marks EOS from the encoder.
  if (buffer->planes[0].bytesused == 0) {
    ctx->got_eos.store(true);
    return false;
  }
  return true;
}

int allocateBgraSurface(NvBufSurface **out) {
  NvBufSurfaceAllocateParams params;
  std::memset(&params, 0, sizeof(params));
  params.params.width = kWidth;
  params.params.height = kHeight;
  params.params.colorFormat = NVBUF_COLOR_FORMAT_BGRA;
  params.params.layout = NVBUF_LAYOUT_PITCH;
  params.params.memType = NVBUF_MEM_SURFACE_ARRAY;
  params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
  int ret = NvBufSurfaceAllocate(out, 1, &params);
  if (ret != 0 || *out == nullptr) {
    return -1;
  }
  (*out)->numFilled = 1;
  return 0;
}

int allocateNv12Surface(NvBufSurface **out) {
  NvBufSurfaceAllocateParams params;
  std::memset(&params, 0, sizeof(params));
  params.params.width = kWidth;
  params.params.height = kHeight;
  params.params.colorFormat = NVBUF_COLOR_FORMAT_NV12;
  params.params.layout = NVBUF_LAYOUT_PITCH;
  params.params.memType = NVBUF_MEM_SURFACE_ARRAY;
  params.memtag = NvBufSurfaceTag_VIDEO_ENC;
  int ret = NvBufSurfaceAllocate(out, 1, &params);
  if (ret != 0 || *out == nullptr) {
    return -1;
  }
  (*out)->numFilled = 1;
  return 0;
}

// Copy a BGRA frame from ZED's CPU buffer into the NvBufSurface BGRA plane.
int copyBgraIntoSurface(NvBufSurface *surf, const uint8_t *src,
                        uint32_t src_pitch) {
  if (NvBufSurfaceMap(surf, 0, 0, NVBUF_MAP_READ_WRITE) != 0) return -1;
  NvBufSurfaceSyncForCpu(surf, 0, 0);
  NvBufSurfaceParams &p = surf->surfaceList[0];
  uint8_t *dst = static_cast<uint8_t *>(p.mappedAddr.addr[0]);
  const uint32_t dst_pitch = p.planeParams.pitch[0];
  const uint32_t row_bytes = kWidth * 4;
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::memcpy(dst + static_cast<size_t>(y) * dst_pitch,
                src + static_cast<size_t>(y) * src_pitch, row_bytes);
  }
  NvBufSurfaceSyncForDevice(surf, 0, 0);
  NvBufSurfaceUnMap(surf, 0, 0);
  return 0;
}

// Copy NV12 from our NvBufSurface into the encoder's output-plane NvBuffer
// (V4L2_MEMORY_MMAP). One ~3.7 MB CPU copy per frame; trades latency for
// simpler code. Returns bytesused for each plane in *out_y_bytes/*out_uv_bytes.
int copyNv12IntoEncoderBuffer(NvBufSurface *src_surf, NvBuffer *dst_buffer,
                              uint32_t *out_y_bytes, uint32_t *out_uv_bytes) {
  if (NvBufSurfaceMap(src_surf, 0, -1, NVBUF_MAP_READ_WRITE) != 0) return -1;
  NvBufSurfaceSyncForCpu(src_surf, 0, -1);
  NvBufSurfaceParams &p = src_surf->surfaceList[0];

  // Plane 0: Y, plane 1: UV interleaved.
  for (uint32_t pl = 0; pl < p.planeParams.num_planes && pl < dst_buffer->n_planes;
       ++pl) {
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
    if (pl == 0 && out_y_bytes) *out_y_bytes = dp.bytesused;
    if (pl == 1 && out_uv_bytes) *out_uv_bytes = dp.bytesused;
  }

  NvBufSurfaceUnMap(src_surf, 0, -1);

  // The encoder buffer was touched by CPU; flush so the device sees it. The
  // sample uses NvBufSurfaceSyncForDevice via NvBufSurfaceFromFd on the
  // V4L2 buffer's fd.
  NvBufSurface *enc_surf = nullptr;
  if (NvBufSurfaceFromFd(dst_buffer->planes[0].fd, (void **)&enc_surf) == 0 &&
      enc_surf) {
    NvBufSurfaceSyncForDevice(enc_surf, -1, -1);
  }
  return 0;
}

}  // namespace

int main() {
  // ---------- ZED ----------
  sl::Camera zed;
  sl::InitParameters init;
  init.camera_resolution = sl::RESOLUTION::HD720;
  init.camera_fps = kFps;
  init.depth_mode = sl::DEPTH_MODE::NONE;
  if (zed.open(init) != sl::ERROR_CODE::SUCCESS) {
    std::cerr << "ZED open failed" << std::endl;
    return 1;
  }
  std::cout << "ZED opened HD720 SIDE_BY_SIDE @ " << kFps << "fps" << std::endl;

  // ---------- NvBufSurfaces (VIC stage) ----------
  NvBufSurface *bgra = nullptr;
  NvBufSurface *nv12 = nullptr;
  if (allocateBgraSurface(&bgra) != 0 || allocateNv12Surface(&nv12) != 0) {
    std::cerr << "NvBufSurfaceAllocate failed" << std::endl;
    zed.close();
    return 1;
  }

  NvBufSurfTransformRect src_rect = {0, 0, kWidth, kHeight};
  NvBufSurfTransformRect dst_rect = {0, 0, kWidth, kHeight};
  NvBufSurfTransformParams tparams;
  std::memset(&tparams, 0, sizeof(tparams));
  tparams.transform_flag = static_cast<NvBufSurfTransform_Transform_Flag>(
      NVBUFSURF_TRANSFORM_FILTER | NVBUFSURF_TRANSFORM_FLIP);
  tparams.transform_flip = NvBufSurfTransform_None;
  tparams.transform_filter = NvBufSurfTransformInter_Nearest;
  tparams.src_rect = &src_rect;
  tparams.dst_rect = &dst_rect;

  // ---------- Encoder ----------
  g_ctx.out_file.open(kOutPath, std::ios::binary);
  if (!g_ctx.out_file) {
    std::cerr << "Cannot open " << kOutPath << std::endl;
    NvBufSurfaceDestroy(bgra);
    NvBufSurfaceDestroy(nv12);
    zed.close();
    return 1;
  }

  g_ctx.enc = NvVideoEncoder::createVideoEncoder("enc0");
  if (!g_ctx.enc) {
    std::cerr << "createVideoEncoder failed" << std::endl;
    return 1;
  }

  // Capture plane format MUST be set before output plane.
  if (g_ctx.enc->setCapturePlaneFormat(V4L2_PIX_FMT_H264, kWidth, kHeight,
                                       2 * 1024 * 1024) < 0) {
    std::cerr << "setCapturePlaneFormat failed" << std::endl;
    return 1;
  }
  if (g_ctx.enc->setOutputPlaneFormat(V4L2_PIX_FMT_NV12M, kWidth, kHeight) <
      0) {
    std::cerr << "setOutputPlaneFormat failed" << std::endl;
    return 1;
  }

  // Low-latency H.264 baseline settings (match orinvideosender2 defaults).
  if (g_ctx.enc->setBitrate(kBitrate) < 0 ||
      g_ctx.enc->setProfile(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) < 0 ||
      g_ctx.enc->setLevel(V4L2_MPEG_VIDEO_H264_LEVEL_5_0) < 0 ||
      g_ctx.enc->setFrameRate(kFps, 1) < 0 ||
      g_ctx.enc->setRateControlMode(V4L2_MPEG_VIDEO_BITRATE_MODE_CBR) < 0 ||
      g_ctx.enc->setIDRInterval(15) < 0 ||
      g_ctx.enc->setIFrameInterval(15) < 0 ||
      g_ctx.enc->setNumBFrames(0) < 0 ||
      g_ctx.enc->setInsertSpsPpsAtIdrEnabled(true) < 0 ||
      g_ctx.enc->setMaxPerfMode(1) < 0) {
    std::cerr << "Encoder parameter setup failed" << std::endl;
    return 1;
  }

  if (g_ctx.enc->output_plane.setupPlane(V4L2_MEMORY_MMAP, 6, true, false) <
          0 ||
      g_ctx.enc->capture_plane.setupPlane(V4L2_MEMORY_MMAP, 4, true, false) <
          0) {
    std::cerr << "Plane setup failed" << std::endl;
    return 1;
  }

  if (g_ctx.enc->output_plane.setStreamStatus(true) < 0 ||
      g_ctx.enc->capture_plane.setStreamStatus(true) < 0) {
    std::cerr << "Stream-on failed" << std::endl;
    return 1;
  }

  g_ctx.enc->capture_plane.setDQThreadCallback(capture_plane_dq_callback);
  g_ctx.enc->capture_plane.startDQThread(&g_ctx);

  // Enqueue all empty capture-plane buffers.
  for (uint32_t i = 0; i < g_ctx.enc->capture_plane.getNumBuffers(); ++i) {
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];
    std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    std::memset(planes, 0, sizeof(planes));
    v4l2_buf.index = i;
    v4l2_buf.m.planes = planes;
    if (g_ctx.enc->capture_plane.qBuffer(v4l2_buf, nullptr) < 0) {
      std::cerr << "Initial capture qBuffer failed" << std::endl;
      return 1;
    }
  }

  std::cout << "Encoder ready: output_buffers="
            << g_ctx.enc->output_plane.getNumBuffers()
            << " capture_buffers=" << g_ctx.enc->capture_plane.getNumBuffers()
            << std::endl;

  // ---------- Main encode loop ----------
  sl::Mat zed_image;
  double grab_ms = 0, retrieve_ms = 0, copy_ms = 0, vic_ms = 0,
         enc_push_ms = 0;
  int produced = 0;
  SteadyClock::time_point t_loop = SteadyClock::now();

  for (int f = 0; f < kFramesToEncode && !g_ctx.got_error.load(); ++f) {
    SteadyClock::time_point t0 = SteadyClock::now();
    if (zed.grab() != sl::ERROR_CODE::SUCCESS) continue;
    SteadyClock::time_point t1 = SteadyClock::now();
    grab_ms += elapsedMs(t0, t1);

    if (zed.retrieveImage(zed_image, sl::VIEW::SIDE_BY_SIDE, sl::MEM::CPU) !=
        sl::ERROR_CODE::SUCCESS)
      continue;
    SteadyClock::time_point t2 = SteadyClock::now();
    retrieve_ms += elapsedMs(t1, t2);

    const uint8_t *src = zed_image.getPtr<sl::uchar1>(sl::MEM::CPU);
    if (!src || zed_image.getWidth() != kWidth ||
        zed_image.getHeight() != kHeight)
      continue;
    if (copyBgraIntoSurface(bgra, src,
                            static_cast<uint32_t>(zed_image.getStepBytes(
                                sl::MEM::CPU))) != 0)
      break;
    SteadyClock::time_point t3 = SteadyClock::now();
    copy_ms += elapsedMs(t2, t3);

    if (NvBufSurfTransform(bgra, nv12, &tparams) !=
        NvBufSurfTransformError_Success) {
      std::cerr << "VIC failed at frame " << f << std::endl;
      break;
    }
    SteadyClock::time_point t4 = SteadyClock::now();
    vic_ms += elapsedMs(t3, t4);

    // Push NV12 to encoder. For the first N frames (where N=output_buffers),
    // we use getNthBuffer; afterwards we dequeue a returned buffer first.
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];
    std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    std::memset(planes, 0, sizeof(planes));
    v4l2_buf.m.planes = planes;
    NvBuffer *enc_buffer = nullptr;
    if (static_cast<uint32_t>(f) <
        g_ctx.enc->output_plane.getNumBuffers()) {
      enc_buffer = g_ctx.enc->output_plane.getNthBuffer(f);
      v4l2_buf.index = f;
    } else {
      if (g_ctx.enc->output_plane.dqBuffer(v4l2_buf, &enc_buffer, nullptr,
                                            -1) < 0) {
        std::cerr << "Output plane dqBuffer failed at frame " << f
                  << std::endl;
        break;
      }
    }

    uint32_t y_bytes = 0, uv_bytes = 0;
    if (copyNv12IntoEncoderBuffer(nv12, enc_buffer, &y_bytes, &uv_bytes) !=
        0) {
      std::cerr << "Copy NV12 -> encoder buffer failed" << std::endl;
      break;
    }
    v4l2_buf.m.planes[0].bytesused = y_bytes;
    v4l2_buf.m.planes[1].bytesused = uv_bytes;

    if (g_ctx.enc->output_plane.qBuffer(v4l2_buf, nullptr) < 0) {
      std::cerr << "Output plane qBuffer failed at frame " << f << std::endl;
      break;
    }
    SteadyClock::time_point t5 = SteadyClock::now();
    enc_push_ms += elapsedMs(t4, t5);
    produced++;
  }

  // Send EOS: queue an empty buffer.
  {
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];
    std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    std::memset(planes, 0, sizeof(planes));
    v4l2_buf.m.planes = planes;
    NvBuffer *enc_buffer = nullptr;
    if (g_ctx.enc->output_plane.dqBuffer(v4l2_buf, &enc_buffer, nullptr, -1) >=
        0) {
      v4l2_buf.m.planes[0].bytesused = 0;
      v4l2_buf.m.planes[1].bytesused = 0;
      g_ctx.enc->output_plane.qBuffer(v4l2_buf, nullptr);
    }
  }

  g_ctx.enc->capture_plane.waitForDQThread(5000);
  double loop_ms = elapsedMs(t_loop, SteadyClock::now());

  std::cout << "Submitted " << produced << " frames" << std::endl;
  std::cout << "Encoded " << g_ctx.encoded_frames << " H.264 packets, "
            << g_ctx.encoded_bytes << " bytes (" << g_ctx.encoded_bytes * 8.0 /
                                                       (produced / 60.0 * 1e6)
            << " Mbps)" << std::endl;
  if (produced > 0) {
    std::cout << "Per-frame averages:"
              << " grab=" << grab_ms / produced << "ms"
              << " retrieve=" << retrieve_ms / produced << "ms"
              << " bgra_copy=" << copy_ms / produced << "ms"
              << " vic=" << vic_ms / produced << "ms"
              << " enc_push=" << enc_push_ms / produced << "ms" << std::endl;
  }
  std::cout << "Loop wall=" << loop_ms
            << "ms => " << (produced * 1000.0 / loop_ms) << " fps" << std::endl;
  std::cout << "Wrote " << kOutPath << std::endl;

  delete g_ctx.enc;
  g_ctx.out_file.close();
  NvBufSurfaceDestroy(bgra);
  NvBufSurfaceDestroy(nv12);
  zed.close();
  return 0;
}
