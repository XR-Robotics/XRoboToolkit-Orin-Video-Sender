// Plan A milestone 1: prove we can drive the VIC hardware directly via the
// Jetson Multimedia API to color-convert a BGRA NvBufSurface into NV12.
//
// Uses the canonical pattern from /usr/src/jetson_multimedia_api/samples/
// 07_video_convert (which is what GStreamer's nvvidconv calls internally):
//   - NvBufSurfaceAllocate (NOT NvBufSurfaceCreate) with NvBufSurfaceAllocateParams
//   - dmabuf-based allocation tagged with NvBufSurfaceTag_VIDEO_CONVERT
//   - NvBufSurfTransform with explicit src_rect AND dst_rect
//   - Map / SyncForCpu / write / SyncForDevice / UnMap on CPU fill
//
// Build (on the Orin):
//   make m1_vic_convert
//
// Run:
//   ./m1_vic_convert
//
// Verify on the laptop (or anywhere with ffplay):
//   scp unitree@<orin>:/tmp/m1_out.nv12 ~/Downloads/
//   ffplay -f rawvideo -pixel_format nv12 -video_size 2560x720 ~/Downloads/m1_out.nv12

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

// Allocate a hardware surface via NvBufSurfaceAllocate (matches the path the
// JetPack 07_video_convert sample / NvBufSurf::NvAllocate wrapper uses).
// Returns 0 on success and populates *out with the freshly-allocated surface.
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
  (*out)->numFilled = 1;  // mirror the wrapper's behavior

  NvBufSurfaceParams &surf = (*out)->surfaceList[0];
  std::cout << "[" << tag << "] alloc OK " << surf.width << "x" << surf.height
            << " planes=" << surf.planeParams.num_planes
            << " fd=" << surf.bufferDesc;
  for (uint32_t pl = 0; pl < surf.planeParams.num_planes; ++pl) {
    std::cout << " p" << pl << "(" << surf.planeParams.width[pl] << "x"
              << surf.planeParams.height[pl]
              << ", pitch=" << surf.planeParams.pitch[pl]
              << ", bpp=" << surf.planeParams.bytesPerPix[pl] << ")";
  }
  std::cout << std::endl;
  return 0;
}

// Fill the BGRA source surface with a known gradient, using the JetPack-
// recommended sync sequence: Map(READ_WRITE) -> SyncForCpu -> write -> SyncForDevice -> UnMap.
int fillBgraGradient(NvBufSurface *surf) {
  if (NvBufSurfaceMap(surf, 0, 0, NVBUF_MAP_READ_WRITE) != 0) {
    std::cerr << "NvBufSurfaceMap(BGRA, plane=0, READ_WRITE) failed" << std::endl;
    return -1;
  }
  NvBufSurfaceSyncForCpu(surf, 0, 0);

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
      // BGRA: B = horizontal ramp, G = vertical ramp, R = 255, A = 0.
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
  if (NvBufSurfaceMap(surf, 0, -1 /* all planes */, NVBUF_MAP_READ_WRITE) != 0) {
    std::cerr << "NvBufSurfaceMap(NV12, all planes, READ_WRITE) failed"
              << std::endl;
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

  // Set up src_rect / dst_rect even though we're not cropping or scaling.
  // The wrapper class always populates these and VIC expects them.
  NvBufSurfTransformRect src_rect;
  NvBufSurfTransformRect dst_rect;
  std::memset(&src_rect, 0, sizeof(src_rect));
  std::memset(&dst_rect, 0, sizeof(dst_rect));
  src_rect.top = 0;
  src_rect.left = 0;
  src_rect.width = kWidth;
  src_rect.height = kHeight;
  dst_rect.top = 0;
  dst_rect.left = 0;
  dst_rect.width = kWidth;
  dst_rect.height = kHeight;

  NvBufSurfTransformParams tparams;
  std::memset(&tparams, 0, sizeof(tparams));
  tparams.transform_flag = static_cast<NvBufSurfTransform_Transform_Flag>(
      NVBUFSURF_TRANSFORM_FILTER | NVBUFSURF_TRANSFORM_FLIP);
  tparams.transform_flip = NvBufSurfTransform_None;
  tparams.transform_filter = NvBufSurfTransformInter_Nearest;
  tparams.src_rect = &src_rect;
  tparams.dst_rect = &dst_rect;

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
