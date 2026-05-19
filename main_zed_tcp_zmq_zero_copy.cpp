#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <glib-unix.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <openssl/md5.h>
#include <sstream>
#include <sl/Camera.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <zmq.h>

#include "network_helper.hpp"

// Network Protocol Structures
struct CameraRequestData {
  int width;
  int height;
  int fps;
  int bitrate;
  int enableMvHevc;
  int renderMode;
  int port;
  std::string camera;
  std::string ip;

  CameraRequestData()
      : width(0), height(0), fps(0), bitrate(0), enableMvHevc(0), renderMode(0),
        port(0) {}
};

struct NetworkDataProtocol {
  std::string command;
  int length;
  std::vector<uint8_t> data;

  NetworkDataProtocol() : length(0) {}
  NetworkDataProtocol(const std::string &cmd, const std::vector<uint8_t> &d)
      : command(cmd), data(d), length(d.size()) {}
};

// Deserialization functions
class CameraRequestDeserializer {
public:
  static CameraRequestData deserialize(const std::vector<uint8_t> &data) {
    if (data.size() < 10) {
      throw std::invalid_argument("Data is too small for valid camera request");
    }

    size_t offset = 0;

    // Check magic bytes (0xCA, 0xFE)
    if (data[offset] != 0xCA || data[offset + 1] != 0xFE) {
      throw std::invalid_argument("Invalid magic bytes");
    }
    offset += 2;

    // Check protocol version
    uint8_t version = data[offset++];
    if (version != 1) {
      throw std::invalid_argument("Unsupported protocol version");
    }

    CameraRequestData result;

    // Read integer fields (7 * 4 bytes)
    if (offset + 28 > data.size()) {
      throw std::invalid_argument("Data too small for integer fields");
    }

    result.width = readInt32(data, offset);
    result.height = readInt32(data, offset + 4);
    result.fps = readInt32(data, offset + 8);
    result.bitrate = readInt32(data, offset + 12);
    result.enableMvHevc = readInt32(data, offset + 16);
    result.renderMode = readInt32(data, offset + 20);
    result.port = readInt32(data, offset + 24);
    offset += 28;

    // Read strings with compact encoding
    result.camera = readCompactString(data, offset);
    result.ip = readCompactString(data, offset);

    return result;
  }

private:
  static int32_t readInt32(const std::vector<uint8_t> &data, size_t offset) {
    if (offset + 4 > data.size()) {
      throw std::out_of_range("Not enough data to read int32");
    }

    // Little-endian format (matching C# BitConverter default)
    return static_cast<int32_t>((data[offset]) | (data[offset + 1] << 8) |
                                (data[offset + 2] << 16) |
                                (data[offset + 3] << 24));
  }

  static std::string readCompactString(const std::vector<uint8_t> &data,
                                       size_t &offset) {
    if (offset >= data.size()) {
      throw std::out_of_range("Not enough data to read string length");
    }

    uint8_t length = data[offset++];
    if (length == 0) {
      return std::string();
    }

    if (offset + length > data.size()) {
      throw std::out_of_range("Not enough data to read string content");
    }

    std::string result(reinterpret_cast<const char *>(&data[offset]), length);
    offset += length;
    return result;
  }
};

class NetworkDataProtocolDeserializer {
public:
  static NetworkDataProtocol deserialize(const std::vector<uint8_t> &buffer) {
    if (buffer.size() <
        8) { // Minimum: 4 bytes command length + 4 bytes data length
      throw std::invalid_argument("Buffer too small for valid protocol data");
    }

    size_t offset = 0;

    // Read command length
    int32_t commandLength = readInt32(buffer, offset);
    offset += 4;

    if (commandLength < 0 || offset + commandLength > buffer.size()) {
      throw std::invalid_argument("Invalid command length");
    }

    // Read command
    std::string command;
    if (commandLength > 0) {
      command = std::string(reinterpret_cast<const char *>(&buffer[offset]),
                            commandLength);
      // Remove any null terminators
      size_t nullPos = command.find('\0');
      if (nullPos != std::string::npos) {
        command = command.substr(0, nullPos);
      }
    }
    offset += commandLength;

    if (offset + 4 > buffer.size()) {
      throw std::invalid_argument("Buffer too small for data length");
    }

    // Read data length
    int32_t dataLength = readInt32(buffer, offset);
    offset += 4;

    if (dataLength < 0 || offset + dataLength > buffer.size()) {
      throw std::invalid_argument("Invalid data length");
    }

    // Read data
    std::vector<uint8_t> data;
    if (dataLength > 0) {
      data.assign(buffer.begin() + offset,
                  buffer.begin() + offset + dataLength);
    }

    return NetworkDataProtocol(command, data);
  }

private:
  static int32_t readInt32(const std::vector<uint8_t> &data, size_t offset) {
    if (offset + 4 > data.size()) {
      throw std::out_of_range("Not enough data to read int32");
    }

    // Little-endian format
    return static_cast<int32_t>((data[offset]) | (data[offset + 1] << 8) |
                                (data[offset + 2] << 16) |
                                (data[offset + 3] << 24));
  }
};

// Global camera configuration
CameraRequestData current_camera_config;

// Thread-safe global state
std::atomic<bool> stop_requested{false};
std::atomic<bool> streaming_active{false};
std::atomic<bool> encoding_enabled{false};
std::atomic<bool> send_enabled{false};
std::atomic<bool> preview_enabled{false};
std::atomic<bool> zmq_enabled{false};
std::atomic<bool> zmq_raw_mode{false};  // 发送原始图像而不是H.264

// Runtime options. Defaults target the G1 Orin NX + PICO path.
std::string zed_view_mode = "sbs";     // "sbs" or "left"
std::string codec_override = "auto";   // "auto", "h264", or "h265"
std::atomic<bool> low_latency_mode{true};
std::atomic<bool> legacy_pipeline_mode{false};
std::atomic<bool> measure_encoder_latency{false};
int idr_interval = 15;
int stats_interval = 120;

// Thread management
std::unique_ptr<std::thread> listen_thread;
std::unique_ptr<std::thread> streaming_thread;
std::mutex config_mutex;
std::condition_variable streaming_cv;
std::mutex streaming_mutex;

// Network components
std::unique_ptr<TCPClient> sender_ptr;
std::unique_ptr<TCPServer> server_ptr;
std::string send_to_server = "";
int send_to_port = 0;

// ZMQ components
void* zmq_context = nullptr;
void* zmq_publisher = nullptr;
std::string zmq_endpoint = "";
std::mutex zmq_mutex;

using SteadyClock = std::chrono::steady_clock;

struct StreamStats {
  uint64_t grabbed = 0;
  uint64_t retrieved = 0;
  uint64_t appsrc_pushed = 0;
  uint64_t encoded = 0;
  uint64_t tcp_sent = 0;
  uint64_t raw_zmq_sent = 0;
  uint64_t raw_zmq_dropped = 0;
  uint64_t encoded_zmq_sent = 0;
  uint64_t encoded_zmq_dropped = 0;
  double grab_ms = 0.0;
  double retrieve_ms = 0.0;
  double appsrc_ms = 0.0;
  double encode_ms = 0.0;
  double tcp_send_ms = 0.0;
  double raw_zmq_ms = 0.0;
  SteadyClock::time_point start = SteadyClock::now();
  SteadyClock::time_point last_log = SteadyClock::now();
  uint64_t grabbed_at_last_log = 0;
  std::map<uint64_t, SteadyClock::time_point> pending_encode_times;
};

std::mutex stats_mutex;
StreamStats stream_stats;

bool initialize_sender() {
  int retry = 10;
  while (retry > 0 && !sender_ptr && !stop_requested.load()) {
    try {
      sender_ptr = std::unique_ptr<TCPClient>(
          new TCPClient(send_to_server, send_to_port));
      std::cout << "Attempting to connect to " << send_to_server << ":"
                << send_to_port << std::endl;
      sender_ptr->connect();
      return true;
    } catch (const TCPException &e) {
      std::cerr << "Failed to connect to server: " << e.what() << std::endl;
      sender_ptr = nullptr;
    }
    // Sleep for 1 second
    std::this_thread::sleep_for(std::chrono::seconds(1));
    retry--;
  }
  return false;
}

bool initialize_zmq() {
  if (zmq_endpoint.empty()) {
    return false;
  }

  try {
    std::lock_guard<std::mutex> lock(zmq_mutex);
    
    // Create ZMQ context
    zmq_context = zmq_ctx_new();
    if (!zmq_context) {
      std::cerr << "Failed to create ZMQ context" << std::endl;
      return false;
    }

    // Create publisher socket
    zmq_publisher = zmq_socket(zmq_context, ZMQ_PUB);
    if (!zmq_publisher) {
      std::cerr << "Failed to create ZMQ publisher socket" << std::endl;
      zmq_ctx_destroy(zmq_context);
      zmq_context = nullptr;
      return false;
    }

    // Set socket options for better performance
    int hwm = 2;  // Keep the stream fresh; subscribers should use CONFLATE too.
    zmq_setsockopt(zmq_publisher, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    
    int linger = 0;  // Don't wait for pending messages on close
    zmq_setsockopt(zmq_publisher, ZMQ_LINGER, &linger, sizeof(linger));

    // Bind to endpoint
    if (zmq_bind(zmq_publisher, zmq_endpoint.c_str()) != 0) {
      std::cerr << "Failed to bind ZMQ publisher to " << zmq_endpoint << std::endl;
      zmq_close(zmq_publisher);
      zmq_ctx_destroy(zmq_context);
      zmq_publisher = nullptr;
      zmq_context = nullptr;
      return false;
    }

    std::cout << "ZMQ publisher bound to " << zmq_endpoint << std::endl;
    return true;

  } catch (const std::exception& e) {
    std::cerr << "Exception during ZMQ initialization: " << e.what() << std::endl;
    return false;
  }
}

void cleanup_zmq() {
  std::lock_guard<std::mutex> lock(zmq_mutex);
  
  if (zmq_publisher) {
    zmq_close(zmq_publisher);
    zmq_publisher = nullptr;
  }
  
  if (zmq_context) {
    zmq_ctx_destroy(zmq_context);
    zmq_context = nullptr;
  }
  
  std::cout << "ZMQ cleaned up" << std::endl;
}

// Template helper for C++11 make_unique replacement
template <typename T, typename... Args>
std::unique_ptr<T> make_unique_helper(Args &&...args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

std::string toLowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool isSupportedZedCameraName(const std::string &camera) {
  std::string normalized = toLowerCopy(camera);
  return normalized == "zed" || normalized == "zedmini" ||
         normalized == "zed_mini" || normalized == "zed-mini";
}

sl::VIEW selectedZedView() {
  return zed_view_mode == "left" ? sl::VIEW::LEFT : sl::VIEW::SIDE_BY_SIDE;
}

const char *selectedZedViewName() {
  return zed_view_mode == "left" ? "LEFT" : "SIDE_BY_SIDE";
}

void applyCodecOverride(CameraRequestData &config) {
  if (codec_override == "h264") {
    config.enableMvHevc = 0;
  } else if (codec_override == "h265") {
    config.enableMvHevc = 1;
  }
}

double elapsedMs(const SteadyClock::time_point &start,
                 const SteadyClock::time_point &end) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
             end - start)
      .count();
}

void resetStreamStats() {
  std::lock_guard<std::mutex> lock(stats_mutex);
  stream_stats = StreamStats();
}

void recordGrabTiming(double ms) {
  std::lock_guard<std::mutex> lock(stats_mutex);
  stream_stats.grabbed++;
  stream_stats.grab_ms += ms;
}

void recordRetrieveTiming(double ms) {
  std::lock_guard<std::mutex> lock(stats_mutex);
  stream_stats.retrieved++;
  stream_stats.retrieve_ms += ms;
}

void recordRawZmqTiming(double ms, bool sent) {
  std::lock_guard<std::mutex> lock(stats_mutex);
  if (sent) {
    stream_stats.raw_zmq_sent++;
    stream_stats.raw_zmq_ms += ms;
  } else {
    stream_stats.raw_zmq_dropped++;
  }
}

void recordAppsrcTiming(uint64_t frame_id, double ms) {
  std::lock_guard<std::mutex> lock(stats_mutex);
  stream_stats.appsrc_pushed++;
  stream_stats.appsrc_ms += ms;
  stream_stats.pending_encode_times[frame_id] = SteadyClock::now();
  while (stream_stats.pending_encode_times.size() > 240) {
    stream_stats.pending_encode_times.erase(
        stream_stats.pending_encode_times.begin());
  }
}

void recordEncodedTiming(uint64_t frame_id) {
  std::lock_guard<std::mutex> lock(stats_mutex);
  stream_stats.encoded++;
  std::map<uint64_t, SteadyClock::time_point>::iterator it =
      stream_stats.pending_encode_times.find(frame_id);
  if (it != stream_stats.pending_encode_times.end()) {
    stream_stats.encode_ms += elapsedMs(it->second, SteadyClock::now());
    stream_stats.pending_encode_times.erase(it);
  }
}

void recordTcpSendTiming(double ms) {
  std::lock_guard<std::mutex> lock(stats_mutex);
  stream_stats.tcp_sent++;
  stream_stats.tcp_send_ms += ms;
}

void recordEncodedZmqResult(bool sent) {
  std::lock_guard<std::mutex> lock(stats_mutex);
  if (sent) {
    stream_stats.encoded_zmq_sent++;
  } else {
    stream_stats.encoded_zmq_dropped++;
  }
}

void maybeLogStreamStats() {
  std::lock_guard<std::mutex> lock(stats_mutex);
  if (stats_interval <= 0 || stream_stats.grabbed == 0 ||
      stream_stats.grabbed - stream_stats.grabbed_at_last_log <
          static_cast<uint64_t>(stats_interval)) {
    return;
  }

  SteadyClock::time_point now = SteadyClock::now();
  double window_sec =
      std::chrono::duration_cast<std::chrono::duration<double>>(
          now - stream_stats.last_log)
          .count();
  double fps = window_sec > 0.0
                   ? (stream_stats.grabbed - stream_stats.grabbed_at_last_log) /
                         window_sec
                   : 0.0;

  double grab_avg = stream_stats.grabbed ? stream_stats.grab_ms / stream_stats.grabbed : 0.0;
  double retrieve_avg =
      stream_stats.retrieved ? stream_stats.retrieve_ms / stream_stats.retrieved : 0.0;
  double appsrc_avg =
      stream_stats.appsrc_pushed ? stream_stats.appsrc_ms / stream_stats.appsrc_pushed : 0.0;
  double encode_avg =
      stream_stats.encoded ? stream_stats.encode_ms / stream_stats.encoded : 0.0;
  double tcp_avg =
      stream_stats.tcp_sent ? stream_stats.tcp_send_ms / stream_stats.tcp_sent : 0.0;
  double raw_zmq_avg =
      stream_stats.raw_zmq_sent ? stream_stats.raw_zmq_ms / stream_stats.raw_zmq_sent : 0.0;

  std::cout << std::fixed << std::setprecision(2)
            << "[stats] fps=" << fps
            << " frames=" << stream_stats.grabbed
            << " grab=" << grab_avg << "ms"
            << " retrieve=" << retrieve_avg << "ms"
            << " appsrc=" << appsrc_avg << "ms"
            << " encode=" << encode_avg << "ms"
            << " tcp_send=" << tcp_avg << "ms"
            << " raw_zmq=" << raw_zmq_avg << "ms"
            << " encoded=" << stream_stats.encoded
            << " tcp_sent=" << stream_stats.tcp_sent
            << " raw_zmq_sent=" << stream_stats.raw_zmq_sent
            << " raw_zmq_drop=" << stream_stats.raw_zmq_dropped
            << " enc_zmq_sent=" << stream_stats.encoded_zmq_sent
            << " enc_zmq_drop=" << stream_stats.encoded_zmq_dropped
            << std::endl;

  stream_stats.last_log = now;
  stream_stats.grabbed_at_last_log = stream_stats.grabbed;
}

GParamSpec *findGstProperty(GstElement *element, const char *name) {
  if (!element || !name) {
    return nullptr;
  }
  return g_object_class_find_property(G_OBJECT_GET_CLASS(element), name);
}

bool setBoolPropertyIfPresent(GstElement *element, const char *name,
                              bool value) {
  GParamSpec *pspec = findGstProperty(element, name);
  if (!pspec || G_PARAM_SPEC_VALUE_TYPE(pspec) != G_TYPE_BOOLEAN) {
    return false;
  }
  g_object_set(G_OBJECT(element), name, static_cast<gboolean>(value), nullptr);
  std::cout << "[gst] " << GST_ELEMENT_NAME(element) << "." << name << "="
            << (value ? "true" : "false") << std::endl;
  return true;
}

bool setIntPropertyIfPresent(GstElement *element, const char *name, int value) {
  GParamSpec *pspec = findGstProperty(element, name);
  if (!pspec) {
    return false;
  }

  GType type = G_PARAM_SPEC_VALUE_TYPE(pspec);
  if (type == G_TYPE_INT || G_TYPE_FUNDAMENTAL(type) == G_TYPE_ENUM) {
    g_object_set(G_OBJECT(element), name, static_cast<gint>(value), nullptr);
  } else if (type == G_TYPE_UINT) {
    g_object_set(G_OBJECT(element), name,
                 static_cast<guint>(std::max(0, value)), nullptr);
  } else if (type == G_TYPE_LONG) {
    g_object_set(G_OBJECT(element), name, static_cast<glong>(value), nullptr);
  } else if (type == G_TYPE_ULONG) {
    g_object_set(G_OBJECT(element), name,
                 static_cast<gulong>(std::max(0, value)), nullptr);
  } else {
    return false;
  }

  std::cout << "[gst] " << GST_ELEMENT_NAME(element) << "." << name << "="
            << value << std::endl;
  return true;
}

bool setFirstIntPropertyIfPresent(GstElement *element,
                                  const std::vector<std::string> &names,
                                  int value) {
  for (size_t i = 0; i < names.size(); ++i) {
    if (setIntPropertyIfPresent(element, names[i].c_str(), value)) {
      return true;
    }
  }
  return false;
}

void configureEncoder(GstElement *encoder, const CameraRequestData &config) {
  if (!encoder) {
    return;
  }

  setIntPropertyIfPresent(encoder, "bitrate", config.bitrate);

  if (!low_latency_mode.load()) {
    return;
  }

  setBoolPropertyIfPresent(encoder, "maxperf-enable", true);
  setBoolPropertyIfPresent(encoder, "insert-sps-pps", true);
  setBoolPropertyIfPresent(encoder, "insert-vui", true);
  setIntPropertyIfPresent(encoder, "iframeinterval", idr_interval);
  setIntPropertyIfPresent(encoder, "idrinterval", idr_interval);
  setIntPropertyIfPresent(encoder, "control-rate", 1);
  setFirstIntPropertyIfPresent(
      encoder, std::vector<std::string>{"num-B-Frames", "num-b-frames",
                                        "num-bframes"},
      0);

  if (measure_encoder_latency.load()) {
    setBoolPropertyIfPresent(encoder, "MeasureEncoderLatency", true);
  }
}

void configureParser(GstElement *parser) {
  if (!parser) {
    return;
  }
  setIntPropertyIfPresent(parser, "config-interval", -1);
}

// Forward declarations
void handleOpenCamera(const std::vector<uint8_t> &data);
void handleCloseCamera(const std::vector<uint8_t> &data);
void startStreamingThread();
void stopStreamingThread();
void streamingThreadFunction();
void listenThreadFunction(const std::string &listen_address);

void onDataCallback(const std::string &command) {
  // Convert string to binary data for protocol parsing
  std::vector<uint8_t> binaryData(command.begin(), command.end());

  for (size_t i = 0; i < std::min(binaryData.size(), size_t(32)); ++i) {
    std::cout << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<unsigned int>(binaryData[i]) << " ";
  }
  std::cout << std::dec << std::endl;

  // First, extract the actual protocol data from the 4-byte wrapper
  if (binaryData.size() < 4) {
    std::cerr << "Data too small to contain length header" << std::endl;
    return;
  }

  // Read the 4-byte header (big-endian format) to get the actual data length
  uint32_t bodyLength = (static_cast<uint32_t>(binaryData[0]) << 24) |
                        (static_cast<uint32_t>(binaryData[1]) << 16) |
                        (static_cast<uint32_t>(binaryData[2]) << 8) |
                        static_cast<uint32_t>(binaryData[3]);

  if (4 + bodyLength > binaryData.size()) {
    std::cerr << "Data too small for declared body length. Expected: "
              << (4 + bodyLength) << ", got: " << binaryData.size()
              << std::endl;
    return;
  }

  // Extract the actual protocol data (skip the 4-byte header)
  std::vector<uint8_t> protocolData(binaryData.begin() + 4,
                                    binaryData.begin() + 4 + bodyLength);

  for (size_t i = 0; i < std::min(protocolData.size(), size_t(32)); ++i) {
    std::cout << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<unsigned int>(protocolData[i]) << " ";
  }
  std::cout << std::dec << std::endl;

  // Now try to parse as NetworkDataProtocol format (with length prefixes)
  try {
    // Debug: Print first few fields of the protocol data
    if (protocolData.size() >= 8) {
      int32_t cmdLen = (protocolData[0]) | (protocolData[1] << 8) |
                       (protocolData[2] << 16) | (protocolData[3] << 24);
      int32_t dataLenPos = 4 + cmdLen;
      std::cout << "Command length: " << cmdLen << std::endl;
      if (static_cast<size_t>(dataLenPos + 4) <= protocolData.size()) {
        int32_t dataLen = (protocolData[dataLenPos]) |
                          (protocolData[dataLenPos + 1] << 8) |
                          (protocolData[dataLenPos + 2] << 16) |
                          (protocolData[dataLenPos + 3] << 24);
        std::cout << "Data length: " << dataLen << std::endl;
      }
    }

    NetworkDataProtocol protocol =
        NetworkDataProtocolDeserializer::deserialize(protocolData);

    std::cout << "Received protocol command: '" << protocol.command
              << "' (length: " << protocol.command.length() << ")" << std::endl;

    // Handle the protocol commands
    if (protocol.command == "OPEN_CAMERA") {
      handleOpenCamera(protocol.data);
    } else if (protocol.command == "CLOSE_CAMERA") {
      handleCloseCamera(protocol.data);
    } else {
      std::cout << "Unknown protocol command: " << protocol.command
                << std::endl;
    }
    return;
  } catch (const std::exception &e) {
    // If NetworkDataProtocol parsing fails, try simple command format
    std::cout << "Failed to parse as NetworkDataProtocol: " << e.what()
              << std::endl;
  }
}

void onDisconnectCallback() {
  std::cout << "Client disconnected, stopping streaming" << std::endl;
  stopStreamingThread();
}

void listenThreadFunction(const std::string &listen_address) {
  std::cout << "Listen thread started on " << listen_address << std::endl;

  while (!stop_requested.load()) {
    try {
      // Initialize TCPServer
      server_ptr = make_unique_helper<TCPServer>(listen_address);
      server_ptr->setDataCallback(onDataCallback);
      server_ptr->setDisconnectCallback(onDisconnectCallback);
      server_ptr->start();
      std::cout << "TCPServer is listening on " << listen_address << std::endl;

      // Wait for server to stop (client disconnect or error)
      while (!stop_requested.load() && server_ptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      if (server_ptr) {
        server_ptr->stop();
        server_ptr = nullptr;
      }

      if (!stop_requested.load()) {
        std::cout << "Waiting for new connection..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }

    } catch (const std::exception &e) {
      std::cerr << "Listen thread error: " << e.what() << std::endl;
      if (!stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
    }
  }

  std::cout << "Listen thread stopped" << std::endl;
}

void handle_sigint(int) {
  std::cout << "\nSIGINT received. Stopping all threads..." << std::endl;
  stop_requested.store(true);

  // Stop streaming first
  stopStreamingThread();

  // Stop server
  if (server_ptr) {
    server_ptr->stop();
    server_ptr = nullptr;
  }

  // Cleanup ZMQ
  cleanup_zmq();

  // Wake up any waiting threads
  streaming_cv.notify_all();
}

// Reference:
// https://github.com/stereolabs/zed-sdk/blob/master/object%20detection/birds%20eye%20viewer/cpp/include/utils.hpp#L82-L106
inline cv::Mat slMat2cvMat(sl::Mat &input) {
  // Mapping between MAT_TYPE and CV_TYPE
  int cv_type = -1;
  switch (input.getDataType()) {
  case sl::MAT_TYPE::F32_C1:
    cv_type = CV_32FC1;
    break;
  case sl::MAT_TYPE::F32_C2:
    cv_type = CV_32FC2;
    break;
  case sl::MAT_TYPE::F32_C3:
    cv_type = CV_32FC3;
    break;
  case sl::MAT_TYPE::F32_C4:
    cv_type = CV_32FC4;
    break;
  case sl::MAT_TYPE::U8_C1:
    cv_type = CV_8UC1;
    break;
  case sl::MAT_TYPE::U8_C2:
    cv_type = CV_8UC2;
    break;
  case sl::MAT_TYPE::U8_C3:
    cv_type = CV_8UC3;
    break;
  case sl::MAT_TYPE::U8_C4:
    cv_type = CV_8UC4;
    break;
  default:
    break;
  }

  return cv::Mat(input.getHeight(), input.getWidth(), cv_type,
                 input.getPtr<sl::uchar1>(sl::MEM::CPU));
}

GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample)
    return GST_FLOW_ERROR;

  GstBuffer *buffer = gst_sample_get_buffer(sample);
  if (!buffer) {
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }
  if (buffer) {
    recordEncodedTiming(static_cast<uint64_t>(GST_BUFFER_OFFSET(buffer)));
  }

  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    const uint8_t *data = map.data;
    gsize size = map.size;

    // TCP发送 (原有逻辑)
    if (send_enabled.load() && sender_ptr && sender_ptr->isConnected() &&
        data && size > 0) {
      try {
        std::vector<uint8_t> packet(4 + size);
        packet[0] = (size >> 24) & 0xFF;
        packet[1] = (size >> 16) & 0xFF;
        packet[2] = (size >> 8) & 0xFF;
        packet[3] = (size)&0xFF;
        std::copy(data, data + size, packet.begin() + 4);

        SteadyClock::time_point send_start = SteadyClock::now();
        sender_ptr->sendData(packet);
        recordTcpSendTiming(elapsedMs(send_start, SteadyClock::now()));
      } catch (const TCPException &e) {
        std::cerr << "TCP error in on_new_sample: " << e.what() << std::endl;
        // Don't quit the whole program, just stop streaming
        streaming_active.store(false);
      } catch (const std::exception &e) {
        std::cerr << "Unexpected error in on_new_sample: " << e.what()
                  << std::endl;
        streaming_active.store(false);
      }
    }

    // Encoded ZMQ output is used only by --zmq. --zmq-raw publishes raw frames
    // from the capture loop, which avoids mixing H.264 packets into the raw feed.
    if (!zmq_raw_mode.load() && zmq_enabled.load() && zmq_publisher && data &&
        size > 0) {
      try {
        std::lock_guard<std::mutex> lock(zmq_mutex);
        
        // 创建带有长度头的消息
        std::vector<uint8_t> zmq_packet(4 + size);
        zmq_packet[0] = (size >> 24) & 0xFF;
        zmq_packet[1] = (size >> 16) & 0xFF;
        zmq_packet[2] = (size >> 8) & 0xFF;
        zmq_packet[3] = (size) & 0xFF;
        std::copy(data, data + size, zmq_packet.begin() + 4);

        // 通过ZMQ发送 (非阻塞)
        int rc = zmq_send(zmq_publisher, zmq_packet.data(), zmq_packet.size(), ZMQ_DONTWAIT);
        if (rc == -1) {
          if (zmq_errno() != EAGAIN) {  // EAGAIN means queue is full, which is OK
            std::cerr << "ZMQ send error: " << zmq_strerror(zmq_errno()) << std::endl;
          }
          recordEncodedZmqResult(false);
        } else {
          recordEncodedZmqResult(true);
        }
      } catch (const std::exception &e) {
        std::cerr << "Unexpected error in ZMQ send: " << e.what() << std::endl;
      }
    }

    gst_buffer_unmap(buffer, &map);
  }

  maybeLogStreamStats();
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

// Handler function implementations
void handleOpenCamera(const std::vector<uint8_t> &data) {
  std::cout << "Handling OPEN_CAMERA command" << std::endl;

  try {
    // Parse the camera configuration data
    CameraRequestData cameraConfig =
        CameraRequestDeserializer::deserialize(data);

    std::cout << "Camera config - Width: " << cameraConfig.width
              << ", Height: " << cameraConfig.height
              << ", FPS: " << cameraConfig.fps
              << ", Bitrate: " << cameraConfig.bitrate
              << ", IP: " << cameraConfig.ip << ", Port: " << cameraConfig.port
              << ", type: " << cameraConfig.camera << std::endl;
    // Accept the names used by XRoboToolkit UI and older sender builds.
    if (!isSupportedZedCameraName(cameraConfig.camera)) {
      std::cout << "Unsupported camera type: " << cameraConfig.camera
                << ". Supported names: ZED, ZEDMINI." << std::endl;
      return;
    }

    applyCodecOverride(cameraConfig);

    // Store the camera configuration globally
    {
      std::lock_guard<std::mutex> lock(config_mutex);
      current_camera_config = cameraConfig;
    }

    // Set the global sender connection parameters from config
    send_to_server = cameraConfig.ip;
    send_to_port = cameraConfig.port;

    std::cout << "Updated sender target to " << send_to_server << ":"
              << send_to_port << std::endl;

    // Start the streaming thread which will use the updated config
    startStreamingThread();

  } catch (const std::exception &e) {
    std::cerr << "Failed to parse camera config: " << e.what() << std::endl;
    // Start with default configuration only if we have valid defaults
    if (!send_to_server.empty() && send_to_port > 0) {
      startStreamingThread();
    } else {
      std::cerr
          << "No valid server configuration available, cannot start streaming"
          << std::endl;
    }
  }
}

void handleCloseCamera(const std::vector<uint8_t> &data) {
  std::cout << "Handling CLOSE_CAMERA command" << std::endl;
  stopStreamingThread();
}

void startStreamingThread() {
  std::lock_guard<std::mutex> lock(streaming_mutex);
  if (streaming_thread && streaming_thread->joinable()) {
    std::cout << "Streaming thread already running" << std::endl;
    return;
  }

  streaming_active.store(true);
  streaming_thread = make_unique_helper<std::thread>(streamingThreadFunction);
  std::cout << "Started streaming thread" << std::endl;
}

void stopStreamingThread() {
  std::lock_guard<std::mutex> lock(streaming_mutex);

  streaming_active.store(false);
  encoding_enabled.store(false);
  send_enabled.store(false);
  zmq_enabled.store(false);

  // Disconnect sender if connected
  if (sender_ptr && sender_ptr->isConnected()) {
    sender_ptr->disconnect();
  }
  sender_ptr = nullptr;

  // Wait for streaming thread to finish
  if (streaming_thread && streaming_thread->joinable()) {
    streaming_cv.notify_all();
    streaming_thread->join();
    streaming_thread = nullptr;
    std::cout << "Stopped streaming thread" << std::endl;
  }
}

// Pipeline configuration functions
std::string buildPipelineString(const CameraRequestData &config,
                                bool preview_enabled) {
  std::string pipeline_str =
      "appsrc name=mysource is-live=true do-timestamp=true format=time "
      "block=false max-bytes=0 ";

  // Use configuration parameters for caps
  pipeline_str +=
      "caps=video/x-raw,format=BGRA,width=" + std::to_string(config.width) +
      ",height=" + std::to_string(config.height) +
      ",framerate=" + std::to_string(config.fps) + "/1 ! ";

  pipeline_str +=
      "queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 "
      "leaky=downstream ! ";

  if (legacy_pipeline_mode.load()) {
    pipeline_str += "videoconvert ! ";
  }

  pipeline_str += "nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! "
                  "tee name=t ";

  // Configure encoder based on settings
  std::string encoder = config.enableMvHevc ? "nvv4l2h265enc" : "nvv4l2h264enc";
  std::string parser = config.enableMvHevc ? "h265parse" : "h264parse";

  pipeline_str +=
      "t. ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 "
      "leaky=downstream ! " +
      encoder + " name=encoder ! " + parser +
      " name=parser ! appsink name=mysink emit-signals=true sync=false "
      "max-buffers=1 drop=true ";

  if (preview_enabled) {
    pipeline_str +=
        "t. ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 "
        "leaky=downstream ! nvvidconv ! videoconvert ! "
        "autovideosink sync=false ";
  }

  return pipeline_str;
}

void updateZedConfiguration(sl::Camera &zed, const CameraRequestData &config) {
  // Map resolution
  sl::RESOLUTION resolution = sl::RESOLUTION::HD720; // default
  if (config.width == 1920 && config.height == 1080) {
    resolution = sl::RESOLUTION::HD1080;
  } else if (config.width == 1280 && config.height == 720) {
    resolution = sl::RESOLUTION::HD720;
  } else if (config.width == 2208 && config.height == 1242) {
    resolution = sl::RESOLUTION::HD2K;
  }

  // Note: In a real implementation, you might need to restart the camera
  // with new parameters. For now, this is informational.
  std::cout << "Camera would be configured with:" << std::endl;
  std::cout << "  Resolution: " << config.width << "x" << config.height
            << std::endl;
  std::cout << "  FPS: " << config.fps << std::endl;
  std::cout << "  Bitrate: " << config.bitrate << std::endl;
  std::cout << "  HEVC: " << (config.enableMvHevc ? "enabled" : "disabled")
            << std::endl;
}

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);
  signal(SIGINT, handle_sigint);

  // Parse command line arguments
  bool preview_enabled_local = false;
  bool listen_enabled = false;
  bool send_enabled_mode = false;
  bool zmq_enabled_mode = false;
  std::string listen_address = "";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--preview") {
      preview_enabled_local = true;
    } else if (arg == "--listen" && i + 1 < argc) {
      listen_enabled = true;
      listen_address = argv[++i];
    } else if (arg == "--send") {
      send_enabled_mode = true;
    } else if (arg == "--server" && i + 1 < argc) {
      send_to_server = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      send_to_port = std::stoi(argv[++i]);
    } else if (arg == "--zmq" && i + 1 < argc) {
      zmq_enabled_mode = true;
      zmq_endpoint = argv[++i];
    } else if (arg == "--zmq-raw" && i + 1 < argc) {
      zmq_enabled_mode = true;
      zmq_endpoint = argv[++i];
      zmq_raw_mode.store(true);
    } else if (arg == "--view" && i + 1 < argc) {
      std::string view = toLowerCopy(argv[++i]);
      if (view == "sbs" || view == "side-by-side" || view == "side_by_side") {
        zed_view_mode = "sbs";
      } else if (view == "left" || view == "mono") {
        zed_view_mode = "left";
      } else {
        std::cerr << "Invalid --view value: " << view
                  << " (expected sbs or left)" << std::endl;
        return -1;
      }
    } else if (arg == "--codec" && i + 1 < argc) {
      std::string codec = toLowerCopy(argv[++i]);
      if (codec == "auto" || codec == "h264" || codec == "h265") {
        codec_override = codec;
      } else {
        std::cerr << "Invalid --codec value: " << codec
                  << " (expected auto, h264, or h265)" << std::endl;
        return -1;
      }
    } else if (arg == "--no-low-latency") {
      low_latency_mode.store(false);
    } else if (arg == "--legacy-pipeline") {
      legacy_pipeline_mode.store(true);
    } else if (arg == "--measure-encoder-latency") {
      measure_encoder_latency.store(true);
    } else if (arg == "--idr-interval" && i + 1 < argc) {
      idr_interval = std::max(1, std::stoi(argv[++i]));
    } else if (arg == "--stats-interval" && i + 1 < argc) {
      stats_interval = std::max(0, std::stoi(argv[++i]));
    } else if (arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [options]\n";
      std::cout << "Options:\n";
      std::cout << "  --preview      Enable video preview\n";
      std::cout << "  --listen ADDR  Listen to control commands on address "
                   "(IP:PORT)\n";
      std::cout << "  --send         Send video stream directly to server\n";
      std::cout << "  --server IP    Server IP address\n";
      std::cout << "  --port PORT    Server port\n";
      std::cout << "  --zmq ENDPOINT     ZMQ publish endpoint for H.264 stream (e.g. tcp://*:5555)\n";
      std::cout << "  --zmq-raw ENDPOINT ZMQ publish endpoint for raw images (e.g. tcp://*:5556)\n";
      std::cout << "  --view MODE        ZED view: sbs (default) or left\n";
      std::cout << "  --codec MODE       Encoder: auto (default), h264, or h265\n";
      std::cout << "  --no-low-latency   Disable Jetson low-latency encoder settings\n";
      std::cout << "  --legacy-pipeline  Use CPU videoconvert before nvvidconv fallback\n";
      std::cout << "  --idr-interval N   IDR/I-frame interval for low-latency mode (default: 15)\n";
      std::cout << "  --stats-interval N Log timing every N grabbed frames (default: 120, 0 disables)\n";
      std::cout << "  --measure-encoder-latency Enable nvv4l2 MeasureEncoderLatency if available\n";
      std::cout << "  --help             Show this help message\n";
      std::cout << "Examples:\n";
      std::cout << "  " << argv[0] << " --send --server 192.168.1.100 --port 8080 --zmq tcp://*:5555\n";
      std::cout << "  " << argv[0] << " --listen 0.0.0.0:9090 --zmq tcp://*:5555\n";
      std::cout << "  " << argv[0] << " --listen 0.0.0.0:9090 --zmq-raw tcp://*:5556  # Low latency raw images\n";
      return 0;
    }
  }

  if (!listen_enabled && !send_enabled_mode) {
    std::cerr << "Error: Either --listen or --send option is required"
              << std::endl;
    std::cerr << "Use --help to see usage options" << std::endl;
    return -1;
  }

  if (send_enabled_mode && (send_to_server.empty() || send_to_port == 0) && !zmq_enabled_mode) {
    std::cerr << "Error: --send mode requires either TCP (--server and --port) or ZMQ (--zmq) options"
              << std::endl;
    std::cerr << "Use --help to see usage options" << std::endl;
    return -1;
  }

  // Initialize ZMQ if requested
  if (zmq_enabled_mode) {
    if (initialize_zmq()) {
      zmq_enabled.store(true);
      std::cout << "ZMQ publisher initialized on " << zmq_endpoint << std::endl;
    } else {
      std::cerr << "Failed to initialize ZMQ, continuing without ZMQ support" << std::endl;
    }
  }

  if (send_enabled_mode) {
    std::cout << "Starting direct video streaming to " << send_to_server << ":"
              << send_to_port;
    if (zmq_enabled.load()) {
      std::cout << " and ZMQ " << zmq_endpoint;
    }
    std::cout << "..." << std::endl;

    // Set global preview flag
    preview_enabled.store(preview_enabled_local);

    // Set up default camera configuration for direct streaming
    {
      std::lock_guard<std::mutex> lock(config_mutex);
      current_camera_config.width = 2560;
      current_camera_config.height = 720;
      current_camera_config.fps = 60;
      current_camera_config.bitrate = 4000000;
      current_camera_config.enableMvHevc = 0;
      current_camera_config.renderMode = 0;
      current_camera_config.camera = "ZED";
      current_camera_config.ip = send_to_server;
      current_camera_config.port = send_to_port;
      applyCodecOverride(current_camera_config);
    }

    // Start streaming directly
    startStreamingThread();

    // Main thread waits for termination signal
    std::cout << "Streaming started. Press Ctrl+C to stop." << std::endl;

    while (!stop_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  } else if (listen_enabled) {
    std::cout << "Starting threaded video streaming server...";
    if (zmq_enabled.load()) {
      std::cout << " with ZMQ support on " << zmq_endpoint;
    }
    std::cout << std::endl;

    // Set global preview flag
    preview_enabled.store(preview_enabled_local);

    // Start listening thread
    listen_thread =
        make_unique_helper<std::thread>(listenThreadFunction, listen_address);

    // Main thread waits for termination signal
    std::cout << "Server started. Press Ctrl+C to stop." << std::endl;

    while (!stop_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Stop listening thread
    if (listen_thread && listen_thread->joinable()) {
      listen_thread->join();
    }
  }

  std::cout << "Shutting down..." << std::endl;

  // Stop streaming thread first
  stopStreamingThread();

  // Cleanup ZMQ
  cleanup_zmq();

  std::cout << "All threads stopped. Exiting." << std::endl;
  return 0;
}

void streamingThreadFunction() {
  std::cout << "Streaming thread started" << std::endl;

  try {
    // Initialize sender (optional for TCP)
    bool tcp_initialized = false;
    if (!send_to_server.empty() && send_to_port > 0) {
      tcp_initialized = initialize_sender();
      if (!tcp_initialized) {
        std::cerr << "Failed to initialize TCP sender, continuing with ZMQ only" << std::endl;
      }
    }

    // Enable streaming flags
    encoding_enabled.store(true);
    if (tcp_initialized) {
      send_enabled.store(true);
    }
    if (!zmq_endpoint.empty() && zmq_publisher) {
      zmq_enabled.store(true);
    }

    // Check if at least one output method is available
    if (!send_enabled.load() && !zmq_enabled.load()) {
      std::cerr << "No output method available (neither TCP nor ZMQ), stopping" << std::endl;
      return;
    }

    resetStreamStats();

    // Initialize ZED camera
    sl::Camera zed;
    sl::InitParameters init_params;

    // Get camera configuration safely
    CameraRequestData config;
    {
      std::lock_guard<std::mutex> lock(config_mutex);
      config = current_camera_config;
    }

    // Configure ZED based on received config
    if (config.width > 0 && config.height > 0) {
      if ((config.width == 3840 || config.width == 1920) &&
          config.height == 1080) {
        init_params.camera_resolution = sl::RESOLUTION::HD1080;
      } else if ((config.width == 2560 || config.width == 1280) &&
                 config.height == 720) {
        init_params.camera_resolution = sl::RESOLUTION::HD720;
      } else if ((config.width == 4416 || config.width == 2208) &&
                 config.height == 1242) {
        init_params.camera_resolution = sl::RESOLUTION::HD2K;
      } else {
        init_params.camera_resolution = sl::RESOLUTION::HD720; // default
      }

      if (config.fps > 0) {
        init_params.camera_fps = config.fps;
      } else {
        init_params.camera_fps = 60; // default
      }
    } else {
      // Use default values if no config
      init_params.camera_resolution = sl::RESOLUTION::HD720;
      init_params.camera_fps = 60;
    }

    if (zed.open(init_params) != sl::ERROR_CODE::SUCCESS) {
      std::cerr << "Failed to open ZED camera in streaming thread" << std::endl;
      return;
    }

    sl::VIEW zed_view = selectedZedView();
    std::cout << "ZED retrieve view: " << selectedZedViewName() << std::endl;

    // Grab once before building the pipeline so caps match the actual ZED view.
    sl::Mat zed_image;
    cv::Mat cv_image;
    bool have_initial_frame = false;
    for (int attempt = 0; attempt < 120 && streaming_active.load() &&
                            !stop_requested.load();
         ++attempt) {
      SteadyClock::time_point grab_start = SteadyClock::now();
      sl::ERROR_CODE grab_status = zed.grab();
      SteadyClock::time_point grab_end = SteadyClock::now();
      if (grab_status == sl::ERROR_CODE::SUCCESS) {
        recordGrabTiming(elapsedMs(grab_start, grab_end));

        SteadyClock::time_point retrieve_start = SteadyClock::now();
        zed.retrieveImage(zed_image, zed_view);
        cv_image = slMat2cvMat(zed_image);
        recordRetrieveTiming(elapsedMs(retrieve_start, SteadyClock::now()));

        if (cv_image.data && cv_image.cols > 0 && cv_image.rows > 0) {
          have_initial_frame = true;
          break;
        }
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    if (!have_initial_frame) {
      std::cerr << "Failed to retrieve initial ZED frame" << std::endl;
      zed.close();
      return;
    }

    CameraRequestData pipeline_config = config;
    pipeline_config.width = cv_image.cols;
    pipeline_config.height = cv_image.rows;
    pipeline_config.fps = pipeline_config.fps > 0 ? pipeline_config.fps : 60;
    pipeline_config.bitrate =
        pipeline_config.bitrate > 0 ? pipeline_config.bitrate : 4000000;
    applyCodecOverride(pipeline_config);

    std::cout << "Effective stream: " << pipeline_config.width << "x"
              << pipeline_config.height << "@" << pipeline_config.fps
              << " view=" << selectedZedViewName()
              << " codec=" << (pipeline_config.enableMvHevc ? "H265" : "H264")
              << " bitrate=" << pipeline_config.bitrate
              << " low_latency=" << (low_latency_mode.load() ? "on" : "off")
              << " legacy_pipeline="
              << (legacy_pipeline_mode.load() ? "on" : "off") << std::endl;

    std::string pipeline_str =
        buildPipelineString(pipeline_config, preview_enabled.load());
    std::cout << "GStreamer pipeline: " << pipeline_str << std::endl;

    // Launch pipeline
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
    if (!pipeline) {
      std::cerr << "Failed to create pipeline in streaming thread: "
                << error->message << std::endl;
      g_clear_error(&error);
      zed.close();
      return;
    }

    // Bind appsrc/appsink
    GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "mysource");
    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "mysink");
    GstElement *encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
    GstElement *parser = gst_bin_get_by_name(GST_BIN(pipeline), "parser");

    if (!appsrc || !appsink || !encoder || !parser) {
      std::cerr << "Failed to bind required GStreamer elements" << std::endl;
      if (appsrc)
        gst_object_unref(appsrc);
      if (appsink)
        gst_object_unref(appsink);
      if (encoder)
        gst_object_unref(encoder);
      if (parser)
        gst_object_unref(parser);
      gst_object_unref(pipeline);
      zed.close();
      return;
    }

    configureEncoder(encoder, pipeline_config);
    configureParser(parser);
    gst_object_unref(encoder);
    gst_object_unref(parser);

    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), nullptr);
    GstStateChangeReturn state_ret =
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
      std::cerr << "GStreamer pipeline failed to enter PLAYING state" << std::endl;
      gst_object_unref(appsrc);
      gst_object_unref(appsink);
      gst_object_unref(pipeline);
      zed.close();
      return;
    }
    uint64_t frame_id = 0;

    std::cout << "Starting streaming loop..." << std::endl;
    while (streaming_active.load() && !stop_requested.load()) {
      SteadyClock::time_point grab_start = SteadyClock::now();
      sl::ERROR_CODE grab_status = zed.grab();
      SteadyClock::time_point grab_end = SteadyClock::now();
      if (grab_status == sl::ERROR_CODE::SUCCESS) {
        recordGrabTiming(elapsedMs(grab_start, grab_end));

        SteadyClock::time_point retrieve_start = SteadyClock::now();
        zed.retrieveImage(zed_image, zed_view);
        cv_image = slMat2cvMat(zed_image);
        recordRetrieveTiming(elapsedMs(retrieve_start, SteadyClock::now()));

        if (!cv_image.data || cv_image.cols != pipeline_config.width ||
            cv_image.rows != pipeline_config.height) {
          std::cerr << "Skipping unexpected ZED frame shape: "
                    << cv_image.cols << "x" << cv_image.rows << " expected "
                    << pipeline_config.width << "x" << pipeline_config.height
                    << std::endl;
          continue;
        }

        // 直接通过ZMQ发送原始图像
        if (zmq_raw_mode.load() && zmq_enabled.load() && zmq_publisher &&
            cv_image.data && cv_image.total() > 0) {
          try {
            SteadyClock::time_point zmq_start = SteadyClock::now();
            std::lock_guard<std::mutex> lock(zmq_mutex);
            
            // 创建消息：[4字节宽度][4字节高度][4字节通道数][图像数据]
            size_t data_size = cv_image.total() * cv_image.elemSize();
            std::vector<uint8_t> zmq_packet(12 + data_size);
            
            // 写入图像信息
            int width = cv_image.cols;
            int height = cv_image.rows;
            int channels = cv_image.channels();
            
            std::memcpy(&zmq_packet[0], &width, 4);
            std::memcpy(&zmq_packet[4], &height, 4);
            std::memcpy(&zmq_packet[8], &channels, 4);
            
            // 写入图像数据
            std::memcpy(&zmq_packet[12], cv_image.data, data_size);

            // 通过ZMQ发送 (非阻塞)
            int rc = zmq_send(zmq_publisher, zmq_packet.data(), zmq_packet.size(), ZMQ_DONTWAIT);
            if (rc == -1) {
              if (zmq_errno() != EAGAIN) {
                std::cerr << "ZMQ send error: " << zmq_strerror(zmq_errno()) << std::endl;
              }
              recordRawZmqTiming(0.0, false);
            } else {
              recordRawZmqTiming(elapsedMs(zmq_start, SteadyClock::now()), true);
            }
          } catch (const std::exception &e) {
            std::cerr << "Unexpected error in ZMQ raw image send: " << e.what() << std::endl;
            recordRawZmqTiming(0.0, false);
          }
        }

        if (encoding_enabled.load()) {
          uint64_t current_frame_id = static_cast<uint64_t>(frame_id);

          SteadyClock::time_point appsrc_start = SteadyClock::now();
          GstBuffer *buffer = gst_buffer_new_allocate(
              nullptr, cv_image.total() * cv_image.elemSize(), nullptr);
          GstMapInfo map;
          if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            std::cerr << "Failed to allocate/map GstBuffer" << std::endl;
            if (buffer) {
              gst_buffer_unref(buffer);
            }
            continue;
          }
          std::memcpy(map.data, cv_image.data,
                      cv_image.total() * cv_image.elemSize());
          gst_buffer_unmap(buffer, &map);

          GST_BUFFER_PTS(buffer) =
              gst_util_uint64_scale(frame_id, GST_SECOND, pipeline_config.fps);
          GST_BUFFER_DURATION(buffer) =
              gst_util_uint64_scale(1, GST_SECOND, pipeline_config.fps);
          GST_BUFFER_OFFSET(buffer) = current_frame_id;
          GstFlowReturn push_status =
              gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
          if (push_status != GST_FLOW_OK) {
            std::cerr << "appsrc push failed: " << push_status << std::endl;
            streaming_active.store(false);
            break;
          }
          recordAppsrcTiming(current_frame_id,
                             elapsedMs(appsrc_start, SteadyClock::now()));

          frame_id++;
        }
        maybeLogStreamStats();
      }
    }

    std::cout << "Streaming loop ended, cleaning up..." << std::endl;

    // Clean shutdown
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(appsrc);
    gst_object_unref(appsink);
    gst_object_unref(pipeline);
    zed.close();

  } catch (const std::exception &e) {
    std::cerr << "Streaming thread error: " << e.what() << std::endl;
  }

  std::cout << "Streaming thread finished" << std::endl;
}
