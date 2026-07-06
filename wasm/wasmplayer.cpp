#include "moonlight_wasm.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include <h264_stream.h>

#include <pthread.h>

#include "samsung/wasm/elementary_audio_track_config.h"
#include "samsung/wasm/elementary_media_packet.h"
#include "samsung/wasm/elementary_video_track_config.h"
#include "samsung/html/html_media_element_listener.h"
#include "samsung/wasm/operation_result.h"

#define INITIAL_DECODE_BUFFER_LEN 128 * 1024

using std::chrono_literals::operator""s;
using std::chrono_literals::operator""ms;
using EmssReadyState = samsung::wasm::ElementaryMediaStreamSource::ReadyState;
using EmssOperationResult = samsung::wasm::OperationResult;
using EmssAsyncResult = samsung::wasm::OperationResult;
using HTMLAsyncResult = samsung::wasm::OperationResult;
using TimeStamp = samsung::wasm::Seconds;

static constexpr TimeStamp kFrameTimeMargin = 0.5ms;
static constexpr TimeStamp kTimeWindow = 1s;
static constexpr uint32_t kSampleRate = 48000;

static bool s_FramePacingEnabled = false;

static uint32_t s_Width = 0;
static uint32_t s_Height = 0;
static uint32_t s_Framerate = 0;

static std::vector<unsigned char> s_DecodeBuffer;

static TimeStamp s_frameDuration;
static TimeStamp s_pktPts;

static TimeStamp s_ptsDiff;
static TimeStamp s_lastSec;

static std::chrono::time_point<std::chrono::steady_clock> s_firstAppend;
static std::chrono::time_point<std::chrono::steady_clock> s_lastTime;
static bool s_hasFirstFrame = false;

// Set to true if the one-time media pipeline setup failed (e.g. the TV never
// opened the source or a track could not be added). VidDecSetup uses this to
// fail the connection cleanly instead of feeding packets into a dead pipeline.
static bool s_VideoSetupFailed = false;

// How long to wait for each media pipeline state transition before giving up.
static constexpr std::chrono::milliseconds kSetupTimeout{10000};

MoonlightInstance::SourceListener::SourceListener(
    MoonlightInstance* instance)
  : m_Instance(instance) {}

void MoonlightInstance::SourceListener::OnSourceClosed() {
  ClLogMessage("EMSS::OnClosed\n");
  bool wasStreaming;
  {
    std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
    m_Instance->m_EmssReadyState = EmssReadyState::kClosed;
    m_Instance->m_EmssStateChanged.notify_all();
    // Only treat this as unexpected if video was actually playing. During
    // normal setup the source legitimately reaches the closed state, and
    // during a user-requested stop m_Running is already false.
    wasStreaming = m_Instance->m_VideoStarted.load() && m_Instance->m_Running;
  }
  // If the source closes while we're still streaming, the TV media pipeline
  // died unexpectedly (decoder error, HDR switch, etc). Instead of leaving a
  // frozen black screen that forces a TV reboot, ask the front end to tear the
  // stream down and return to the menu.
  if (wasStreaming) {
    ClLogMessage("EMSS closed during active stream, notifying front end\n");
    PostToJs(std::string("streamError"));
  }
}

void MoonlightInstance::SourceListener::OnSourceOpenPending() {
  ClLogMessage("EMSS::OnOpenPending\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_EmssReadyState = EmssReadyState::kOpenPending;
  m_Instance->m_EmssStateChanged.notify_all();
}

void MoonlightInstance::SourceListener::OnSourceOpen() {
  ClLogMessage("EMSS::OnOpen\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_EmssReadyState = EmssReadyState::kOpen;
  m_Instance->m_EmssStateChanged.notify_all();
}

MoonlightInstance::AudioTrackListener::AudioTrackListener(
    MoonlightInstance* instance)
  : m_Instance(instance) {}

void MoonlightInstance::AudioTrackListener::OnTrackOpen() {
  ClLogMessage("AUDIO ElementaryMediaTrack::OnTrackOpen\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_AudioStarted = true;
  m_Instance->m_EmssAudioStateChanged.notify_all();
}

void MoonlightInstance::AudioTrackListener::OnTrackClosed(
    samsung::wasm::ElementaryMediaTrack::CloseReason) {
  ClLogMessage("AUDIO ElementaryMediaTrack::OnTrackClosed\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_AudioStarted = false;
}

void MoonlightInstance::AudioTrackListener::OnSessionIdChanged(
    samsung::wasm::SessionId new_session_id) {
  ClLogMessage("AUDIO ElementaryMediaTrack::OnSessionIdChanged\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_AudioSessionId.store(new_session_id);
}

MoonlightInstance::VideoTrackListener::VideoTrackListener(
    MoonlightInstance* instance)
  : m_Instance(instance) {}

void MoonlightInstance::VideoTrackListener::OnTrackOpen() {
  ClLogMessage("VIDEO ElementaryMediaTrack::OnTrackOpen\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_VideoStarted = true;
  m_Instance->m_EmssVideoStateChanged.notify_all();
  LiRequestIdrFrame();
}

void MoonlightInstance::VideoTrackListener::OnTrackClosed(
    samsung::wasm::ElementaryMediaTrack::CloseReason) {
  ClLogMessage("VIDEO ElementaryMediaTrack::OnTrackClosed\n");
  bool wasStreaming;
  {
    std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
    m_Instance->m_VideoStarted = false;
    wasStreaming = m_Instance->m_Running;
  }
  // If the video track closes while we're still streaming (decoder error on the
  // TV that doesn't also close the whole source), the picture freezes with no
  // other event to react to. Ask the front end to tear down and return to the
  // menu instead of leaving a frozen black screen. During a normal stop
  // m_Running is already false, so this won't fire spuriously.
  if (wasStreaming) {
    ClLogMessage("Video track closed during active stream, notifying front end\n");
    PostToJs(std::string("streamError"));
  }
}

void MoonlightInstance::VideoTrackListener::OnSessionIdChanged(
    samsung::wasm::SessionId new_session_id) {
  ClLogMessage("VIDEO ElementaryMediaTrack::OnSessionIdChanged\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_VideoSessionId.store(new_session_id);
}

void MoonlightInstance::DidChangeFocus(bool got_focus) {
  // Request an IDR frame to dump the frame queue that may have
  // built up from the GL pipeline being stalled.
  if (got_focus) {
    LiRequestIdrFrame();
  }
}

bool MoonlightInstance::InitializeRenderingSurface(int width, int height) {
  return true;
}

int MoonlightInstance::StartupVidDecSetup(int videoFormat, int width,
int height, int redrawRate, void* context, int drFlags) {
  g_Instance->m_MediaElement.SetSrc(&g_Instance->m_Source);
  ClLogMessage("Waiting for closed\n");
  if (!g_Instance->WaitForTimeout(&g_Instance->m_EmssStateChanged, [] {
      return g_Instance->m_EmssReadyState == EmssReadyState::kClosed;
  }, kSetupTimeout)) {
    ClLogMessage("Timed out waiting for source to close\n");
    s_VideoSetupFailed = true;
    return -1;
  }
  ClLogMessage("closed done\n");

  {
    auto add_track_result = g_Instance->m_Source.AddTrack(
      samsung::wasm::ElementaryAudioTrackConfig {
        "audio/webm; codecs=\"pcm\"",  // mimeType
        {},  // extradata (empty?)
        samsung::wasm::SampleFormat::kS16,
        samsung::wasm::ChannelLayout::kStereo,
        kSampleRate
      });
    if (add_track_result) {
      g_Instance->m_AudioTrack = std::move(*add_track_result);
      g_Instance->m_AudioTrack.SetListener(&g_Instance->m_AudioTrackListener);
    } else {
      ClLogMessage("Failed to add audio track\n");
      s_VideoSetupFailed = true;
      return -1;
    }
  }

  {
    const char *mimetype = "video/mp4";
    if(videoFormat & VIDEO_FORMAT_H265_MAIN10) {
      mimetype = "video/mp4; codecs=\"hev1.2.4.L153.B0\"";  // h265 main10 mimeType	: hev1.2.4.L153.B0 can be updated to hev1.2.6.L153.B0 depending on TV capabilities
    } else if(videoFormat & VIDEO_FORMAT_H265) {
      mimetype = "video/mp4; codecs=\"hev1.1.6.L93.B0\"";  // h265 main mimeType
    } else if(videoFormat & VIDEO_FORMAT_H264) {
      mimetype = "video/mp4; codecs=\"avc1.64002A\"";  // h264 High Profile 4.2 mimeType
    } else if (videoFormat & VIDEO_FORMAT_AV1_MAIN8) {
      mimetype = "video/mp4; codecs=\"av01.0.13M.08\"";  // AV1 Main Profile, level 5.1, Main tier, 8 bits
    } else if (videoFormat & VIDEO_FORMAT_AV1_MAIN10) {
      mimetype = "video/mp4; codecs=\"av01.0.13M.10\"";  // AV1 Main Profile, level 5.1, Main tier, 10 bits
    }
    else {
      ClLogMessage("Cannot select mime type for videoFormat=0x%x\n", videoFormat);
      s_VideoSetupFailed = true;
      return -1;
    }

    ClLogMessage("Using mimeType %s\n", mimetype);
    auto add_track_result = g_Instance->m_Source.AddTrack(
      samsung::wasm::ElementaryVideoTrackConfig{
        mimetype,
        {},                                   // extradata (empty?)
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<uint32_t>(redrawRate),  // framerateNum
        1,                                  // framerateDen
      });
    if (add_track_result) {
      g_Instance->m_VideoTrack = std::move(*add_track_result);
      g_Instance->m_VideoTrack.SetListener(&g_Instance->m_VideoTrackListener);
    } else {
      ClLogMessage("Failed to add video track\n");
      s_VideoSetupFailed = true;
      return -1;
    }
  }

  ClLogMessage("Inb4 source open\n");
  g_Instance->m_Source.Open([](EmssOperationResult){});
  if (!g_Instance->WaitForTimeout(&g_Instance->m_EmssStateChanged, [] {
      return g_Instance->m_EmssReadyState == EmssReadyState::kOpenPending;
  }, kSetupTimeout)) {
    ClLogMessage("Timed out waiting for source open pending\n");
    s_VideoSetupFailed = true;
    return -1;
  }
  ClLogMessage("Source ready to open\n");
  g_Instance->m_MediaElement.Play([](EmssOperationResult err) {
    if (err != EmssOperationResult::kSuccess) {
      ClLogMessage("Play error\n");
    } else {
      ClLogMessage("Play success\n");
    }
  });

  ClLogMessage("Waiting for start\n");
  if (!g_Instance->WaitForTimeout(&g_Instance->m_EmssAudioStateChanged,
                      [] { return g_Instance->m_AudioStarted.load(); }, kSetupTimeout)) {
    ClLogMessage("Timed out waiting for audio track to start\n");
    s_VideoSetupFailed = true;
    return -1;
  }

  if (!g_Instance->WaitForTimeout(&g_Instance->m_EmssVideoStateChanged,
                      [] { return g_Instance->m_VideoStarted.load(); }, kSetupTimeout)) {
    ClLogMessage("Timed out waiting for video track to start\n");
    s_VideoSetupFailed = true;
    return -1;
  }
  ClLogMessage("started\n");
  return 0;
}

int MoonlightInstance::VidDecSetup(int videoFormat, int width, int height,
int redrawRate, void* context, int drFlags) {
  ClLogMessage("MoonlightInstance::VidDecSetup\n");
  s_DecodeBuffer.resize(INITIAL_DECODE_BUFFER_LEN);

  s_Width = width;
  s_Height = height;
  s_Framerate = redrawRate;

  s_frameDuration = TimeStamp(1.0 / redrawRate);
  s_pktPts = 0s;
  s_hasFirstFrame = false;
  s_lastSec = 0s;
  s_ptsDiff = 0s;

  s_FramePacingEnabled = g_Instance->m_FramePacingEnabled;

  static std::once_flag once_flag;
  std::call_once(once_flag, &MoonlightInstance::StartupVidDecSetup,
  videoFormat, width, height, redrawRate, context, drFlags);
  if (s_VideoSetupFailed) {
    ClLogMessage("Video pipeline setup failed, aborting connection\n");
    return -1;
  }
  return DR_OK;
}

void MoonlightInstance::VidDecCleanup(void) {
  s_DecodeBuffer.clear();
  s_DecodeBuffer.shrink_to_fit();
}

int MoonlightInstance::VidDecSubmitDecodeUnit(PDECODE_UNIT decodeUnit) {
  // ClLogMessage("MoonlightInstance::VidDecSubmitDecodeUnit\n");

  if (!g_Instance->m_VideoStarted)
    return DR_OK;

  PLENTRY entry;
  unsigned int offset;
  unsigned int totalLength;
  // ClLogMessage("Video packet append at: %f\n", s_pktPts);

  totalLength = decodeUnit->fullLength;
  // Resize the decode buffer if needed
  if (totalLength > s_DecodeBuffer.size()) {
    s_DecodeBuffer.resize(totalLength);
  }

  entry = decodeUnit->bufferList;
  offset = 0;
  while (entry != NULL) {
    memcpy(&s_DecodeBuffer[offset], entry->data, entry->length);
    offset += entry->length;
    entry = entry->next;
  }

  auto now = std::chrono::steady_clock::now();
  if (!s_hasFirstFrame) {
    s_firstAppend = std::chrono::steady_clock::now();
    s_hasFirstFrame = true;
  } else if (s_FramePacingEnabled) {
    TimeStamp fromStart = now - s_firstAppend;

    while (s_pktPts > fromStart - s_ptsDiff + kFrameTimeMargin) {
      // Yield instead of busy-spinning so we don't peg a CPU core on the TV's
      // weak ARM chip (which would steal cycles from the decoder and stutter).
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      now = std::chrono::steady_clock::now();
      fromStart = now - s_firstAppend;
    }

    if (fromStart > s_lastSec + kTimeWindow) {
      s_lastSec += kTimeWindow;
      s_ptsDiff = fromStart - s_pktPts;
    }
  }
  s_lastTime = now;

  // Start the decoding
  samsung::wasm::ElementaryMediaPacket pkt{
    s_pktPts,
    s_pktPts,
    s_frameDuration,
    decodeUnit->frameType == FRAME_TYPE_IDR,
    offset,
    s_DecodeBuffer.data(),
    s_Width,
    s_Height,
    s_Framerate,
    1,
    g_Instance->m_VideoSessionId.load()
  };

  if (g_Instance->m_VideoTrack.AppendPacket(pkt)) {
    s_pktPts += s_frameDuration;
  } else {
    ClLogMessage("Append video packet failed\n");
    return DR_NEED_IDR;
  }

  return DR_OK;
}

void MoonlightInstance::WaitFor(std::condition_variable* variable,
std::function<bool()> condition) {
  std::unique_lock<std::mutex> lock(m_Mutex);
  variable->wait(lock, condition);
}

bool MoonlightInstance::WaitForTimeout(std::condition_variable* variable,
std::function<bool()> condition, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(m_Mutex);
  // Returns true if the predicate is satisfied, false if the timeout elapsed.
  return variable->wait_for(lock, timeout, condition);
}

DECODER_RENDERER_CALLBACKS MoonlightInstance::s_DrCallbacks = {
  .setup = MoonlightInstance::VidDecSetup,
  .cleanup = MoonlightInstance::VidDecCleanup,
  .submitDecodeUnit = MoonlightInstance::VidDecSubmitDecodeUnit,
  .capabilities = CAPABILITY_SLICES_PER_FRAME(4)
};
