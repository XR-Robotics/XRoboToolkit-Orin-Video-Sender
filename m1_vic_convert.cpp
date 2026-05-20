// Plan A milestone 1: prove we can drive the VIC hardware directly via the
// Jetson Multimedia API to color-convert a BGRA NvBufSurface into NV12.
//
// This is the same hardware path that the GStreamer `nvvidconv` element uses
// internally, but called directly so we don't have to satisfy GStreamer's
// caps negotiation, buffer pool, or NvBufSurface metadata rules. Once this
// works we have the building block that previously broke main_zed_zero_copy.cpp.
//
// Build (on the Orin):
//   make m1_vic_convert
//
// Run:
//   ./m1_vic_convert
//
// Verify on the laptop (or anywhere with ffplay):
//   scp unitree@192.168.123.164:/tmp/m1_out.nv12 ~/Downloads/
//   ffplay -f rawvideo -pixel_format nv12 -video_size 2560x720 ~/Downloads/m1_out.nv12
//
// Expected: a static frame with a horizontal blue ramp + vertical green ramp +
// solid red, indicating the VIC correctly consumed our BGRA test pattern and
// produced a valid NV12 frame.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include <nvbufsurface.h>
#include <nvbufsurftransform.h>

namespace {

constexpr uint32_t kWidth = 2560;
constexpr uint32_t kHeight = 720;
const char *kOutPath = "/tmp/m1_out.nv12";

int allocateSurface(NvBufSurface **out, NvBufSurfaceColorFormat color,
                    const char *tag) {
  NvBufSurfaceCreateParams params;
  std::memset(&params, 0, sizeof(params));
  params.gpuId = 0;
  params.width = kWidth;
  params.height = kHeight;
  params.size = 0;            // auto
  params.isContiguous = true; // VIC prefers contiguous
  params.colorFormat = color;
  params.layout = NVBUF_LAYOUT_PITCH;
  // SURFACE_ARRAY is the Tegra-native memory type that VIC, NVENC, and the
  // 3D engine all consume. It is mappable to CPU via NvBufSurfaceMap (with
  // a sync cost) but the hot path stays device-side.
  params.memType = NVBUF_MEM_SURFACE_ARRAY;

  int ret = NvBufSurfaceCreate(out, 1, &params);
  if (ret != 0 || *out == nullptr) {
    std::cerr << "[" << tag << "] NvBufSurfaceCreate failed (" << ret << ")"
              << std::endl;
    return -1;
  }

  NvBufSurfaceParams &surf = (*out)->surfaceList[0];
  std::cout << "[" << tag << "] alloc OK " << surf.width << "x" << surf.height
            << " planes=" << surf.planeParams.num_planes;
  for (uint32_t pl = 0; pl < surf.planeParams.num_planes; ++pl) {
    std::cout << " p" << pl << "(" << surf.planeParams.width[pl] << "x"
              << surf.planeParams.height[pl]
              << ", pitch=" << surf.planeParams.pitch[pl]
              << ", bpp=" << surf.planeParams.bytesPerPix[pl] << ")";
  }
  std::cout << std::endl;
  return 0;
}

int fillBgraGradient(NvBufSurface *surf) {
  if (NvBufSurfaceMap(surf, 0, 0, NVBUF_MAP_WRITE) != 0) {
    std::cerr << "NvBufSurfaceMap(BGRA, plane=0, WRITE) failed" << std::endl;
    return -1;
  }

  NvBufSurfaceParams &p = surf->surfaceList[0];
  uint8_t *base = static_cast<uint8_t *>(p.mappedAddr.addr[0]);
  if (!base) {
    std::cerr << "BGRA mappedAddr is null" << std::endl;
    NvBufSurfaceUnMap(surf, 0, 0);
    return -1;
  }

  const uint32_t pitch = p.planeParams.pitch[0];
  const uint32_t w = p.planeParams.width[0];
  const uint32_t h = p.planeParams.height[0];
  for (uint32_t y = 0; y < h; ++y) {
    uint8_t *row = base + static_cast<size_t>(y) * pitch;
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t *px = row + x * 4;
      // BGRA / BGRx: B = horizontal ramp, G = vertical ramp, R = solid 255,
      // alpha/padding = 0.
      px[0] = static_cast<uint8_t>(x * 255 / w);
      px[1] = static_cast<uint8_t>(y * 255 / h);
      px[2] = 255;
      px[3] = 0;
    }
  }

  NvBufSurfaceSyncForDevice(surf, 0, 0);
  NvBufSurfaceUnMap(surf, 0, 0);
  return 0;
}

int dumpPlanesToFile(NvBufSurface *surf, const char *path) {
  if (NvBufSurfaceMap(surf, 0, -1 /* all planes */, NVBUF_MAP_READ) != 0) {
    std::cerr << "NvBufSurfaceMap(NV12, all planes, READ) failed" << std::endl;
    return -1;
  }
  NvBufSurfaceSyncForCpu(surf, 0, -1);

  NvBufSurfaceParams &p = surf->surfaceList[0];
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::cerr << "Cannot open " << path << " for writing" << std::endl;
    NvBufSurfaceUnMap(surf, 0, -1);
    return -1;
  }

  for (uint32_t pl = 0; pl < p.planeParams.num_planes; ++pl) {
    const uint8_t *base = static_cast<uint8_t *>(p.mappedAddr.addr[pl]);
    if (!base) {
      std::cerr << "Plane " << pl << " mappedAddr is null" << std::endl;
      NvBufSurfaceUnMap(surf, 0, -1);
      return -1;
    }
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

  out.close();
  NvBufSurfaceUnMap(surf, 0, -1);
  return 0;
}

}  // namespace

int main() {
  NvBufSurface *bgra = nullptr;
  NvBufSurface *nv12 = nullptr;

  if (allocateSurface(&bgra, NVBUF_COLOR_FORMAT_BGRA, "BGRA") != 0) {
    return 1;
  }
  if (allocateSurface(&nv12, NVBUF_COLOR_FORMAT_NV12, "NV12") != 0) {
    NvBufSurfaceDestroy(bgra);
    return 1;
  }

  if (fillBgraGradient(bgra) != 0) {
    NvBufSurfaceDestroy(bgra);
    NvBufSurfaceDestroy(nv12);
    return 1;
  }

  NvBufSurfTransformParams tparams;
  std::memset(&tparams, 0, sizeof(tparams));
  tparams.transform_flag = NVBUFSURF_TRANSFORM_FILTER;
  tparams.transform_filter = NvBufSurfTransformInter_Nearest;

  NvBufSurfTransform_Error err = NvBufSurfTransform(bgra, nv12, &tparams);
  if (err != NvBufSurfTransformError_Success) {
    std::cerr << "NvBufSurfTransform failed (" << err << ")" << std::endl;
    NvBufSurfaceDestroy(bgra);
    NvBufSurfaceDestroy(nv12);
    return 1;
  }
  std::cout << "VIC BGRA->NV12 transform OK" << std::endl;

  if (dumpPlanesToFile(nv12, kOutPath) != 0) {
    NvBufSurfaceDestroy(bgra);
    NvBufSurfaceDestroy(nv12);
    return 1;
  }
  std::cout << "Wrote " << kOutPath << " (" << kWidth << "x" << kHeight
            << " NV12)" << std::endl;

  NvBufSurfaceDestroy(bgra);
  NvBufSurfaceDestroy(nv12);
  return 0;
}
