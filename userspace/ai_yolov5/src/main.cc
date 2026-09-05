#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <condition_variable>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define _BASETSD_H

#include "RgaUtils.h"
#include "im2d.h"
#include "opencv2/core/core.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"
#include "opencv2/videoio.hpp"
#include "postprocess.h"
#include "rga.h"
#include "rk_mpi.h"
#include "rknn_api.h"

static volatile sig_atomic_t g_stop = 0;

static void handle_sigint(int)
{
  g_stop = 1;
}

struct CaptureFrame
{
  cv::Mat bgr;
  std::chrono::steady_clock::time_point captured_at;
  uint64_t sequence;
};

struct OverlayBox
{
  float left_norm = 0.0f;
  float top_norm = 0.0f;
  float right_norm = 0.0f;
  float bottom_norm = 0.0f;
  float confidence = 0.0f;
  std::string label;
};

struct OverlaySnapshot
{
  bool valid = false;
  float roi_x_min = 0.0f;
  float roi_y_min = 0.0f;
  float roi_x_max = 0.0f;
  float roi_y_max = 0.0f;
  std::vector<OverlayBox> boxes;
};

/* The inference thread writes a compact result snapshot; the encoder only reads copies. */
class LatestOverlay {
 public:
  void update(float roi_x_min, float roi_y_min, float roi_x_max, float roi_y_max,
              const std::vector<OverlayBox>& boxes) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.valid = true;
    snapshot_.roi_x_min = roi_x_min;
    snapshot_.roi_y_min = roi_y_min;
    snapshot_.roi_x_max = roi_x_max;
    snapshot_.roi_y_max = roi_y_max;
    snapshot_.boxes = boxes;
  }

  OverlaySnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
  }

 private:
  mutable std::mutex mutex_;
  OverlaySnapshot snapshot_;
};

struct H264AccessUnit
{
  uint64_t sequence = 0;
  std::chrono::steady_clock::time_point captured_at;
  std::vector<unsigned char> bytes;
  bool has_extra_data = false;
  bool is_idr = false;
};

static void inspect_h264_annexb(const unsigned char* data, size_t length,
                                bool* has_extra_data, bool* has_idr)
{
  if (data == NULL || has_extra_data == NULL || has_idr == NULL) {
    return;
  }
  for (size_t i = 0; i + 3 <= length;) {
    size_t start_code_size = 0;
    if (i + 3 <= length && data[i] == 0x00 && data[i + 1] == 0x00 &&
        data[i + 2] == 0x01) {
      start_code_size = 3;
    } else if (i + 4 <= length && data[i] == 0x00 && data[i + 1] == 0x00 &&
               data[i + 2] == 0x00 && data[i + 3] == 0x01) {
      start_code_size = 4;
    }
    if (start_code_size == 0) {
      ++i;
      continue;
    }
    const size_t nal_offset = i + start_code_size;
    if (nal_offset < length && (data[nal_offset] & 0x1f) == 5) {
      *has_idr = true;
    }
    if (nal_offset < length && ((data[nal_offset] & 0x1f) == 7 ||
                                (data[nal_offset] & 0x1f) == 8)) {
      *has_extra_data = true;
    }
    i = nal_offset;
  }
}

static void cache_h264_parameter_sets(const unsigned char* data, size_t length,
                                      std::vector<unsigned char>* parameter_sets)
{
  if (data == NULL || parameter_sets == NULL) {
    return;
  }

  std::vector<unsigned char> updated;
  for (size_t i = 0; i + 3 <= length;) {
    size_t start_code_size = 0;
    if (i + 3 <= length && data[i] == 0x00 && data[i + 1] == 0x00 &&
        data[i + 2] == 0x01) {
      start_code_size = 3;
    } else if (i + 4 <= length && data[i] == 0x00 && data[i + 1] == 0x00 &&
               data[i + 2] == 0x00 && data[i + 3] == 0x01) {
      start_code_size = 4;
    }
    if (start_code_size == 0) {
      ++i;
      continue;
    }

    size_t next = length;
    for (size_t candidate = i + start_code_size; candidate + 3 <= length; ++candidate) {
      if (data[candidate] == 0x00 && data[candidate + 1] == 0x00 &&
          (data[candidate + 2] == 0x01 ||
           (candidate + 3 < length && data[candidate + 2] == 0x00 &&
            data[candidate + 3] == 0x01))) {
        next = candidate;
        break;
      }
    }
    const size_t nal_offset = i + start_code_size;
    if (nal_offset < length) {
      const unsigned char nal_type = data[nal_offset] & 0x1f;
      if (nal_type == 7 || nal_type == 8) {
        updated.insert(updated.end(), data + i, data + next);
      }
    }
    i = next;
  }
  if (!updated.empty()) {
    parameter_sets->swap(updated);
  }
}

/* Nonblocking, single-client handoff for the local HMI; it never queues video. */
class H264AccessUnitPublisher {
 public:
  explicit H264AccessUnitPublisher(const char* socket_path)
      : socket_path_(socket_path), listen_fd_(-1), client_fd_(-1), client_needs_config_(true),
        client_needs_idr_(true) {}

  ~H264AccessUnitPublisher() {
    stop();
  }

  bool start() {
    if (listen_fd_ >= 0) {
      return true;
    }
    listen_fd_ = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (listen_fd_ < 0 || !set_nonblocking(listen_fd_)) {
      printf("h264 socket create failed path=%s error=%s\n", socket_path_.c_str(), strerror(errno));
      stop();
      return false;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(address.sun_path)) {
      printf("h264 socket path too long path=%s\n", socket_path_.c_str());
      stop();
      return false;
    }
    memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
    unlink(socket_path_.c_str());
    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listen_fd_, 1) != 0) {
      printf("h264 socket listen failed path=%s error=%s\n", socket_path_.c_str(), strerror(errno));
      stop();
      return false;
    }
    printf("h264 socket ready path=%s\n", socket_path_.c_str());
    return true;
  }

  void stop() {
    drop_client();
    if (listen_fd_ >= 0) {
      close(listen_fd_);
      listen_fd_ = -1;
    }
    if (!socket_path_.empty()) {
      unlink(socket_path_.c_str());
    }
  }

  void publish(const H264AccessUnit& access_unit) {
    if (access_unit.has_extra_data) {
      cache_h264_parameter_sets(access_unit.bytes.data(), access_unit.bytes.size(), &parameter_sets_);
    }
    accept_latest_client();
    if (client_fd_ < 0) {
      return;
    }
    if (client_needs_config_) {
      if (parameter_sets_.empty() ||
          !send_message(kMessageConfig, kFlagExtraData, access_unit.sequence,
                        access_unit.captured_at, parameter_sets_)) {
        return;
      }
      client_needs_config_ = false;
      client_needs_idr_ = true;
    }
    if (client_needs_idr_ && !access_unit.is_idr) {
      return;
    }
    uint32_t flags = access_unit.is_idr ? kFlagIdr : 0;
    if (access_unit.has_extra_data) {
      flags |= kFlagExtraData;
    }
    if (send_message(kMessageAccessUnit, flags, access_unit.sequence,
                     access_unit.captured_at, access_unit.bytes)) {
      client_needs_idr_ = false;
    }
  }

 private:
  struct H264SocketHeader {
    uint32_t magic;
    uint32_t type;
    uint32_t flags;
    uint32_t payload_size;
    uint64_t sequence;
    uint64_t captured_at_ms;
  };

  enum MessageType {
    kMessageConfig = 1,
    kMessageAccessUnit = 2,
  };

  enum MessageFlags {
    kFlagExtraData = 1U << 0,
    kFlagIdr = 1U << 1,
  };

  static bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
  }

  void drop_client() {
    if (client_fd_ >= 0) {
      close(client_fd_);
      client_fd_ = -1;
    }
    client_needs_config_ = true;
    client_needs_idr_ = true;
  }

  void accept_latest_client() {
    if (listen_fd_ < 0) {
      return;
    }
    for (;;) {
      const int fd = accept(listen_fd_, NULL, NULL);
      if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          printf("h264 socket accept failed error=%s\n", strerror(errno));
        }
        return;
      }
      if (!set_nonblocking(fd)) {
        close(fd);
        continue;
      }
      drop_client();
      client_fd_ = fd;
      printf("h264 socket client connected\n");
    }
  }

  bool send_message(uint32_t type, uint32_t flags, uint64_t sequence,
                    const std::chrono::steady_clock::time_point& captured_at,
                    const std::vector<unsigned char>& bytes) {
    if (client_fd_ < 0 || bytes.empty() || bytes.size() > UINT32_MAX) {
      drop_client();
      return false;
    }
    H264SocketHeader header;
    header.magic = 0x524b4832U;  /* RK H2 */
    header.type = type;
    header.flags = flags;
    header.payload_size = static_cast<uint32_t>(bytes.size());
    header.sequence = sequence;
    header.captured_at_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            captured_at.time_since_epoch()).count());

    struct iovec iov[2];
    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(header);
    iov[1].iov_base = const_cast<unsigned char*>(bytes.data());
    iov[1].iov_len = bytes.size();
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = iov;
    message.msg_iovlen = 2;
    const ssize_t sent = sendmsg(client_fd_, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    const size_t expected = sizeof(header) + bytes.size();
    if (sent != static_cast<ssize_t>(expected)) {
      printf("h264 socket drop client sent=%zd expected=%zu error=%s\n",
             sent, expected, sent < 0 ? strerror(errno) : "short send");
      drop_client();
      return false;
    }
    return true;
  }

  std::string socket_path_;
  int listen_fd_;
  int client_fd_;
  bool client_needs_config_;
  bool client_needs_idr_;
  std::vector<unsigned char> parameter_sets_;
};

/* CaptureWorker is the only owner of /dev/video9 and publishes two latest-only slots. */
class CaptureWorker {
 public:
  enum StartResult {
    kStarted,
    kOpenFailed,
    kFormatMismatch,
  };

  struct Stats {
    uint64_t attempts;
    uint64_t success;
    uint64_t failed;
    uint64_t dropped_infer;
    uint64_t dropped_encode;
  };

  CaptureWorker(const char* device_name, int width, int height, int max_failures)
      : device_name_(device_name),
        width_(width),
        height_(height),
        max_failures_(max_failures),
        stop_requested_(false),
        startup_done_(false),
        start_result_(kOpenFailed),
        terminal_failure_(false),
        has_latest_infer_(false),
        has_latest_encode_(false),
        next_sequence_(0) {
    stats_.attempts = 0;
    stats_.success = 0;
    stats_.failed = 0;
    stats_.dropped_infer = 0;
    stats_.dropped_encode = 0;
  }

  ~CaptureWorker() {
    request_stop();
    join();
  }

  StartResult start() {
    worker_ = std::thread(&CaptureWorker::run, this);
    std::unique_lock<std::mutex> lock(mutex_);
    startup_cv_.wait(lock, [this]() { return startup_done_; });
    return start_result_;
  }

  void request_stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_requested_ = true;
    }
    frame_cv_.notify_all();
  }

  void join() {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  bool wait_take_latest_infer(CaptureFrame* frame,
                              const std::chrono::steady_clock::time_point& deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    frame_cv_.wait_until(lock, deadline, [this]() {
      return has_latest_infer_ || stop_requested_ || terminal_failure_ || g_stop;
    });
    if (!has_latest_infer_) {
      return false;
    }
    *frame = latest_infer_;
    has_latest_infer_ = false;
    return true;
  }

  bool wait_take_latest_encode(CaptureFrame* frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    frame_cv_.wait(lock, [this]() {
      return has_latest_encode_ || stop_requested_ || terminal_failure_ || g_stop;
    });
    if (!has_latest_encode_) {
      return false;
    }
    *frame = latest_encode_;
    has_latest_encode_ = false;
    return true;
  }

  bool terminal_failure() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return terminal_failure_;
  }

  Stats snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

 private:
  void finish_startup(StartResult result) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      start_result_ = result;
      startup_done_ = true;
    }
    startup_cv_.notify_all();
  }

  bool stop_requested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_requested_;
  }

  void run() {
    cv::VideoCapture cap;

    try {
      if (!cap.open(device_name_, cv::CAP_V4L2) || !cap.isOpened()) {
        printf("VideoCapture open failed: %s\n", device_name_);
        finish_startup(kOpenFailed);
        return;
      }

      const int requested_fourcc = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
      const bool fourcc_set = cap.set(cv::CAP_PROP_FOURCC, requested_fourcc);
      const bool width_set = cap.set(cv::CAP_PROP_FRAME_WIDTH, width_);
      const bool height_set = cap.set(cv::CAP_PROP_FRAME_HEIGHT, height_);
      cap.set(cv::CAP_PROP_FPS, 30.0);

      const int actual_width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
      const int actual_height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
      const int actual_fourcc = static_cast<int>(cap.get(cv::CAP_PROP_FOURCC));
      const double actual_camera_fps = cap.get(cv::CAP_PROP_FPS);

      printf("camera device=%s requested=%dx%d MJPG set_result=%d/%d/%d actual=%dx%d fourcc=0x%08x fps=%.3f\n",
             device_name_, width_, height_,
             fourcc_set ? 1 : 0, width_set ? 1 : 0, height_set ? 1 : 0,
             actual_width, actual_height, actual_fourcc, actual_camera_fps);

      if (actual_width != width_ || actual_height != height_) {
        printf("Camera format mismatch: expected=%dx%d actual=%dx%d\n",
               width_, height_, actual_width, actual_height);
        finish_startup(kFormatMismatch);
        return;
      }

      finish_startup(kStarted);

      uint64_t consecutive_failures = 0;
      while (!g_stop && !stop_requested()) {
        cv::Mat captured_bgr;
        bool read_ok = false;

        try {
          read_ok = cap.read(captured_bgr) && !captured_bgr.empty();
        } catch (const cv::Exception& exception) {
          printf("capture read exception: %s\n", exception.what());
        }

        if (!read_ok) {
          uint64_t total_failed = 0;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.attempts;
            ++stats_.failed;
            ++consecutive_failures;
            total_failed = stats_.failed;
            if (consecutive_failures >= static_cast<uint64_t>(max_failures_)) {
              terminal_failure_ = true;
            }
          }

          printf("capture read failed: consecutive=%llu total=%llu\n",
                 static_cast<unsigned long long>(consecutive_failures),
                 static_cast<unsigned long long>(total_failed));

          if (consecutive_failures >= static_cast<uint64_t>(max_failures_)) {
            printf("capture failure threshold reached\n");
            frame_cv_.notify_all();
            return;
          }
          continue;
        }

        cv::Mat immutable_bgr;
        try {
          immutable_bgr = captured_bgr.clone();
        } catch (const cv::Exception& exception) {
          printf("capture clone exception: %s\n", exception.what());
          std::lock_guard<std::mutex> lock(mutex_);
          terminal_failure_ = true;
          frame_cv_.notify_all();
          return;
        }

        {
          std::lock_guard<std::mutex> lock(mutex_);
          ++stats_.attempts;
          ++stats_.success;
          consecutive_failures = 0;
          if (has_latest_infer_) {
            ++stats_.dropped_infer;
          }
          if (has_latest_encode_) {
            ++stats_.dropped_encode;
          }
          latest_infer_.bgr = immutable_bgr;
          latest_infer_.captured_at = std::chrono::steady_clock::now();
          latest_infer_.sequence = ++next_sequence_;
          has_latest_infer_ = true;
          latest_encode_ = latest_infer_;
          has_latest_encode_ = true;
        }
        frame_cv_.notify_all();
      }
    } catch (const cv::Exception& exception) {
      printf("capture worker exception: %s\n", exception.what());
      {
        std::lock_guard<std::mutex> lock(mutex_);
        terminal_failure_ = true;
      }
      frame_cv_.notify_all();
      finish_startup(kOpenFailed);
    }
  }

  const char* device_name_;
  const int width_;
  const int height_;
  const int max_failures_;
  mutable std::mutex mutex_;
  std::condition_variable startup_cv_;
  std::condition_variable frame_cv_;
  std::thread worker_;
  bool stop_requested_;
  bool startup_done_;
  StartResult start_result_;
  bool terminal_failure_;
  bool has_latest_infer_;
  bool has_latest_encode_;
  uint64_t next_sequence_;
  CaptureFrame latest_infer_;
  CaptureFrame latest_encode_;
  Stats stats_;
};

/* EncoderWorker consumes only the latest original BGR frame and never opens V4L2. */
class EncoderWorker {
 public:
  struct Stats {
    uint64_t frames_encoded;
    uint64_t encode_failures;
    uint64_t bytes_encoded;
    double color_convert_us_total;
    double mpp_encode_us_total;
    double end_to_end_us_total;
  };

  EncoderWorker(CaptureWorker* capture_worker, H264AccessUnitPublisher* publisher,
                const LatestOverlay* latest_overlay,
                int width, int height)
      : capture_worker_(capture_worker),
        publisher_(publisher),
        latest_overlay_(latest_overlay),
        width_(width),
        height_(height),
        hor_stride_(width),
        ver_stride_(height),
        mpp_buffer_size_(static_cast<size_t>((width + 63) & ~63) *
                         static_cast<size_t>((height + 63) & ~63) * 3 / 2),
        ctx_(NULL),
        mpi_(NULL),
        cfg_(NULL),
        buf_grp_(NULL),
        frm_buf_(NULL),
        pkt_buf_(NULL),
        startup_done_(false),
        startup_ok_(false),
        running_(false) {
    memset(&stats_, 0, sizeof(stats_));
  }

  ~EncoderWorker() {
    join();
  }

  bool start() {
    worker_ = std::thread(&EncoderWorker::run, this);
    std::unique_lock<std::mutex> lock(startup_mutex_);
    startup_cv_.wait(lock, [this]() { return startup_done_; });
    return startup_ok_;
  }

  void join() {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  Stats snapshot() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
  }

  bool running() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return running_;
  }

 private:
  void finish_startup(bool ok) {
    {
      std::lock_guard<std::mutex> lock(startup_mutex_);
      startup_ok_ = ok;
      startup_done_ = true;
    }
    startup_cv_.notify_all();
  }

  bool initialize() {
    MPP_RET ret = mpp_buffer_group_get_internal(&buf_grp_, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK) {
      printf("mpp buffer group init failed ret=%d\n", ret);
      return false;
    }
    ret = mpp_buffer_get(buf_grp_, &frm_buf_, mpp_buffer_size_);
    if (ret != MPP_OK) {
      printf("mpp input buffer init failed ret=%d\n", ret);
      return false;
    }
    ret = mpp_buffer_get(buf_grp_, &pkt_buf_, mpp_buffer_size_);
    if (ret != MPP_OK) {
      printf("mpp packet buffer init failed ret=%d\n", ret);
      return false;
    }
    ret = mpp_create(&ctx_, &mpi_);
    if (ret != MPP_OK) {
      printf("mpp_create failed ret=%d\n", ret);
      return false;
    }

    MppPollType timeout = MPP_POLL_BLOCK;
    ret = mpi_->control(ctx_, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (ret != MPP_OK) {
      printf("mpp output timeout setup failed ret=%d\n", ret);
      return false;
    }
    ret = mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) {
      printf("mpp_init AVC failed ret=%d\n", ret);
      return false;
    }
    ret = mpp_enc_cfg_init(&cfg_);
    if (ret != MPP_OK) {
      printf("mpp_enc_cfg_init failed ret=%d\n", ret);
      return false;
    }

    const int target_fps = 15;
    const int target_bps = 4 * 1000 * 1000;
    mpp_enc_cfg_set_s32(cfg_, "prep:width", width_);
    mpp_enc_cfg_set_s32(cfg_, "prep:height", height_);
    mpp_enc_cfg_set_s32(cfg_, "prep:hor_stride", hor_stride_);
    mpp_enc_cfg_set_s32(cfg_, "prep:ver_stride", ver_stride_);
    mpp_enc_cfg_set_s32(cfg_, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg_, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_num", target_fps);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_num", target_fps);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(cfg_, "rc:gop", target_fps * 2);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_target", target_bps);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_max", target_bps * 17 / 16);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_min", target_bps * 15 / 16);
    mpp_enc_cfg_set_s32(cfg_, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg_, "h264:profile", 100);
    mpp_enc_cfg_set_s32(cfg_, "h264:level", 31);
    mpp_enc_cfg_set_s32(cfg_, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(cfg_, "h264:trans8x8", 1);
    ret = mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg_);
    if (ret != MPP_OK) {
      printf("mpp encoder config failed ret=%d\n", ret);
      return false;
    }

    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    ret = mpi_->control(ctx_, MPP_ENC_SET_HEADER_MODE, &header_mode);
    if (ret != MPP_OK) {
      printf("mpp header mode setup failed ret=%d\n", ret);
      return false;
    }
    printf("mpp encoder ready codec=h264 format=nv12 size=%dx%d fps=%d bps=%d\n",
           width_, height_, target_fps, target_bps);
    return true;
  }

  void release() {
    if (ctx_ != NULL) {
      mpp_destroy(ctx_);
      ctx_ = NULL;
      mpi_ = NULL;
    }
    if (cfg_ != NULL) {
      mpp_enc_cfg_deinit(cfg_);
      cfg_ = NULL;
    }
    if (frm_buf_ != NULL) {
      mpp_buffer_put(frm_buf_);
      frm_buf_ = NULL;
    }
    if (pkt_buf_ != NULL) {
      mpp_buffer_put(pkt_buf_);
      pkt_buf_ = NULL;
    }
    if (buf_grp_ != NULL) {
      mpp_buffer_group_put(buf_grp_);
      buf_grp_ = NULL;
    }
  }

  bool bgr_to_mpp_nv12(const cv::Mat& bgr, void* destination) {
    if (bgr.empty() || bgr.cols != width_ || bgr.rows != height_) {
      return false;
    }
    cv::Mat i420;
    try {
      cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
    } catch (const cv::Exception& exception) {
      printf("mpp BGR to I420 conversion failed: %s\n", exception.what());
      return false;
    }
    if (destination == NULL || i420.empty()) {
      return false;
    }
    const size_t y_size = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    const size_t chroma_size = y_size / 4;
    unsigned char* nv12 = static_cast<unsigned char*>(destination);
    memset(nv12, 0, mpp_buffer_size_);
    for (int row = 0; row < height_; ++row) {
      memcpy(nv12 + static_cast<size_t>(row) * hor_stride_,
             i420.data + static_cast<size_t>(row) * width_, width_);
    }
    const unsigned char* u = i420.data + y_size;
    const unsigned char* v = u + chroma_size;
    unsigned char* uv = nv12 + static_cast<size_t>(hor_stride_) * ver_stride_;
    for (int row = 0; row < height_ / 2; ++row) {
      for (int column = 0; column < width_ / 2; ++column) {
        const size_t index = static_cast<size_t>(row) * (width_ / 2) + column;
        const size_t offset = static_cast<size_t>(row) * hor_stride_ + column * 2;
        uv[offset] = u[index];
        uv[offset + 1] = v[index];
      }
    }
    return true;
  }

  void draw_latest_overlay(cv::Mat* bgr) const {
    if (bgr == NULL || bgr->empty() || latest_overlay_ == NULL) {
      return;
    }
    const OverlaySnapshot overlay = latest_overlay_->snapshot();
    if (!overlay.valid) {
      return;
    }
    const int width = bgr->cols;
    const int height = bgr->rows;
    const auto point_from_normalized = [width, height](float x, float y) {
      const int px = std::max(0, std::min(width - 1,
          static_cast<int>(std::round(x * static_cast<float>(width)))));
      const int py = std::max(0, std::min(height - 1,
          static_cast<int>(std::round(y * static_cast<float>(height)))));
      return cv::Point(px, py);
    };
    cv::rectangle(*bgr,
                  point_from_normalized(overlay.roi_x_min, overlay.roi_y_min),
                  point_from_normalized(overlay.roi_x_max, overlay.roi_y_max),
                  cv::Scalar(255, 0, 0), 2);
    for (const OverlayBox& box : overlay.boxes) {
      const cv::Point top_left = point_from_normalized(box.left_norm, box.top_norm);
      const cv::Point bottom_right = point_from_normalized(box.right_norm, box.bottom_norm);
      cv::rectangle(*bgr, top_left, bottom_right, cv::Scalar(0, 255, 0), 2);
      char text[256];
      snprintf(text, sizeof(text), "%s %.1f%%", box.label.c_str(), box.confidence * 100.0f);
      cv::putText(*bgr, text, cv::Point(top_left.x, std::min(height - 1, top_left.y + 16)),
                  cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
    }
  }

  bool encode_frame(const CaptureFrame& capture_frame) {
    typedef std::chrono::steady_clock Clock;
    void* input = mpp_buffer_get_ptr(frm_buf_);
    if (input == NULL) {
      printf("mpp input buffer pointer unavailable\n");
      return false;
    }
    cv::Mat encoded_bgr;
    try {
      encoded_bgr = capture_frame.bgr.clone();
    } catch (const cv::Exception& exception) {
      printf("encoder overlay frame clone exception: %s\n", exception.what());
      return false;
    }
    draw_latest_overlay(&encoded_bgr);
    const Clock::time_point convert_begin = Clock::now();
    if (!bgr_to_mpp_nv12(encoded_bgr, input)) {
      return false;
    }
    const Clock::time_point convert_end = Clock::now();

    MppFrame frame = NULL;
    MPP_RET ret = mpp_frame_init(&frame);
    if (ret != MPP_OK) {
      printf("mpp_frame_init failed ret=%d\n", ret);
      return false;
    }
    mpp_frame_set_width(frame, width_);
    mpp_frame_set_height(frame, height_);
    mpp_frame_set_hor_stride(frame, hor_stride_);
    mpp_frame_set_ver_stride(frame, ver_stride_);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_eos(frame, 0);
    mpp_frame_set_buffer(frame, frm_buf_);

    MppPacket packet = NULL;
    ret = mpp_packet_init_with_buffer(&packet, pkt_buf_);
    if (ret != MPP_OK) {
      printf("mpp packet init failed ret=%d\n", ret);
      mpp_frame_deinit(&frame);
      return false;
    }
    mpp_packet_set_length(packet, 0);
    mpp_meta_set_packet(mpp_frame_get_meta(frame), KEY_OUTPUT_PACKET, packet);

    const Clock::time_point encode_begin = Clock::now();
    ret = mpi_->encode_put_frame(ctx_, frame);
    mpp_frame_deinit(&frame);
    if (ret != MPP_OK) {
      printf("mpp encode_put_frame failed ret=%d\n", ret);
      mpp_packet_deinit(&packet);
      return false;
    }

    H264AccessUnit access_unit;
    access_unit.sequence = capture_frame.sequence;
    access_unit.captured_at = capture_frame.captured_at;
    bool end_of_image = false;
    while (!end_of_image) {
      ret = mpi_->encode_get_packet(ctx_, &packet);
      if (ret != MPP_OK || packet == NULL) {
        printf("mpp encode_get_packet failed ret=%d packet=%p\n", ret, packet);
        if (packet != NULL) {
          mpp_packet_deinit(&packet);
        }
        return false;
      }
      const void* packet_pos = mpp_packet_get_pos(packet);
      const size_t packet_length = static_cast<size_t>(mpp_packet_get_length(packet));
      if (packet_pos == NULL || packet_length == 0) {
        printf("mpp packet has no bytes length=%zu\n", packet_length);
        mpp_packet_deinit(&packet);
        return false;
      }
      const unsigned char* packet_data = static_cast<const unsigned char*>(packet_pos);
      access_unit.bytes.insert(access_unit.bytes.end(), packet_data,
                               packet_data + packet_length);
      inspect_h264_annexb(packet_data, packet_length,
                          &access_unit.has_extra_data, &access_unit.is_idr);
      end_of_image = !mpp_packet_is_partition(packet) || mpp_packet_is_eoi(packet);
      mpp_packet_deinit(&packet);
    }
    if (access_unit.bytes.empty()) {
      printf("mpp access unit is empty\n");
      return false;
    }
    if (publisher_ != NULL) {
      publisher_->publish(access_unit);
    }
    const Clock::time_point encode_end = Clock::now();

    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.frames_encoded;
    stats_.bytes_encoded += static_cast<uint64_t>(access_unit.bytes.size());
    stats_.color_convert_us_total +=
        std::chrono::duration<double, std::micro>(convert_end - convert_begin).count();
    stats_.mpp_encode_us_total +=
        std::chrono::duration<double, std::micro>(encode_end - encode_begin).count();
    stats_.end_to_end_us_total +=
        std::chrono::duration<double, std::micro>(encode_end - capture_frame.captured_at).count();
    return true;
  }

  void record_failure() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.encode_failures;
  }

  void run() {
    const bool initialized = initialize();
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      running_ = initialized;
    }
    finish_startup(initialized);
    if (!initialized) {
      record_failure();
      release();
      return;
    }

    while (!g_stop && !capture_worker_->terminal_failure()) {
      CaptureFrame capture_frame;
      if (!capture_worker_->wait_take_latest_encode(&capture_frame)) {
        break;
      }
      if (!encode_frame(capture_frame)) {
        record_failure();
        break;
      }
    }

    release();
    std::lock_guard<std::mutex> lock(stats_mutex_);
    running_ = false;
  }

  CaptureWorker* capture_worker_;
  H264AccessUnitPublisher* publisher_;
  const LatestOverlay* latest_overlay_;
  const int width_;
  const int height_;
  const int hor_stride_;
  const int ver_stride_;
  const size_t mpp_buffer_size_;
  MppCtx ctx_;
  MppApi* mpi_;
  MppEncCfg cfg_;
  MppBufferGroup buf_grp_;
  MppBuffer frm_buf_;
  MppBuffer pkt_buf_;
  std::thread worker_;
  mutable std::mutex startup_mutex_;
  std::condition_variable startup_cv_;
  bool startup_done_;
  bool startup_ok_;
  mutable std::mutex stats_mutex_;
  bool running_;
  Stats stats_;
};

static void dump_tensor_attr(rknn_tensor_attr* attr)
{
  printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, type=%s, qnt_type=%s, "
         "zp=%d, scale=%f\n",
         attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2], attr->dims[3],
         attr->n_elems, attr->size, get_format_string(attr->fmt), get_type_string(attr->type),
         get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

static unsigned char* load_data(FILE* fp, size_t offset, size_t size)
{
  if (fp == NULL) {
    return NULL;
  }

  if (fseek(fp, static_cast<long>(offset), SEEK_SET) != 0) {
    printf("blob seek failure\n");
    return NULL;
  }

  unsigned char* data = static_cast<unsigned char*>(malloc(size));
  if (data == NULL) {
    printf("buffer malloc failure\n");
    return NULL;
  }

  const size_t bytes_read = fread(data, 1, size, fp);
  if (bytes_read != size) {
    printf("blob read failure: expected=%zu actual=%zu\n", size, bytes_read);
    free(data);
    return NULL;
  }

  return data;
}

static unsigned char* load_model(const char* filename, int* model_size)
{
  FILE* fp = fopen(filename, "rb");
  if (fp == NULL) {
    printf("Open file %s failed\n", filename);
    return NULL;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    printf("Model seek failed: %s\n", filename);
    fclose(fp);
    return NULL;
  }

  const long file_size = ftell(fp);
  if (file_size <= 0) {
    printf("Invalid model size: %ld\n", file_size);
    fclose(fp);
    return NULL;
  }

  unsigned char* data = load_data(fp, 0, static_cast<size_t>(file_size));
  fclose(fp);

  if (data == NULL) {
    return NULL;
  }

  *model_size = static_cast<int>(file_size);
  return data;
}

static bool parse_positive_double(const char* text, double* value)
{
  errno = 0;
  char* end = NULL;
  const double parsed = strtod(text, &end);

  if (errno == ERANGE || end == text || *end != '\0' || !std::isfinite(parsed) || parsed <= 0.0) {
    return false;
  }

  *value = parsed;
  return true;
}

struct DangerRoi
{
  double x_min;
  double y_min;
  double x_max;
  double y_max;
};

static std::string trim_text(const std::string& text)
{
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }

  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }

  return text.substr(begin, end - begin);
}

static bool parse_roi_value(const std::string& text, double* value)
{
  errno = 0;
  char* end = NULL;
  const double parsed = strtod(text.c_str(), &end);

  if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
      !std::isfinite(parsed) || parsed < 0.0 || parsed > 1.0) {
    return false;
  }

  *value = parsed;
  return true;
}

static bool load_danger_roi(const char* filename, DangerRoi* roi)
{
  std::ifstream file(filename);
  if (!file.is_open()) {
    printf("Open danger ROI config failed: %s\n", filename);
    return false;
  }

  bool has_x_min = false;
  bool has_y_min = false;
  bool has_x_max = false;
  bool has_y_max = false;
  std::string line;
  int line_number = 0;

  while (std::getline(file, line)) {
    ++line_number;
    const size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line.erase(comment);
    }

    line = trim_text(line);
    if (line.empty()) {
      continue;
    }

    const size_t equal = line.find('=');
    if (equal == std::string::npos) {
      printf("Invalid danger ROI config line=%d\n", line_number);
      return false;
    }

    const std::string key = trim_text(line.substr(0, equal));
    const std::string value_text = trim_text(line.substr(equal + 1));
    double value = 0.0;

    if (!parse_roi_value(value_text, &value)) {
      printf("Invalid danger ROI value line=%d key=%s\n",
             line_number, key.c_str());
      return false;
    }

    if (key == "x_min") {
      roi->x_min = value;
      has_x_min = true;
    } else if (key == "y_min") {
      roi->y_min = value;
      has_y_min = true;
    } else if (key == "x_max") {
      roi->x_max = value;
      has_x_max = true;
    } else if (key == "y_max") {
      roi->y_max = value;
      has_y_max = true;
    } else {
      printf("Unknown danger ROI key line=%d key=%s\n",
             line_number, key.c_str());
      return false;
    }
  }

  if (!has_x_min || !has_y_min || !has_x_max || !has_y_max ||
      roi->x_min >= roi->x_max || roi->y_min >= roi->y_max) {
    printf("Invalid danger ROI range\n");
    return false;
  }

  return true;
}

struct HmiRuntimePaths
{
  std::string preview_temp;
  std::string preview_final;
  std::string state_temp;
  std::string state_final;
};

static bool ensure_directory(const char* path)
{
  struct stat status;

  if (mkdir(path, 0755) == 0) {
    return true;
  }
  if (errno == EEXIST && stat(path, &status) == 0 && S_ISDIR(status.st_mode)) {
    return true;
  }

  fprintf(stderr, "hmi runtime mkdir failed path=%s error=%s\n", path, strerror(errno));
  return false;
}

static bool write_all_bytes(int fd, const char* data, size_t size)
{
  size_t written = 0;

  while (written < size) {
    const ssize_t result = write(fd, data + written, size - written);
    if (result > 0) {
      written += static_cast<size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }

  return true;
}

static bool fsync_file(const std::string& path)
{
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "hmi runtime open for sync failed path=%s error=%s\n",
            path.c_str(), strerror(errno));
    return false;
  }

  const bool ok = fsync(fd) == 0;
  if (!ok) {
    fprintf(stderr, "hmi runtime fsync failed path=%s error=%s\n",
            path.c_str(), strerror(errno));
  }
  close(fd);
  return ok;
}

static bool write_atomic_text(const std::string& temporary_path,
                              const std::string& final_path,
                              const char* text, size_t length)
{
  unlink(temporary_path.c_str());
  const int fd = open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fprintf(stderr, "hmi runtime state open failed path=%s error=%s\n",
            temporary_path.c_str(), strerror(errno));
    return false;
  }

  const bool written = write_all_bytes(fd, text, length);
  const bool synced = written && fsync(fd) == 0;
  const int close_result = close(fd);
  if (!written || !synced || close_result != 0) {
    fprintf(stderr, "hmi runtime state write failed path=%s error=%s\n",
            temporary_path.c_str(), strerror(errno));
    unlink(temporary_path.c_str());
    return false;
  }

  if (rename(temporary_path.c_str(), final_path.c_str()) != 0) {
    fprintf(stderr, "hmi runtime state rename failed from=%s to=%s error=%s\n",
            temporary_path.c_str(), final_path.c_str(), strerror(errno));
    unlink(temporary_path.c_str());
    return false;
  }
  return true;
}

static uint64_t monotonic_milliseconds()
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(now.tv_sec) * 1000ULL +
         static_cast<uint64_t>(now.tv_nsec / 1000000L);
}

static bool prepare_hmi_runtime(HmiRuntimePaths* paths)
{
  if (!ensure_directory("hmi_runtime") || !ensure_directory("hmi_runtime/preview")) {
    return false;
  }

  paths->preview_temp = "hmi_runtime/preview/.latest.tmp.jpg";
  paths->preview_final = "hmi_runtime/preview/latest.jpg";
  paths->state_temp = "hmi_runtime/.ai_runtime.tmp.json";
  paths->state_final = "hmi_runtime/ai_runtime.json";
  return true;
}

static bool publish_preview_frame(const HmiRuntimePaths& paths, const cv::Mat& annotated_bgr)
{
  unlink(paths.preview_temp.c_str());
  try {
    if (!cv::imwrite(paths.preview_temp, annotated_bgr)) {
      fprintf(stderr, "hmi preview JPEG encode failed\n");
      return false;
    }
  } catch (const cv::Exception& exception) {
    fprintf(stderr, "hmi preview JPEG exception: %s\n", exception.what());
    return false;
  }

  if (!fsync_file(paths.preview_temp)) {
    unlink(paths.preview_temp.c_str());
    return false;
  }
  if (rename(paths.preview_temp.c_str(), paths.preview_final.c_str()) != 0) {
    fprintf(stderr, "hmi preview rename failed error=%s\n", strerror(errno));
    unlink(paths.preview_temp.c_str());
    return false;
  }
  return true;
}

struct PreviewEncoderMetrics {
  bool encoder_running;
  CaptureWorker::Stats capture;
  EncoderWorker::Stats encoder;
  double elapsed_sec;
};

static PreviewEncoderMetrics snapshot_preview_encoder_metrics(
    const CaptureWorker& capture_worker, const EncoderWorker& encoder_worker,
    double elapsed_sec) {
  PreviewEncoderMetrics metrics;
  metrics.encoder_running = encoder_worker.running();
  metrics.capture = capture_worker.snapshot();
  metrics.encoder = encoder_worker.snapshot();
  metrics.elapsed_sec = elapsed_sec > 0.0 ? elapsed_sec : 0.0;
  return metrics;
}

static bool publish_ai_runtime_state(const HmiRuntimePaths& paths,
                                     const char* camera_state, const char* ai_state,
                                     uint64_t preview_sequence, uint64_t frames_inferred,
                                     uint64_t intrusion_events, uint64_t intrusion_clears,
                                     bool intrusion_active, uint64_t last_event_monotonic_ms,
                                     const char* last_event_source, uint64_t preview_saved,
                                     uint64_t preview_failed,
                                     const PreviewEncoderMetrics* encoder_metrics)
{
  const uint64_t frames_encoded =
      encoder_metrics != NULL ? encoder_metrics->encoder.frames_encoded : 0;
  const uint64_t encode_failures =
      encoder_metrics != NULL ? encoder_metrics->encoder.encode_failures : 0;
  const uint64_t encoded_bytes =
      encoder_metrics != NULL ? encoder_metrics->encoder.bytes_encoded : 0;
  const uint64_t dropped_infer =
      encoder_metrics != NULL ? encoder_metrics->capture.dropped_infer : 0;
  const uint64_t dropped_encode =
      encoder_metrics != NULL ? encoder_metrics->capture.dropped_encode : 0;
  const double elapsed_sec = encoder_metrics != NULL ? encoder_metrics->elapsed_sec : 0.0;
  const bool has_encoded_frames = frames_encoded > 0 && elapsed_sec > 0.0;
  const double encode_fps = has_encoded_frames ? frames_encoded / elapsed_sec : 0.0;
  const double bitrate_bps = has_encoded_frames ? encoded_bytes * 8.0 / elapsed_sec : 0.0;
  const double color_convert_ms = has_encoded_frames
      ? encoder_metrics->encoder.color_convert_us_total / frames_encoded / 1000.0 : 0.0;
  const double mpp_encode_ms = has_encoded_frames
      ? encoder_metrics->encoder.mpp_encode_us_total / frames_encoded / 1000.0 : 0.0;
  const double encode_latency_ms = has_encoded_frames
      ? encoder_metrics->encoder.end_to_end_us_total / frames_encoded / 1000.0 : 0.0;
  const bool encoder_running = encoder_metrics != NULL && encoder_metrics->encoder_running;

  char json[3072];
  const int length = snprintf(
      json, sizeof(json),
      "{\"schema_version\":1,\"monotonic_ms\":%llu,"
      "\"camera_state\":\"%s\",\"ai_state\":\"%s\","
      "\"preview_frame_path\":\"hmi_runtime/preview/latest.jpg\","
      "\"preview_frame_sequence\":%llu,\"frames_inferred\":%llu,"
      "\"preview_saved\":%llu,\"preview_failed\":%llu,"
      "\"preview_transport\":\"%s\",\"preview_stream_state\":\"%s\","
      "\"preview_encoded_frames\":%llu,\"preview_encode_failures\":%llu,"
      "\"preview_dropped_infer_frames\":%llu,\"preview_dropped_encode_frames\":%llu,"
      "\"preview_fps\":%.3f,\"preview_bitrate_bps\":%.3f,"
      "\"preview_color_convert_ms\":%.3f,\"preview_mpp_encode_ms\":%.3f,"
      "\"preview_latency_ms\":%.3f,"
      "\"roi_event_count\":%llu,\"roi_clear_count\":%llu,"
      "\"roi_active\":%s,\"last_event_monotonic_ms\":%llu,"
      "\"last_event_source\":\"%s\"}\n",
      static_cast<unsigned long long>(monotonic_milliseconds()), camera_state, ai_state,
      static_cast<unsigned long long>(preview_sequence),
      static_cast<unsigned long long>(frames_inferred),
      static_cast<unsigned long long>(preview_saved),
      static_cast<unsigned long long>(preview_failed),
      encoder_running ? "mpp_h264" : "jpeg_fallback",
      encoder_running ? "encoding" : "unavailable",
      static_cast<unsigned long long>(frames_encoded),
      static_cast<unsigned long long>(encode_failures),
      static_cast<unsigned long long>(dropped_infer),
      static_cast<unsigned long long>(dropped_encode),
      encode_fps, bitrate_bps, color_convert_ms, mpp_encode_ms, encode_latency_ms,
      static_cast<unsigned long long>(intrusion_events),
      static_cast<unsigned long long>(intrusion_clears), intrusion_active ? "true" : "false",
      static_cast<unsigned long long>(last_event_monotonic_ms), last_event_source);

  if (length < 0 || static_cast<size_t>(length) >= sizeof(json)) {
    fprintf(stderr, "hmi runtime JSON format failed\n");
    return false;
  }
  return write_atomic_text(paths.state_temp, paths.state_final, json,
                           static_cast<size_t>(length));
}

int main(int argc, char** argv)
{
  if (argc != 6) {
    printf("Usage: %s <rknn_model> <device> <duration_sec> <infer_fps> <roi_config>\n",
           argv[0]);
    return 2;
  }

  const char* model_name = argv[1];
  const char* device_name = argv[2];
  const char* roi_config_name = argv[5];
  DangerRoi danger_roi;

  if (!load_danger_roi(roi_config_name, &danger_roi)) {
    printf("Invalid danger ROI config: %s\n", roi_config_name);
    return 2;
  }

  printf("danger ROI config=%s x=[%.3f, %.3f] y=[%.3f, %.3f]\n",
         roi_config_name,
         danger_roi.x_min, danger_roi.x_max,
         danger_roi.y_min, danger_roi.y_max);

  double duration_sec = 0.0;
  double target_infer_fps = 0.0;

  if (!parse_positive_double(argv[3], &duration_sec)) {
    printf("Invalid duration_sec: %s\n", argv[3]);
    return 2;
  }

  if (!parse_positive_double(argv[4], &target_infer_fps)) {
    printf("Invalid infer_fps: %s\n", argv[4]);
    return 2;
  }

  const int camera_width = 1280;
  const int camera_height = 720;
  const int max_consecutive_capture_failures = 20;
  const uint64_t evidence_interval = 10;
  const float nms_threshold = NMS_THRESH;
  const float box_conf_threshold = BOX_THRESH;

  int program_rc = 1;
  rknn_context ctx = 0;
  bool rknn_ready = false;
  unsigned char* model_data = NULL;
  int model_data_size = 0;

  CaptureWorker capture_worker(device_name, camera_width, camera_height,
                               max_consecutive_capture_failures);
  H264AccessUnitPublisher h264_publisher(
      "/opt/rk3568_yolov5_demo/hmi_runtime/h264.sock");
  LatestOverlay latest_overlay;
  EncoderWorker encoder_worker(&capture_worker, &h264_publisher, &latest_overlay,
                                camera_width, camera_height);
  bool encoder_started = false;
  HmiRuntimePaths hmi_runtime;
  const bool hmi_runtime_ready = prepare_hmi_runtime(&hmi_runtime);
  uint64_t preview_sequence = 0;
  uint64_t preview_saved = 0;
  uint64_t preview_failed = 0;
  uint64_t last_event_monotonic_ms = 0;
  std::string last_event_source = "none";

  if (hmi_runtime_ready &&
      !publish_ai_runtime_state(hmi_runtime, "starting", "starting",
                                preview_sequence, 0, 0, 0, false,
                                last_event_monotonic_ms, last_event_source.c_str(),
                                preview_saved, preview_failed, NULL)) {
    ++preview_failed;
  }

  do {
    /* CaptureWorker 是唯一 V4L2 打开者；启动失败不得进入 RKNN 初始化。 */
    const CaptureWorker::StartResult capture_start = capture_worker.start();
    if (capture_start != CaptureWorker::kStarted) {
      if (hmi_runtime_ready &&
          !publish_ai_runtime_state(hmi_runtime, "unavailable", "failed",
                                    preview_sequence, 0, 0, 0, false,
                                    last_event_monotonic_ms, last_event_source.c_str(),
                                    preview_saved, preview_failed, NULL)) {
        ++preview_failed;
      }
      program_rc = capture_start == CaptureWorker::kFormatMismatch ? 4 : 3;
      break;
    }

    printf("post process config: box_conf_threshold=%.2f nms_threshold=%.2f\n",
           box_conf_threshold, nms_threshold);

    model_data = load_model(model_name, &model_data_size);
    if (model_data == NULL) {
      printf("Model load failed: %s\n", model_name);
      program_rc = 5;
      break;
    }

    int ret = rknn_init(&ctx, model_data, model_data_size, 0, NULL);
    if (ret < 0) {
      printf("rknn_init failed ret=%d\n", ret);
      program_rc = 6;
      break;
    }
    rknn_ready = true;

    rknn_sdk_version version;
    memset(&version, 0, sizeof(version));
    ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(version));
    if (ret < 0) {
      printf("RKNN_QUERY_SDK_VERSION failed ret=%d\n", ret);
      program_rc = 7;
      break;
    }
    printf("sdk version: %s driver version: %s\n", version.api_version, version.drv_version);

    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0) {
      printf("RKNN_QUERY_IN_OUT_NUM failed ret=%d\n", ret);
      program_rc = 8;
      break;
    }

    if (io_num.n_input != 1 || io_num.n_output < 3) {
      printf("Unsupported model IO count: input=%d output=%d\n", io_num.n_input, io_num.n_output);
      program_rc = 9;
      break;
    }
    printf("model input num=%d output num=%d\n", io_num.n_input, io_num.n_output);

    std::vector<rknn_tensor_attr> input_attrs(io_num.n_input);
    std::vector<rknn_tensor_attr> output_attrs(io_num.n_output);
    memset(input_attrs.data(), 0, input_attrs.size() * sizeof(rknn_tensor_attr));
    memset(output_attrs.data(), 0, output_attrs.size() * sizeof(rknn_tensor_attr));

    bool attr_query_failed = false;
    for (uint32_t i = 0; i < io_num.n_input; ++i) {
      input_attrs[i].index = i;
      ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
      if (ret < 0) {
        printf("RKNN_QUERY_INPUT_ATTR failed index=%u ret=%d\n", i, ret);
        attr_query_failed = true;
        break;
      }
      dump_tensor_attr(&input_attrs[i]);
    }
    if (attr_query_failed) {
      program_rc = 10;
      break;
    }

    for (uint32_t i = 0; i < io_num.n_output; ++i) {
      output_attrs[i].index = i;
      ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
      if (ret < 0) {
        printf("RKNN_QUERY_OUTPUT_ATTR failed index=%u ret=%d\n", i, ret);
        attr_query_failed = true;
        break;
      }
      dump_tensor_attr(&output_attrs[i]);
    }
    if (attr_query_failed) {
      program_rc = 11;
      break;
    }

    int model_width = 0;
    int model_height = 0;
    int model_channel = 3;

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
      model_channel = input_attrs[0].dims[1];
      model_width = input_attrs[0].dims[2];
      model_height = input_attrs[0].dims[3];
    } else {
      model_width = input_attrs[0].dims[1];
      model_height = input_attrs[0].dims[2];
      model_channel = input_attrs[0].dims[3];
    }

    if (model_width <= 0 || model_height <= 0 || model_channel != 3) {
      printf("Unsupported model input: width=%d height=%d channel=%d\n",
             model_width, model_height, model_channel);
      program_rc = 12;
      break;
    }
    printf("model input height=%d width=%d channel=%d\n",
           model_height, model_width, model_channel);

    const size_t resize_size = static_cast<size_t>(model_width) *
                               static_cast<size_t>(model_height) *
                               static_cast<size_t>(model_channel);
    std::vector<unsigned char> resize_buffer(resize_size, 0);

    rknn_input input;
    memset(&input, 0, sizeof(input));
    input.index = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.size = static_cast<uint32_t>(resize_size);
    input.fmt = RKNN_TENSOR_NHWC;
    input.pass_through = 0;

    std::vector<float> out_scales;
    std::vector<int32_t> out_zps;
    out_scales.reserve(io_num.n_output);
    out_zps.reserve(io_num.n_output);
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
      out_scales.push_back(output_attrs[i].scale);
      out_zps.push_back(output_attrs[i].zp);
    }

    std::vector<rknn_output> outputs(io_num.n_output);

    uint64_t frames_inferred = 0;
    uint64_t detections = 0;
    uint64_t evidence_saved = 0;
    uint64_t evidence_failed = 0;
    uint64_t intrusion_events = 0;
    uint64_t intrusion_clears = 0;
    const uint64_t clear_after_missed_inferences = 3;
    uint64_t intrusion_miss_streak = 0;
    bool intrusion_active = false;

    double preprocess_us_total = 0.0;
    double inference_us_total = 0.0;
    double postprocess_us_total = 0.0;
    double end_to_end_us_total = 0.0;

    std::signal(SIGINT, handle_sigint);

    /* The local H.264 publisher is optional and must not stop AI on failure. */
    if (hmi_runtime_ready && !h264_publisher.start()) {
      printf("h264 socket unavailable; AI continues with JPEG preview\n");
    }

    /* MPP is an optional preview producer: startup failure must not stop AI. */
    encoder_started = encoder_worker.start();
    if (!encoder_started) {
      printf("mpp encoder unavailable; AI continues with JPEG preview\n");
    }

    typedef std::chrono::steady_clock Clock;
    const Clock::time_point loop_start = Clock::now();
    const Clock::time_point loop_deadline =
        loop_start + std::chrono::microseconds(static_cast<int64_t>(duration_sec * 1000000.0));
    const std::chrono::microseconds infer_interval(
        static_cast<int64_t>(1000000.0 / target_infer_fps));
    Clock::time_point next_infer = loop_start;

    bool fatal_loop_error = false;

    while (!g_stop && Clock::now() < loop_deadline) {
      if (capture_worker.terminal_failure()) {
        printf("capture failure threshold reached\n");
        const PreviewEncoderMetrics encoder_metrics = snapshot_preview_encoder_metrics(
            capture_worker, encoder_worker,
            std::chrono::duration<double>(Clock::now() - loop_start).count());
        if (hmi_runtime_ready &&
            !publish_ai_runtime_state(hmi_runtime, "failed", "failed",
                                      preview_sequence, frames_inferred,
                                      intrusion_events, intrusion_clears, intrusion_active,
                                      last_event_monotonic_ms, last_event_source.c_str(),
                                      preview_saved, preview_failed,
                                      &encoder_metrics)) {
          ++preview_failed;
        }
        program_rc = 13;
        fatal_loop_error = true;
        break;
      }

      const Clock::time_point selection_time = Clock::now();
      if (selection_time < next_infer) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      CaptureFrame capture_frame;
      if (!capture_worker.wait_take_latest_infer(
              &capture_frame, selection_time + std::chrono::milliseconds(5))) {
        if (capture_worker.terminal_failure() || g_stop) {
          continue;
        }
        continue;
      }

      /* 以当前时间重新设定下次推理点，避免推理落后时连续突发补帧。 */
      next_infer = selection_time + infer_interval;

      const cv::Mat original_bgr = capture_frame.bgr;
      const Clock::time_point capture_begin = capture_frame.captured_at;

      const Clock::time_point preprocess_begin = Clock::now();
      cv::Mat rgb;
      cv::cvtColor(original_bgr, rgb, cv::COLOR_BGR2RGB);
      if (!rgb.isContinuous()) {
        rgb = rgb.clone();
      }

      if (rgb.cols != model_width || rgb.rows != model_height) {
        rga_buffer_t src = wrapbuffer_virtualaddr(rgb.data, rgb.cols, rgb.rows, RK_FORMAT_RGB_888);
        rga_buffer_t dst = wrapbuffer_virtualaddr(resize_buffer.data(),
                                                  model_width, model_height,
                                                  RK_FORMAT_RGB_888);
        im_rect src_rect;
        im_rect dst_rect;
        memset(&src_rect, 0, sizeof(src_rect));
        memset(&dst_rect, 0, sizeof(dst_rect));

        IM_STATUS rga_status = imcheck(src, dst, src_rect, dst_rect);
        if (rga_status != IM_STATUS_NOERROR) {
          printf("RGA imcheck failed: %s\n", imStrError(rga_status));
          program_rc = 14;
          fatal_loop_error = true;
          break;
        }

        rga_status = imresize(src, dst);
        if (rga_status != IM_STATUS_SUCCESS) {
          printf("RGA imresize failed: %s\n", imStrError(rga_status));
          program_rc = 15;
          fatal_loop_error = true;
          break;
        }
        input.buf = resize_buffer.data();
      } else {
        input.buf = rgb.data;
      }

      const Clock::time_point preprocess_end = Clock::now();
      preprocess_us_total +=
          std::chrono::duration<double, std::micro>(preprocess_end - preprocess_begin).count();

      const Clock::time_point inference_begin = Clock::now();
      ret = rknn_inputs_set(ctx, 1, &input);
      if (ret < 0) {
        printf("rknn_inputs_set failed ret=%d\n", ret);
        program_rc = 16;
        fatal_loop_error = true;
        break;
      }

      ret = rknn_run(ctx, NULL);
      if (ret < 0) {
        printf("rknn_run failed ret=%d\n", ret);
        program_rc = 17;
        fatal_loop_error = true;
        break;
      }

      memset(outputs.data(), 0, outputs.size() * sizeof(rknn_output));
      for (uint32_t i = 0; i < io_num.n_output; ++i) {
        outputs[i].want_float = 0;
      }

      ret = rknn_outputs_get(ctx, io_num.n_output, outputs.data(), NULL);
      if (ret < 0) {
        printf("rknn_outputs_get failed ret=%d\n", ret);
        program_rc = 18;
        fatal_loop_error = true;
        break;
      }
      const Clock::time_point inference_end = Clock::now();
      inference_us_total +=
          std::chrono::duration<double, std::micro>(inference_end - inference_begin).count();

      const Clock::time_point postprocess_begin = Clock::now();
      const float scale_w = static_cast<float>(model_width) / original_bgr.cols;
      const float scale_h = static_cast<float>(model_height) / original_bgr.rows;
      detect_result_group_t detect_result_group;
      memset(&detect_result_group, 0, sizeof(detect_result_group));

      post_process(static_cast<int8_t*>(outputs[0].buf),
                   static_cast<int8_t*>(outputs[1].buf),
                   static_cast<int8_t*>(outputs[2].buf),
                   model_height, model_width,
                   box_conf_threshold, nms_threshold,
                   scale_w, scale_h,
                   out_zps, out_scales,
                   &detect_result_group);

      ret = rknn_outputs_release(ctx, io_num.n_output, outputs.data());
      if (ret < 0) {
        printf("rknn_outputs_release failed ret=%d\n", ret);
        program_rc = 19;
        fatal_loop_error = true;
        break;
      }

      ++frames_inferred;
      detections += static_cast<uint64_t>(detect_result_group.count);

      cv::Mat annotated_bgr;
      try {
        annotated_bgr = original_bgr.clone();
      } catch (const cv::Exception& exception) {
        printf("annotated frame clone exception: %s\n", exception.what());
        program_rc = 21;
        fatal_loop_error = true;
        break;
      }

      const bool save_evidence =
          frames_inferred == 1 || (frames_inferred % evidence_interval) == 0;
      cv::Mat evidence_input;
      if (save_evidence) {
        evidence_input = annotated_bgr.clone();
      }

      const int roi_left = static_cast<int>(std::round(danger_roi.x_min * original_bgr.cols));
      const int roi_top = static_cast<int>(std::round(danger_roi.y_min * original_bgr.rows));
      const int roi_right = static_cast<int>(std::round(danger_roi.x_max * original_bgr.cols));
      const int roi_bottom = static_cast<int>(std::round(danger_roi.y_max * original_bgr.rows));

      cv::rectangle(annotated_bgr,
                    cv::Point(roi_left, roi_top),
                    cv::Point(roi_right, roi_bottom),
                    cv::Scalar(255, 0, 0), 2);

      bool person_in_roi = false;
      float event_x_norm = 0.0f;
      float event_y_norm = 0.0f;
      std::vector<OverlayBox> overlay_boxes;
      overlay_boxes.reserve(static_cast<size_t>(detect_result_group.count));

      for (int i = 0; i < detect_result_group.count; ++i) {
        const detect_result_t* result = &detect_result_group.results[i];

        OverlayBox overlay_box;
        overlay_box.left_norm = static_cast<float>(result->box.left) /
            static_cast<float>(original_bgr.cols);
        overlay_box.top_norm = static_cast<float>(result->box.top) /
            static_cast<float>(original_bgr.rows);
        overlay_box.right_norm = static_cast<float>(result->box.right) /
            static_cast<float>(original_bgr.cols);
        overlay_box.bottom_norm = static_cast<float>(result->box.bottom) /
            static_cast<float>(original_bgr.rows);
        overlay_box.confidence = result->prop;
        overlay_box.label = result->name;
        overlay_boxes.push_back(overlay_box);

        if (strcmp(result->name, "person") == 0) {
          const float point_x =
              0.5f * static_cast<float>(result->box.left + result->box.right);
          const float point_y = static_cast<float>(result->box.bottom);
          const float point_x_norm = point_x / static_cast<float>(annotated_bgr.cols);
          const float point_y_norm = point_y / static_cast<float>(annotated_bgr.rows);

          if (point_x_norm >= danger_roi.x_min &&
              point_x_norm <= danger_roi.x_max &&
              point_y_norm >= danger_roi.y_min &&
              point_y_norm <= danger_roi.y_max) {
            person_in_roi = true;
            event_x_norm = point_x_norm;
            event_y_norm = point_y_norm;
          }
        }
        printf("%s @ (%d %d %d %d) %.6f\n",
               result->name,
               result->box.left, result->box.top,
               result->box.right, result->box.bottom,
               result->prop);

        char text[256];
        snprintf(text, sizeof(text), "%s %.1f%%", result->name, result->prop * 100.0f);
        cv::rectangle(annotated_bgr,
                      cv::Point(result->box.left, result->box.top),
                      cv::Point(result->box.right, result->box.bottom),
                      cv::Scalar(0, 255, 0), 2);
        cv::putText(annotated_bgr, text,
                    cv::Point(result->box.left, result->box.top + 16),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(0, 255, 0), 1);
      }

      latest_overlay.update(danger_roi.x_min, danger_roi.y_min,
                            danger_roi.x_max, danger_roi.y_max, overlay_boxes);

      if (person_in_roi) {
        intrusion_miss_streak = 0;

        if (!intrusion_active) {
          intrusion_active = true;
          ++intrusion_events;
          last_event_monotonic_ms = monotonic_milliseconds();
          last_event_source = "ai";
          printf("INTRUSION_EVENT infer_index=%llu point=(%.4f,%.4f) "
                "roi=(%.4f,%.4f,%.4f,%.4f)\n",
                static_cast<unsigned long long>(frames_inferred),
                event_x_norm, event_y_norm,
                danger_roi.x_min, danger_roi.y_min,
                danger_roi.x_max, danger_roi.y_max);
          fflush(stdout);
        }
      } else if (intrusion_active) {
        ++intrusion_miss_streak;

        if (intrusion_miss_streak >= clear_after_missed_inferences) {
          intrusion_active = false;
          intrusion_miss_streak = 0;
          ++intrusion_clears;
          last_event_monotonic_ms = monotonic_milliseconds();
          last_event_source = "ai";
          printf("INTRUSION_CLEAR infer_index=%llu\n",
                static_cast<unsigned long long>(frames_inferred));
          fflush(stdout);
        }
      }
      const Clock::time_point postprocess_end = Clock::now();
      postprocess_us_total +=
          std::chrono::duration<double, std::micro>(postprocess_end - postprocess_begin).count();
      end_to_end_us_total +=
          std::chrono::duration<double, std::micro>(postprocess_end - capture_begin).count();

      if (hmi_runtime_ready) {
        if (publish_preview_frame(hmi_runtime, annotated_bgr)) {
          ++preview_sequence;
          ++preview_saved;
        } else {
          ++preview_failed;
        }
        const PreviewEncoderMetrics encoder_metrics = snapshot_preview_encoder_metrics(
            capture_worker, encoder_worker,
            std::chrono::duration<double>(Clock::now() - loop_start).count());
        if (!publish_ai_runtime_state(hmi_runtime, "online", "running",
                                      preview_sequence, frames_inferred,
                                      intrusion_events, intrusion_clears, intrusion_active,
                                      last_event_monotonic_ms, last_event_source.c_str(),
                                      preview_saved, preview_failed,
                                      &encoder_metrics)) {
          ++preview_failed;
        }
      }

      /* 只保存第一轮和每 10 次推理的证据，避免每帧 JPEG I/O 污染性能。 */
      if (save_evidence) {
        char input_path[128];
        char output_path[128];
        snprintf(input_path, sizeof(input_path), "evidence_input_%04llu.jpg",
                 static_cast<unsigned long long>(frames_inferred));
        snprintf(output_path, sizeof(output_path), "evidence_out_%04llu.jpg",
                 static_cast<unsigned long long>(frames_inferred));

        bool input_saved = false;
        bool output_saved = false;
        try {
          input_saved = cv::imwrite(input_path, evidence_input);
          output_saved = cv::imwrite(output_path, annotated_bgr);
        } catch (const cv::Exception& exception) {
          printf("evidence imwrite exception: %s\n", exception.what());
        }

        if (input_saved && output_saved) {
          ++evidence_saved;
          printf("evidence_saved input=%s output=%s\n", input_path, output_path);
        } else {
          ++evidence_failed;
          printf("evidence_save_failed input=%s output=%s\n", input_path, output_path);
        }
      }
    }

    capture_worker.request_stop();
    encoder_worker.join();
    capture_worker.join();
    const CaptureWorker::Stats capture_stats = capture_worker.snapshot();
    const EncoderWorker::Stats encoder_stats = encoder_worker.snapshot();

    const double elapsed_sec =
        std::chrono::duration<double>(Clock::now() - loop_start).count();

    printf("===== runtime summary =====\n");
    printf("capture_attempts=%llu\n", static_cast<unsigned long long>(capture_stats.attempts));
    printf("capture_success=%llu\n", static_cast<unsigned long long>(capture_stats.success));
    printf("capture_failed=%llu\n", static_cast<unsigned long long>(capture_stats.failed));
    printf("frames_seen=%llu\n", static_cast<unsigned long long>(capture_stats.success));
    printf("frames_inferred=%llu\n", static_cast<unsigned long long>(frames_inferred));
    printf("frames_skipped=%llu\n", static_cast<unsigned long long>(capture_stats.dropped_infer));
    printf("detections=%llu\n", static_cast<unsigned long long>(detections));
    printf("evidence_saved=%llu\n", static_cast<unsigned long long>(evidence_saved));
    printf("evidence_failed=%llu\n", static_cast<unsigned long long>(evidence_failed));
    printf("intrusion_events=%llu\n", static_cast<unsigned long long>(intrusion_events));
    printf("intrusion_clears=%llu\n", static_cast<unsigned long long>(intrusion_clears));
    printf("elapsed_sec=%.3f\n", elapsed_sec);

    if (capture_stats.success > 0) {
      printf("capture_fps=%.3f\n", capture_stats.success / elapsed_sec);
      printf("latest_infer_dropped=%llu\n",
             static_cast<unsigned long long>(capture_stats.dropped_infer));
      printf("latest_encode_dropped=%llu\n",
             static_cast<unsigned long long>(capture_stats.dropped_encode));
    }

    if (frames_inferred > 0) {
      printf("avg_preprocess_ms=%.3f\n", preprocess_us_total / frames_inferred / 1000.0);
      printf("avg_inference_ms=%.3f\n", inference_us_total / frames_inferred / 1000.0);
      printf("avg_postprocess_ms=%.3f\n", postprocess_us_total / frames_inferred / 1000.0);
      printf("avg_end_to_end_ms=%.3f\n", end_to_end_us_total / frames_inferred / 1000.0);
      printf("inference_fps=%.3f\n", frames_inferred / elapsed_sec);
    }

    printf("encoder_running=%d\n", encoder_worker.running() ? 1 : 0);
    printf("frames_encoded=%llu\n", static_cast<unsigned long long>(encoder_stats.frames_encoded));
    printf("encode_failures=%llu\n", static_cast<unsigned long long>(encoder_stats.encode_failures));
    printf("encoded_bytes=%llu\n", static_cast<unsigned long long>(encoder_stats.bytes_encoded));
    if (encoder_stats.frames_encoded > 0) {
      printf("encode_fps=%.3f\n", encoder_stats.frames_encoded / elapsed_sec);
      printf("encode_bitrate_bps=%.3f\n",
             encoder_stats.bytes_encoded * 8.0 / elapsed_sec);
      printf("avg_color_convert_ms=%.3f\n",
             encoder_stats.color_convert_us_total / encoder_stats.frames_encoded / 1000.0);
      printf("avg_mpp_encode_ms=%.3f\n",
             encoder_stats.mpp_encode_us_total / encoder_stats.frames_encoded / 1000.0);
      printf("avg_encode_end_to_end_ms=%.3f\n",
             encoder_stats.end_to_end_us_total / encoder_stats.frames_encoded / 1000.0);
    }

    const PreviewEncoderMetrics final_encoder_metrics = snapshot_preview_encoder_metrics(
        capture_worker, encoder_worker, elapsed_sec);
    if (hmi_runtime_ready &&
        !publish_ai_runtime_state(hmi_runtime,
                                  fatal_loop_error ? "failed" : "stopped",
                                  fatal_loop_error ? "failed" : "stopped",
                                  preview_sequence, frames_inferred,
                                  intrusion_events, intrusion_clears, intrusion_active,
                                  last_event_monotonic_ms, last_event_source.c_str(),
                                  preview_saved, preview_failed,
                                  &final_encoder_metrics)) {
      ++preview_failed;
    }

    if (!fatal_loop_error) {
      program_rc = 0;
    }
  } while (false);

  capture_worker.request_stop();
  encoder_worker.join();
  capture_worker.join();

  if (rknn_ready) {
    const int destroy_ret = rknn_destroy(ctx);
    if (destroy_ret < 0) {
      printf("rknn_destroy failed ret=%d\n", destroy_ret);
      if (program_rc == 0) {
        program_rc = 20;
      }
    }
  }

  if (model_data != NULL) {
    free(model_data);
  }

  deinitPostProcess();
  printf("program_rc=%d\n", program_rc);
  return program_rc;
}
