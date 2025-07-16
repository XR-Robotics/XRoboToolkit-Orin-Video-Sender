#include <chrono>
#include <csignal>
#include <cstring>
#include <glib-unix.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <iomanip>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <openssl/md5.h>
#include <sl/Camera.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

// Handler function declarations
void handleOpenCamera(const std::vector<uint8_t> &data);
void handleCloseCamera(const std::vector<uint8_t> &data);

// Pipeline configuration functions
std::string buildPipelineString(const CameraRequestData &config,
                                bool preview_enabled) {
  std::string pipeline_str = "appsrc name=mysource is-live=true format=time ";

  // Use configuration parameters for caps
  pipeline_str +=
      "caps=video/x-raw,format=BGRA,width=" + std::to_string(config.width) +
      ",height=" + std::to_string(config.height) +
      ",framerate=" + std::to_string(config.fps) + "/1 ! ";

  pipeline_str += "videoconvert ! nvvidconv ! "
                  "video/x-raw(memory:NVMM),format=NV12 ! tee name=t ";

  // Configure encoder based on settings
  std::string encoder = config.enableMvHevc ? "nvv4l2h265enc" : "nvv4l2h264enc";
  std::string parser = config.enableMvHevc ? "h265parse" : "h264parse";

  pipeline_str +=
      "t. ! queue ! " + encoder + " maxperf-enable=1 insert-sps-pps=true ";
  pipeline_str +=
      "idrinterval=15 bitrate=" + std::to_string(config.bitrate) + " ! ";
  pipeline_str +=
      parser + " ! appsink name=mysink emit-signals=true sync=false ";

  if (preview_enabled) {
    pipeline_str +=
        "t. ! queue ! nvvidconv ! videoconvert ! autovideosink sync=false ";
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

std::unique_ptr<TCPClient> sender_ptr;
volatile sig_atomic_t stop_requested = 0;
bool send_enabled = false; // <-- Global flag for sending

// client for sending
std::string send_to_server = "";
int send_to_port = 0;

// server for listening to control command
std::unique_ptr<TCPServer> server_ptr; // Pointer to TCPServer
bool encoding_enabled = false;         // Flag to control encoding and sending

bool initialize_sender() {
  int retry = 10;
  while (retry > 0 && !sender_ptr) {
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

std::string inline printTimeMs(std::string tag, bool force_print = true) {
  auto now = std::chrono::system_clock::now();
  // point the current time in milliseconds
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch())
                    .count();

  std::stringstream ss;
  ss << "===LATENCY TEST===[" << tag.c_str() << "]\t" << now_ms << " ms\n";
  std::string formatted_string = ss.str();

  if (force_print) {
    std::cout << formatted_string;
  }
  return formatted_string;
}

void onDataCallback(const std::string &command) {
  // Handle legacy string commands for backward compatibility
  if (command == "StartRobotCameraStream") {
    // initialize the sender if needed
    if (initialize_sender()) {
      // enable encoding and sending
      encoding_enabled = true;
      send_enabled = true;
    } else {
      // failed to intialized sender, quit
      stop_requested = 1;
      return;
    }
    std::cout << "Started encoding and sending data" << std::endl;
  } else if (command == "StopRobotCameraStream") {
    encoding_enabled = false;
    send_enabled = false;
    std::cout << "Stopped encoding and sending data" << std::endl;
  } else if (command.substr(0, 8) == "LOOPTEST") {
    printTimeMs("Loop Receive");
  } else if (command.substr(0, 12) == "MediaDecoder") {
    printTimeMs("Java - " + command);
  } else {
    // Convert string to binary data for protocol parsing
    std::vector<uint8_t> binaryData(command.begin(), command.end());

    // Debug: Print the raw bytes received
    std::cout << "Raw TCP data (" << binaryData.size() << " bytes): ";
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

    std::cout << "Packet body length: " << bodyLength << std::endl;

    if (4 + bodyLength > binaryData.size()) {
      std::cerr << "Data too small for declared body length. Expected: "
                << (4 + bodyLength) << ", got: " << binaryData.size()
                << std::endl;
      return;
    }

    // Extract the actual protocol data (skip the 4-byte header)
    std::vector<uint8_t> protocolData(binaryData.begin() + 4,
                                      binaryData.begin() + 4 + bodyLength);

    // Debug: Print the extracted protocol data
    std::cout << "Protocol data (" << protocolData.size() << " bytes): ";
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
                << "' (length: " << protocol.command.length() << ")"
                << std::endl;

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

    // Try to parse as simple command format (command string followed by data)
    try {
      // Convert protocol data back to string for simple parsing
      std::string protocolString(protocolData.begin(), protocolData.end());

      // Check if it starts with a known command
      if (protocolString.substr(0, 11) == "OPEN_CAMERA") {
        std::cout << "Received OPEN_CAMERA command (simple format)"
                  << std::endl;
        // Extract data after the command string
        if (protocolString.size() > 11) {
          std::vector<uint8_t> cameraData(protocolString.begin() + 11,
                                          protocolString.end());
          handleOpenCamera(cameraData);
        } else {
          std::cerr << "OPEN_CAMERA command has no data" << std::endl;
        }
      } else if (protocolString.substr(0, 12) == "CLOSE_CAMERA") {
        std::cout << "Received CLOSE_CAMERA command (simple format)"
                  << std::endl;
        // Extract data after the command string
        if (protocolString.size() > 12) {
          std::vector<uint8_t> cameraData(protocolString.begin() + 12,
                                          protocolString.end());
          handleCloseCamera(cameraData);
        } else {
          handleCloseCamera(std::vector<uint8_t>());
        }
      } else {
        // Unknown command format
        std::cerr << "Unknown protocol command received: ";
        // Print first 20 bytes as hex for debugging
        for (size_t i = 0; i < std::min(protocolData.size(), size_t(20)); ++i) {
          std::cerr << std::hex << std::setfill('0') << std::setw(2)
                    << static_cast<unsigned int>(protocolData[i]) << " ";
        }
        std::cerr << std::endl;
      }
    } catch (const std::exception &e) {
      std::cerr << "Error processing protocol data: " << e.what() << std::endl;
    }
  }
}
///

void onDisconnectCallback() {
  // release the send thread
  std::cout << "onDisconnectCallback" << std::endl;
  sender_ptr->~TCPClient();
  sender_ptr = nullptr;
}

void handle_sigint(int) {
  std::cout << "\nSIGINT received. Stopping ..." << std::endl;
  if (server_ptr) {
    server_ptr->stop(); // Gracefully stop the server
    server_ptr = nullptr;
  }
  if (sender_ptr) {
    sender_ptr->disconnect();
    server_ptr = nullptr;
  }
  stop_requested = 1;
}

// Handler function implementations
void handleOpenCamera(const std::vector<uint8_t> &data) {
  try {
    current_camera_config = CameraRequestDeserializer::deserialize(data);

    std::cout << "OpenCameraHandler - Received camera config:" << std::endl;
    std::cout << "  Resolution: " << current_camera_config.width << "x"
              << current_camera_config.height << std::endl;
    std::cout << "  FPS: " << current_camera_config.fps << std::endl;
    std::cout << "  Bitrate: " << current_camera_config.bitrate << std::endl;
    std::cout << "  HEVC: "
              << (current_camera_config.enableMvHevc ? "enabled" : "disabled")
              << std::endl;
    std::cout << "  RenderMode: " << current_camera_config.renderMode
              << std::endl;
    std::cout << "  Camera: " << current_camera_config.camera << std::endl;
    std::cout << "  Target: " << current_camera_config.ip << ":"
              << current_camera_config.port << std::endl;

    // Update global send configuration
    send_to_server = current_camera_config.ip;
    send_to_port = current_camera_config.port;

    // Initialize sender if needed
    if (initialize_sender()) {
      encoding_enabled = true;
      send_enabled = true;
      std::cout << "Camera stream started successfully" << std::endl;
      std::cout << "Note: Dynamic pipeline reconfiguration requires restart "
                   "for full effect"
                << std::endl;
    } else {
      std::cerr << "Failed to initialize sender for camera stream" << std::endl;
      stop_requested = 1;
    }

  } catch (const std::exception &e) {
    std::cerr << "Error parsing camera request: " << e.what() << std::endl;
  }
}

void handleCloseCamera(const std::vector<uint8_t> &data) {
  std::cout << "CloseCameraHandler - Stopping camera stream" << std::endl;

  // Stop encoding and sending
  encoding_enabled = false;
  send_enabled = false;

  // Disconnect sender if connected
  if (sender_ptr && sender_ptr->isConnected()) {
    sender_ptr->disconnect();
    std::cout << "Camera stream stopped and disconnected" << std::endl;
  }
}

void printErrorAndQuit(const std::string &error_msg) {
  std::cerr << "Error: " << error_msg << std::endl;
  stop_requested = 1;
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
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    printTimeMs("After Encoding");
    const uint8_t *data = map.data;
    gsize size = map.size;
    if (send_enabled && sender_ptr && sender_ptr->isConnected() && data &&
        size > 0) {
      try {
        std::vector<uint8_t> packet(4 + size);
        packet[0] = (size >> 24) & 0xFF;
        packet[1] = (size >> 16) & 0xFF;
        packet[2] = (size >> 8) & 0xFF;
        packet[3] = (size)&0xFF;
        std::copy(data, data + size, packet.begin() + 4);

        // calculate the md5 checksum
        unsigned char md5sum[16]; // 16 bytes for MD5 checksum
        MD5(data, size, md5sum);  // Calculate MD5 checksum
        // Convert to hex string for printing
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < 16; ++i) {
          ss << std::setw(2) << static_cast<int>(md5sum[i]);
        }
        std::cout << "MD5: " << ss.str() << std::endl;

        printTimeMs("Before Send");
        sender_ptr->sendData(packet);
        printTimeMs("After Send");
        std::cout << "Sent " << size << " bytes of H.264 data" << std::endl;
      } catch (const TCPException &e) {
        printErrorAndQuit(e.what());
      } catch (const std::exception &e) {
        printErrorAndQuit("Unexpected error during sendData: " +
                          std::string(e.what()));
      }
    }

    gst_buffer_unmap(buffer, &map);
  }

  if (buffer) {
    GstClockTime timestamp = GST_BUFFER_PTS(buffer);
    std::cout << "Encoded frame at timestamp: "
              << GST_TIME_AS_MSECONDS(timestamp) << " ms" << std::endl;
  }

  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

int main(int argc, char *argv[]) {
  printTimeMs("Start");
  gst_init(&argc, &argv);
  signal(SIGINT, handle_sigint);

  // Check for --preview flag
  bool preview_enabled = false;
  std::string server_ip = "127.0.0.1";
  int server_port = 12345;

  bool listen_enabled = false;
  std::string listen_address = "";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--preview") {
      preview_enabled = true;
    } else if (arg == "--send") {
      send_enabled = true;
      encoding_enabled = true;
    } else if (arg == "--listen" && i + 1 < argc) {
      listen_enabled = true;
      listen_address = argv[++i];
    } else if (arg == "--server" && i + 1 < argc) {
      server_ip = argv[++i];
      // cache the ip
      send_to_server = std::string(server_ip);
    } else if (arg == "--port" && i + 1 < argc) {
      server_port = std::stoi(argv[++i]);
      // cache the port
      send_to_port = server_port;
    } else if (arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [options]\n";
      std::cout << "Options:\n";
      std::cout << "  --preview      Enable video preview\n";
      std::cout << "  --listen       Listen to control commands\n";
      std::cout << "  --send         Enable sending encoded video over TCP\n";
      std::cout << "  --server IP    Server IP address (default: 127.0.0.1)\n";
      std::cout << "  --port PORT    Server port (default: 12345)\n";
      std::cout << "  --help         Show this help message\n";
      return 0;
    }
  }

  // Correct logic: disable sending when listen is enabled
  // as the sending is controlled by the remote side (headset)
  if (listen_enabled) {
    send_enabled = false;
    encoding_enabled = false;
  }

  if (listen_enabled) {
    // Initialize TCPServer
    server_ptr = std::unique_ptr<TCPServer>(new TCPServer(listen_address));
    server_ptr->setDataCallback(onDataCallback);
    server_ptr->setDisconnectCallback(onDisconnectCallback);
    server_ptr->start();
    std::cout << "TCPServer is listening on " << listen_address << std::endl;
  }

  if (send_enabled && !initialize_sender()) {
    return -1;
  }

  // Initialize ZED
  sl::Camera zed;
  sl::InitParameters init_params;
  init_params.camera_resolution = sl::RESOLUTION::HD720;
  init_params.camera_fps = 60;
  if (zed.open(init_params) != sl::ERROR_CODE::SUCCESS) {
    std::cerr << "Failed to open ZED camera\n";
    return -1;
  }

  // Construct pipeline string based on preview flag
  // `insert-sps-pps=true idrinterval=15` are necessary for the headset decoding
  // insert-sps-pps=true: Makes sure every keyframe (IDR) is preceded by SPS/PPS
  // — necessary for streaming and decoding startup. idrinterval=15: Forces a
  // keyframe every 15 frames (adjust as needed).

  std::string pipeline_str =
      "appsrc name=mysource is-live=true format=time "
      "caps=video/x-raw,format=BGRA,width=2560,height=720,framerate=60/1 ! "
      "videoconvert ! nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! "
      "tee name=t "
      "t. ! queue ! nvv4l2h264enc maxperf-enable=1 insert-sps-pps=true "
      "idrinterval=15 bitrate=4000000 ! h264parse ! appsink name=mysink "
      "emit-signals=true sync=false ";

  if (preview_enabled) {
    pipeline_str += "t. ! queue ! "
                    "nvvidconv ! videoconvert ! "
                    "autovideosink sync=false ";
    encoding_enabled = true;
  }

  // Launch pipeline
  GError *error = nullptr;
  GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
  if (!pipeline) {
    std::cerr << "Failed to create pipeline: " << error->message << std::endl;
    g_clear_error(&error);
    return -1;
  }

  // Bind appsrc/appsink
  GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "mysource");
  GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "mysink");

  g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), nullptr);

  gst_element_set_state(pipeline, GST_STATE_PLAYING);

  sl::Mat zed_image;
  int frame_id = 0;

  while (!stop_requested) {
    if (zed.grab() == sl::ERROR_CODE::SUCCESS) {
      auto grabbed = printTimeMs("Grabbed", false);
      zed.retrieveImage(zed_image, sl::VIEW::SIDE_BY_SIDE);
      cv::Mat cv_image =
          slMat2cvMat(zed_image); // ZED OpenCV conversion -> BGRA

      auto befored = printTimeMs("Before Encoding", false);
      if (encoding_enabled) {
        printf(grabbed.c_str());
        printf(befored.c_str());
        GstBuffer *buffer = gst_buffer_new_allocate(
            nullptr, cv_image.total() * cv_image.elemSize(), nullptr);
        GstMapInfo map;
        gst_buffer_map(buffer, &map, GST_MAP_WRITE);
        memcpy(map.data, cv_image.data, cv_image.total() * cv_image.elemSize());
        gst_buffer_unmap(buffer, &map);

        GST_BUFFER_PTS(buffer) =
            gst_util_uint64_scale(frame_id, GST_SECOND, 60);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, 60);
        gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);

        frame_id++;
      }
    }
  }

  if (send_enabled && sender_ptr) {
    sender_ptr->disconnect();
    std::cout << "Disconnected from server" << std::endl;
  }

  // close the server if it was created
  if (listen_enabled && server_ptr) {
    server_ptr->stop();
    std::cout << "TCPServer stopped" << std::endl;
  }

  // Clean shutdown
  gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(appsrc);
  gst_object_unref(appsink);
  gst_object_unref(pipeline);
  zed.close();

  return 0;
}