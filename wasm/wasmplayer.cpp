#include "moonlight_wasm.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>

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

MoonlightInstance::SourceListener::SourceListener(
    MoonlightInstance* instance)
  : m_Instance(instance) {}

void MoonlightInstance::SourceListener::OnSourceClosed() {
  ClLogMessage("EMSS::OnClosed\n");
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_EmssReadyState = EmssReadyState::kClosed;
  m_Instance->m_EmssStateChanged.notify_all();
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
  std::unique_lock<std::mutex> lock(m_Instance->m_Mutex);
  m_Instance->m_VideoStarted = false;
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
  g_Instance->WaitFor(&g_Instance->m_EmssStateChanged, [] {
      return g_Instance->m_EmssReadyState == EmssReadyState::kClosed;
  });
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
    }
  }

  {
    const char *mimetype = "video/mp4";
    if(videoFormat & VIDEO_FORMAT_H265_MAIN10) {
      mimetype = "video/mp4; codecs=\"hev1.2.6.L153.B0\"";  // h265 main10 mimeType	//FHEN : hev1.2.4.L153.B0 => hev1.2.6.L153.B0
    } else if(videoFormat & VIDEO_FORMAT_H265) {
      mimetype = "video/mp4; codecs=\"hev1.1.6.L93.B0\"";  // h265 main mimeType
    } else if(videoFormat & VIDEO_FORMAT_H264) {
      mimetype = "video/mp4; codecs=\"avc1.64002A\"";  // h264 High Profile 4.2 mimeType
    }
    else {
      ClLogMessage("Cannot select mime type for videoFormat=0x%x\n", videoFormat);
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
    }
  }

  ClLogMessage("Inb4 source open\n");
  g_Instance->m_Source.Open([](EmssOperationResult){});
  g_Instance->WaitFor(&g_Instance->m_EmssStateChanged, [] {
      return g_Instance->m_EmssReadyState == EmssReadyState::kOpenPending;
  });
  ClLogMessage("Source ready to open\n");
  g_Instance->m_MediaElement.Play([](EmssOperationResult err) {
    if (err != EmssOperationResult::kSuccess) {
      ClLogMessage("Play error\n");
    } else {
      ClLogMessage("Play success\n");
    }
  });

  ClLogMessage("Waiting for start\n");
  g_Instance->WaitFor(&g_Instance->m_EmssAudioStateChanged,
                      [] { return g_Instance->m_AudioStarted.load(); });

  g_Instance->WaitFor(&g_Instance->m_EmssVideoStateChanged,
                      [] { return g_Instance->m_VideoStarted.load(); });
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

DECODER_RENDERER_CALLBACKS MoonlightInstance::s_DrCallbacks = {
  .setup = MoonlightInstance::VidDecSetup,
  .cleanup = MoonlightInstance::VidDecCleanup,
  .submitDecodeUnit = MoonlightInstance::VidDecSubmitDecodeUnit,
  .capabilities = CAPABILITY_SLICES_PER_FRAME(4)
};
