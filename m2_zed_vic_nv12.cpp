// Plan A milestone 2: ZED SDK CPU retrieve -> NvBufSurface (BGRA) -> VIC ->
// NvBufSurface (NV12) -> raw file. No GStreamer.
//
// Validates that the full per-frame VIC pipeline survives at 60 fps with real
// ZED data, before we add NVENC (M3). The CPU retrieve here is deliberate:
// for the ZED Mini (USB), the SDK lands the frame in host memory anyway, so
// CPU retrieve is one copy less than sl::MEM::GPU. We will optimize to GPU
// retrieve + cudaMemcpy2D via EGL interop after the full pipeline works.
//
// Build (on the Orin):
//   make m2_zed_vic_nv12
//
// Run (writes 60 frames):
//   ./m2_zed_vic_nv12
//
// Verify on the laptop:
//   scp unitree@<orin>:/tmp/m2_out.nv12 ~/Downloads/
//   ffplay -f rawvideo -pixel_format nv12 -video_size 2560x720 -framerate 60 \
//     ~/Downloads/m2_out.nv12

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include <nvbufsurface.h>
#include <nvbufsurftransform.h>
#include <sl/Camera.hpp>

namespace {

constexpr uint32_t kWidth = 2560;
constexpr uint32_t kHeight = 720;
constexpr int kFrames = 60;
const char *kOutPath = "/tmp/m2_out.nv12";

using SteadyClock = std::chrono::steady_clock;

double elapsedMs(const SteadyClock::time_point &start,
                 const SteadyClock::time_point &end) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
             end - start)
      .count();
}

int allocateSurface(NvBufSurface **out, NvBufSurfaceColorFormat color,
                    const char *tag) {
  NvBufSurfaceAllocateParams params;
  std::memset(&params, 0, sizeof(params));
  params.params.width = kWidth;
  params.params.height = kHeight;
  params.params.colorFormat = color;
  params.params.layout = NVBUF_LAYOUT_PITCH;
  params.params.memType = NVBUF_MEM_SURFACE_ARRAY;
  params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;

  int ret = NvBufSurfaceAllocate(out, 1, &params);
  if (ret != 0 || *out == nullptr) {
    std::cerr << "[" << tag << "] NvBufSurfaceAllocate failed (" << ret << ")"
              << std::endl;
    return -1;
  }
  (*out)->numFilled = 1;
  return 0;
}

// Copy a BGRA frame from the ZED's CPU buffer into the NvBufSurface BGRA
// plane, row-by-row to handle pitch differences. ZED's getStepBytes() gives
// us its own pitch.
int copyBgraIntoSurface(NvBufSurface *surf, const uint8_t *src,
                        uint32_t src_pitch) {
  if (NvBufSurfaceMap(surf, 0, 0, NVBUF_MAP_READ_WRITE) != 0) {
    std::cerr << "NvBufSurfaceMap(BGRA, plane=0) failed" << std::endl;
    return -1;
  }
  NvBufSurfaceSyncForCpu(surf, 0, 0);

  NvBufSurfaceParams &p = surf->surfaceList[0];
  uint8_t *dst = static_cast<uint8_t *>(p.mappedAddr.addr[0]);
  if (!dst) {
    std::cerr << "BGRA mappedAddr is null" << std::endl;
    NvBufSurfaceUnMap(surf, 0, 0);
    return -1;
  }
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

int appendNv12ToStream(NvBufSurface *surf, std::ofstream &out) {
  if (NvBufSurfaceMap(surf, 0, -1, NVBUF_MAP_READ_WRITE) != 0) {
    std::cerr << "NvBufSurfaceMap(NV12, all planes) failed" << std::endl;
    return -1;
  }
  NvBufSurfaceSyncForCpu(surf, 0, -1);

  NvBufSurfaceParams &p = surf->surfaceList[0];
  for (uint32_t pl = 0; pl < p.planeParams.num_planes; ++pl) {
    const uint8_t *base = static_cast<uint8_t *>(p.mappedAddr.addr[pl]);
    const uint32_t pitch = p.planeParams.pitch[pl];
    const uint32_t bytes_per_row =
        p.planeParams.width[pl] * p.planeParams.bytesPerPix[pl];
    const uint32_t h = p.planeParams.height[pl];
    for (uint32_t y = 0; y < h; ++y) {
      out.write(reinterpret_cast<const char *>(base + static_cast<size_t>(y) *
                                                          pitch),
                bytes_per_row);
    }
  }

  NvBufSurfaceUnMap(surf, 0, -1);
  return 0;
}

}  // namespace

int main() {
  // 1) Open ZED HD720 @ 60fps.
  sl::Camera zed;
  sl::InitParameters init;
  init.camera_resolution = sl::RESOLUTION::HD720;
  init.camera_fps = 60;
  init.depth_mode = sl::DEPTH_MODE::NONE;  // we only want color
  if (zed.open(init) != sl::ERROR_CODE::SUCCESS) {
    std::cerr << "ZED open failed" << std::endl;
    return 1;
  }
  std::cout << "ZED opened at "
            << zed.getCameraInformation().camera_configuration.resolution.width
            << "x"
            << zed.getCameraInformation().camera_configuration.resolution.height
            << " @ "
            << zed.getCameraInformation().camera_configuration.fps << "fps"
            << std::endl;

  // 2) Allocate input + output NvBufSurfaces.
  NvBufSurface *bgra = nullptr;
  NvBufSurface *nv12 = nullptr;
  if (allocateSurface(&bgra, NVBUF_COLOR_FORMAT_BGRA, "BGRA") != 0) {
    zed.close();
    return 1;
  }
  if (allocateSurface(&nv12, NVBUF_COLOR_FORMAT_NV12, "NV12") != 0) {
    NvBufSurfaceDestroy(bgra);
    zed.close();
    return 1;
  }

  // 3) Transform params (no crop / no scale, BGRA -> NV12 only).
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

  std::ofstream out(kOutPath, std::ios::binary);
  if (!out) {
    std::cerr << "Cannot open " << kOutPath << std::endl;
    NvBufSurfaceDestroy(bgra);
    NvBufSurfaceDestroy(nv12);
    zed.close();
    return 1;
  }

  // 4) Per-frame timing buckets so we know where the time goes.
  double grab_ms_total = 0, retrieve_ms_total = 0, copy_ms_total = 0,
         vic_ms_total = 0, dump_ms_total = 0;
  int produced = 0;
  sl::Mat zed_image;

  SteadyClock::time_point t_loop_start = SteadyClock::now();
  for (int f = 0; f < kFrames; ++f) {
    SteadyClock::time_point t0 = SteadyClock::now();
    if (zed.grab() != sl::ERROR_CODE::SUCCESS) {
      std::cerr << "grab() failed at frame " << f << std::endl;
      continue;
    }
    SteadyClock::time_point t1 = SteadyClock::now();
    grab_ms_total += elapsedMs(t0, t1);

    if (zed.retrieveImage(zed_image, sl::VIEW::SIDE_BY_SIDE, sl::MEM::CPU) !=
        sl::ERROR_CODE::SUCCESS) {
      std::cerr << "retrieveImage failed at frame " << f << std::endl;
      continue;
    }
    SteadyClock::time_point t2 = SteadyClock::now();
    retrieve_ms_total += elapsedMs(t1, t2);

    const uint8_t *src = zed_image.getPtr<sl::uchar1>(sl::MEM::CPU);
    uint32_t src_pitch =
        static_cast<uint32_t>(zed_image.getStepBytes(sl::MEM::CPU));
    if (!src || zed_image.getWidth() != kWidth ||
        zed_image.getHeight() != kHeight) {
      std::cerr << "Unexpected ZED frame: " << zed_image.getWidth() << "x"
                << zed_image.getHeight() << " ptr=" << static_cast<const void *>(src)
                << std::endl;
      continue;
    }
    if (copyBgraIntoSurface(bgra, src, src_pitch) != 0) {
      break;
    }
    SteadyClock::time_point t3 = SteadyClock::now();
    copy_ms_total += elapsedMs(t2, t3);

    NvBufSurfTransform_Error err = NvBufSurfTransform(bgra, nv12, &tparams);
    if (err != NvBufSurfTransformError_Success) {
      std::cerr << "NvBufSurfTransform failed at frame " << f << " (" << err
                << ")" << std::endl;
      break;
    }
    SteadyClock::time_point t4 = SteadyClock::now();
    vic_ms_total += elapsedMs(t3, t4);

    if (appendNv12ToStream(nv12, out) != 0) {
      break;
    }
    SteadyClock::time_point t5 = SteadyClock::now();
    dump_ms_total += elapsedMs(t4, t5);
    produced++;
  }
  SteadyClock::time_point t_loop_end = SteadyClock::now();
  out.close();

  double loop_ms = elapsedMs(t_loop_start, t_loop_end);
  std::cout << "Produced " << produced << " frames in " << loop_ms
            << "ms (" << (produced > 0 ? produced * 1000.0 / loop_ms : 0)
            << " fps)" << std::endl;
  if (produced > 0) {
    std::cout << "Per-frame averages:"
              << " grab=" << grab_ms_total / produced << "ms"
              << " retrieve=" << retrieve_ms_total / produced << "ms"
              << " copy=" << copy_ms_total / produced << "ms"
              << " vic=" << vic_ms_total / produced << "ms"
              << " dump=" << dump_ms_total / produced << "ms" << std::endl;
  }
  std::cout << "Wrote " << kOutPath << std::endl;

  NvBufSurfaceDestroy(bgra);
  NvBufSurfaceDestroy(nv12);
  zed.close();
  return 0;
}
