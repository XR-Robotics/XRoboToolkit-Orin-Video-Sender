###############################################################################
#
# Standalone Makefile for OrinVideoSender
# Self-contained build configuration
# By liuchuan.yu@bytedance.com
#
###############################################################################

# Compiler settings
CXX = g++
PKG_CONFIG ?= pkg-config
APP := orinvideosender2
DEB_DEPS := build-essential pkg-config \
	libssl-dev libzmq3-dev libopencv-dev libglib2.0-dev \
	libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
	gstreamer1.0-tools gstreamer1.0-plugins-base \
	gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
	libavcodec-dev libavformat-dev libavutil-dev \
	libswscale-dev libavdevice-dev

###############################################################################
# WebCam

# # TCP w/o asio -- pass
# SRCS := \
# 	main_web_gst.cpp
###############################################################################

###############################################################################
# ZED

# TCP w/o asio + ZMQ raw/encoded publisher -- G1 Orin NX low-latency build
SRCS := \
	main_zed_tcp_zmq_zero_copy.cpp

# Original ZMQ-capable sender fallback
# SRCS := \
# 	main_zed_tcp_zmq.cpp

# TCP w/o asio, no ZMQ fallback
# SRCS := \
# 	main_zed_tcp.cpp

# # TCP with asio -- pass
# SRCS := \
# 	main_zed_asio.cpp

# # UDP w/ asio -- pass
# SRCS:= \
# 	main_zed_asio_udp.cpp

# # [NOT WORKING] Zero Copy - depends on jetson multimedia api
# SRCS := \
# 	main_zed_zero_copy.cpp
###############################################################################

OBJS := $(SRCS:.cpp=.o)

# Include paths
CPPFLAGS := -std=c++11 \
	-I./asio-1.30.2/include \
	-I/usr/local/zed/include \
	-I/usr/include/opencv4 \
	-I/usr/local/cuda/include \
	$(shell $(PKG_CONFIG) --cflags gstreamer-1.0 2>/dev/null) \
	$(shell $(PKG_CONFIG) --cflags gstreamer-app-1.0 2>/dev/null) \
	$(shell $(PKG_CONFIG) --cflags glib-2.0 2>/dev/null) \
	$(shell $(PKG_CONFIG) --cflags libzmq 2>/dev/null)

# Compiler flags
CXXFLAGS := -Wall -Wextra -O2 -g

# FFmpeg flags
CXXFLAGS += $(shell $(PKG_CONFIG) --cflags libavcodec libavformat libavutil libswscale libavdevice 2>/dev/null)

# Library paths and libraries
LDFLAGS := -L/usr/local/zed/lib \
	-L/usr/local/cuda/lib64
	# -L/usr/lib/aarch64-linux-gnu

# FFmpeg libraries (must come first to avoid conflicts)
LDFLAGS += $(shell $(PKG_CONFIG) --libs libavcodec libavformat libavutil libswscale libavdevice 2>/dev/null || echo "-lavcodec -lavformat -lavutil -lswscale -lavdevice")

# Core libraries
LDFLAGS += -lsl_zed \
	-lcuda -lcudart \
	-lopencv_core -lopencv_imgproc -lopencv_videoio -lopencv_imgcodecs \
	-lssl -lcrypto \
	-lpthread \
	-lstdc++

# GStreamer libraries
LDFLAGS += $(shell $(PKG_CONFIG) --libs gstreamer-1.0 2>/dev/null || echo "-lgstreamer-1.0") \
	$(shell $(PKG_CONFIG) --libs gstreamer-app-1.0 2>/dev/null || echo "-lgstapp-1.0") \
	$(shell $(PKG_CONFIG) --libs glib-2.0 2>/dev/null || echo "-lglib-2.0")

# ZMQ library
LDFLAGS += $(shell $(PKG_CONFIG) --libs libzmq 2>/dev/null || echo "-lzmq")

all: $(APP)

debug: CXXFLAGS += -DDEBUG -g3 -O0
debug: $(APP)

%.o: %.cpp
	@echo "Compiling: $<"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(APP): $(OBJS)
	@echo "Linking: $@"
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

###############################################################################
# Plan A milestone targets — Jetson Multimedia API (VIC + NVENC direct).
# Independent of $(APP); built only when explicitly requested. These targets
# do not pull in the ZED SDK, OpenCV, or GStreamer; they only need MMAPI.
###############################################################################

MMAPI_INCLUDES := -I/usr/src/jetson_multimedia_api/include
MMAPI_LDFLAGS  := -L/usr/lib/aarch64-linux-gnu/tegra \
	-Wl,-rpath,/usr/lib/aarch64-linux-gnu/tegra \
	-lnvbufsurface -lnvbufsurftransform

m1_vic_convert: m1_vic_convert.cpp
	@echo "Compiling+linking: $@"
	$(CXX) -std=c++11 -O2 -g -Wall -Wextra $(MMAPI_INCLUDES) \
		$< -o $@ $(MMAPI_LDFLAGS)

# M2 needs ZED SDK + CUDA in addition to MMAPI.
M2_INCLUDES := $(MMAPI_INCLUDES) -I/usr/local/zed/include -I/usr/local/cuda/include
M2_LDFLAGS  := $(MMAPI_LDFLAGS) \
	-L/usr/local/zed/lib -L/usr/local/cuda/lib64 \
	-lsl_zed -lcuda -lcudart -lpthread

m2_zed_vic_nv12: m2_zed_vic_nv12.cpp
	@echo "Compiling+linking: $@"
	$(CXX) -std=c++11 -O2 -g -Wall -Wextra $(M2_INCLUDES) \
		$< -o $@ $(M2_LDFLAGS)

# M3 needs ZED + CUDA + MMAPI helper classes (compiled in from the JetPack tree).
MMAPI_CLASS_DIR := /usr/src/jetson_multimedia_api/samples/common/classes
MMAPI_BUILD_DIR := _mmapi_build
M3_MMAPI_SRCS := \
	$(MMAPI_CLASS_DIR)/NvBuffer.cpp \
	$(MMAPI_CLASS_DIR)/NvLogging.cpp \
	$(MMAPI_CLASS_DIR)/NvElement.cpp \
	$(MMAPI_CLASS_DIR)/NvElementProfiler.cpp \
	$(MMAPI_CLASS_DIR)/NvV4l2Element.cpp \
	$(MMAPI_CLASS_DIR)/NvV4l2ElementPlane.cpp \
	$(MMAPI_CLASS_DIR)/NvVideoEncoder.cpp
M3_MMAPI_OBJS := $(patsubst $(MMAPI_CLASS_DIR)/%.cpp,$(MMAPI_BUILD_DIR)/%.o,$(M3_MMAPI_SRCS))

$(MMAPI_BUILD_DIR):
	mkdir -p $@

$(MMAPI_BUILD_DIR)/%.o: $(MMAPI_CLASS_DIR)/%.cpp | $(MMAPI_BUILD_DIR)
	@echo "Compiling MMAPI helper: $<"
	$(CXX) -std=c++11 -O2 -g $(M2_INCLUDES) -c $< -o $@

m3_zed_vic_nvenc: m3_zed_vic_nvenc.cpp $(M3_MMAPI_OBJS)
	@echo "Compiling+linking: $@"
	$(CXX) -std=c++11 -O2 -g -Wall -Wextra $(M2_INCLUDES) \
		m3_zed_vic_nvenc.cpp $(M3_MMAPI_OBJS) -o $@ $(M2_LDFLAGS) -lnvv4l2

# Plan A end product: drop-in TCP-protocol replacement for orinvideosender2.
orinvideosender3: main_zed_mmapi.cpp $(M3_MMAPI_OBJS) network_helper.hpp
	@echo "Compiling+linking: $@"
	$(CXX) -std=c++11 -O2 -g -Wall -Wextra $(M2_INCLUDES) \
		main_zed_mmapi.cpp $(M3_MMAPI_OBJS) -o $@ $(M2_LDFLAGS) -lnvv4l2

clean:
	rm -rf $(APP) $(OBJS) m1_vic_convert m2_zed_vic_nv12 m3_zed_vic_nvenc \
		orinvideosender3 $(MMAPI_BUILD_DIR)

install: $(APP)
	@echo "Installing $(APP)..."
	install -D $(APP) /usr/local/bin/$(APP)

install-deps:
	sudo apt update
	sudo apt install -y $(DEB_DEPS)

check-jetson-deps:
	@$(PKG_CONFIG) --exists gstreamer-1.0 gstreamer-app-1.0 glib-2.0 libzmq opencv4
	@gst-inspect-1.0 nvvidconv | grep -q '^Factory Details'
	@gst-inspect-1.0 nvv4l2h264enc | grep -q '^Factory Details'
	@gst-inspect-1.0 h264parse | grep -q '^Factory Details'
	@test -d /usr/local/zed/include
	@test -d /usr/local/cuda/include
	@echo "JetPack/ZED build dependencies look available."

.PHONY: all debug clean install install-deps check-jetson-deps
