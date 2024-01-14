#include <atomic>
#include <memory>
#include <queue>

#include <emscripten/bind.h>
#include <emscripten/html5.h>
#include <emscripten/val.h>
#include <pthread.h>
#include <future>
#include <string>

#include <Limelight.h>
#include <opus_multistream.h>

#include "lib.hpp"

#include "samsung/wasm/elementary_media_stream_source.h"
#include "samsung/wasm/elementary_media_stream_source_listener.h"
#include "samsung/wasm/elementary_media_track.h"
#include "samsung/wasm/elementary_media_track_listener.h"
#include "samsung/html/html_media_element.h"
#include "samsung/html/html_media_element_listener.h"

// Uncomment this line to enable the profiling infrastructure
// #define ENABLE_PROFILING 1

// Use this define to choose the time threshold in milliseconds above
// which a profiling message is printed
#define PROFILING_MESSAGE_THRESHOLD 1

#define DR_FLAG_FORCE_SW_DECODE 0x01

// These will mostly be I/O bound so we'll create
// a bunch to allow more concurrent server requests
// since our HTTP request libary is synchronous.
#define HTTP_HANDLER_THREADS 8

struct MessageResult {
  std::string type;
  emscripten::val ret;

  MessageResult(std::string t = "", emscripten::val r = emscripten::val::null())
      : type(t), ret(r) {}

  static MessageResult Resolve(emscripten::val r = emscripten::val::null()) {
    return {"resolve", r};
  }
  static MessageResult Reject(emscripten::val r = emscripten::val::null()) {
    return {"reject", r};
  }
};

enum class LoadResult { Success, CertErr, PrivateKeyErr };

constexpr const char* kCanvasName = "#nacl_module";

class MoonlightInstance {
public:
  explicit MoonlightInstance();

  MessageResult StartStream(std::string host, std::string width,
  std::string height, std::string fps, std::string bitrate, std::string rikey,
  std::string rikeyid, std::string appversion, std::string gfeversion, bool framePacing,
  bool audioSync, std::string rtspurl, bool hdrEnabled);
  MessageResult StopStream();

  void STUN(int callbackId);
  void Pair(int callbackId, std::string serverMajorVersion, std::string address, std::string randomNumber);

  virtual ~MoonlightInstance();

  bool Init(uint32_t argc, const char* argn[], const char* argv[]);

  EM_BOOL HandleMouseDown(const EmscriptenMouseEvent& event);
  EM_BOOL HandleMouseMove(const EmscriptenMouseEvent& event);
  EM_BOOL HandleMouseUp(const EmscriptenMouseEvent& event);
  EM_BOOL HandleWheel(const EmscriptenWheelEvent& event);
  EM_BOOL HandleKeyDown(const EmscriptenKeyboardEvent& event);
  EM_BOOL HandleKeyUp(const EmscriptenKeyboardEvent& event);

  void ReportMouseMovement();

  void PollGamepads();

  void MouseLockLost();
  void DidLockMouse(int32_t result);

  void OnConnectionStopped(uint32_t unused);
  void OnConnectionStarted(uint32_t error);
  void StopConnection();

  static uint32_t ProfilerGetPackedMillis();
  static uint64_t ProfilerGetMillis();
  static uint64_t ProfilerUnpackTime(uint32_t packedTime);
  static void ProfilerPrintPackedDelta(const char* message, uint32_t packedTimeA, uint32_t packedTimeB);
  static void ProfilerPrintDelta(const char* message, uint64_t timeA, uint64_t timeB);
  static void ProfilerPrintPackedDeltaFromNow(const char* message, uint32_t packedTime);
  static void ProfilerPrintDeltaFromNow(const char* message, uint64_t time);
  static void ProfilerPrintWarning(const char* message);

  static void* ConnectionThreadFunc(void* context);
  static void* InputThreadFunc(void* context);
  static void* StopThreadFunc(void* context);

  static void ClStageStarting(int stage);
  static void ClStageFailed(int stage, int errorCode);
  static void ClConnectionStarted(void);
  static void ClConnectionTerminated(int errorCode);
  static void ClDisplayMessage(const char* message);
  static void ClDisplayTransientMessage(const char* message);
  static void ClLogMessage(const char* format, ...);
  static void ClControllerRumble(unsigned short gamepadID, unsigned short lowFreqMotor, unsigned short highFreqMotor);

  void DidChangeFocus(bool got_focus);
  bool InitializeRenderingSurface(int width, int height);

  static int VidDecSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags);
  static int StartupVidDecSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags);
  static void VidDecCleanup(void);
  static int VidDecSubmitDecodeUnit(PDECODE_UNIT decodeUnit);

  static int AudDecInit(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int flags);
  static void AudDecCleanup(void);
  static void AudDecDecodeAndPlaySample(char* sampleData, int sampleLength);

  MessageResult MakeCert();

  MessageResult HttpInit(std::string cert, std::string privateKey, std::string myUniqueId);
  void OpenUrl(int callbackId, std::string url, std::string ppk, bool binaryResponse);

  LoadResult LoadCert(const char* certStr, const char* keyStr);

  private:
    using EmssReadyState = samsung::wasm::ElementaryMediaStreamSource::ReadyState;
    using EmssTrackCloseReason = samsung::wasm::ElementaryMediaTrack::CloseReason;

  class SourceListener
      : public samsung::wasm::ElementaryMediaStreamSourceListener {
  public:
    SourceListener(MoonlightInstance* instance);
    void OnSourceClosed() override;
    void OnSourceOpenPending() override;
    void OnSourceOpen() override;

  private:
    MoonlightInstance* m_Instance;
  };

  class AudioTrackListener
      : public samsung::wasm::ElementaryMediaTrackListener {
  public:
    AudioTrackListener(MoonlightInstance* instance);
    void OnTrackOpen() override;
    void OnTrackClosed(EmssTrackCloseReason) override;
    void OnSessionIdChanged(samsung::wasm::SessionId new_session_id) override;

  private:
    MoonlightInstance* m_Instance;
  };

  class VideoTrackListener
      : public samsung::wasm::ElementaryMediaTrackListener {
  public:
    VideoTrackListener(MoonlightInstance* instance);
    void OnTrackOpen() override;
    void OnTrackClosed(EmssTrackCloseReason) override;
    void OnSessionIdChanged(samsung::wasm::SessionId new_session_id) override;

  private:
    MoonlightInstance* m_Instance;
  };

  void WaitFor(std::condition_variable* variable, std::function<bool()> condition);

  void OpenUrl_private(int callbackId, std::string url, std::string ppk, bool binaryResponse);
  void STUN_private(int callbackId);
  void Pair_private(int callbackId, std::string serverMajorVersion, std::string address, std::string randomNumber);

  void LockMouse();
  void UnlockMouse();

  static CONNECTION_LISTENER_CALLBACKS s_ClCallbacks;
  static DECODER_RENDERER_CALLBACKS s_DrCallbacks;
  static AUDIO_RENDERER_CALLBACKS s_ArCallbacks;

  std::string m_Host;
  std::string m_AppVersion;
  std::string m_GfeVersion;
  std::string m_RtspUrl;
  bool m_FramePacingEnabled;
  bool m_AudioSyncEnabled;
  bool m_HdrEnabled;
  STREAM_CONFIGURATION m_StreamConfig;
  bool m_Running;

  pthread_t m_ConnectionThread;
  pthread_t m_InputThread;

  OpusMSDecoder* m_OpusDecoder;

#ifndef SAMSUNG_TIZEN_TV
  double m_LastPadTimestamps[4];
#endif
  bool m_MouseLocked;
  long m_MouseLastPosX;
  long m_MouseLastPosY;
  bool m_WaitingForAllModifiersUp;
  float m_AccumulatedTicks;
  int32_t m_MouseDeltaX, m_MouseDeltaY;
  uint32_t m_HttpThreadPoolSequence;

  Dispatcher m_Dispatcher;

  std::mutex m_Mutex;
  std::condition_variable m_EmssStateChanged;
  std::condition_variable m_EmssAudioStateChanged;
  std::condition_variable m_EmssVideoStateChanged;
  EmssReadyState m_EmssReadyState;
  std::atomic<bool> m_AudioStarted;
  std::atomic<bool> m_VideoStarted;
  std::atomic<samsung::wasm::SessionId> m_AudioSessionId;
  std::atomic<samsung::wasm::SessionId> m_VideoSessionId;
  samsung::html::HTMLMediaElement m_MediaElement;
  samsung::wasm::ElementaryMediaStreamSource m_Source;
  SourceListener m_SourceListener;
  AudioTrackListener m_AudioTrackListener;
  VideoTrackListener m_VideoTrackListener;
  samsung::wasm::ElementaryMediaTrack m_VideoTrack;
  samsung::wasm::ElementaryMediaTrack m_AudioTrack;
};

extern MoonlightInstance* g_Instance;

void PostToJs(std::string msg);
void PostPromiseMessage(int callbackId, const std::string& type, const std::string& response);
void PostPromiseMessage(int callbackId, const std::string& type, const std::vector<uint8_t>& response);

MessageResult makeCert();

MessageResult httpInit(std::string cert, std::string privateKey, std::string myUniqueId);
void openUrl(int callbackId, std::string url, emscripten::val ppk, bool binaryResponse);

MessageResult startStream(std::string host, std::string width, std::string height, std::string fps,
std::string bitrate, std::string rikey, std::string rikeyid, std::string appversion,
std::string gfeversion, bool framePacing, bool audioSync,  std::string rtspurl, bool hdrEnabled);
MessageResult stopStream();

void stun(int callbackId);
void pair(int callbackId, std::string serverMajorVersion, std::string address, std::string randomNumber);

EM_BOOL handleKeyDown(int eventType, const EmscriptenKeyboardEvent* keyEvent, void* userData);
EM_BOOL handleKeyUp(int eventType, const EmscriptenKeyboardEvent* keyEvent, void* userData);
EM_BOOL handleMouseMove(int eventType, const EmscriptenMouseEvent* keyEvent, void* userData);
EM_BOOL handleMouseUp(int eventType, const EmscriptenMouseEvent* keyEvent, void* userData);
EM_BOOL handleMouseDown(int eventType, const EmscriptenMouseEvent* keyEvent, void* userData);
EM_BOOL handleWheel(int eventType, const EmscriptenWheelEvent* keyEvent, void* userData);
EM_BOOL handlePointerLockChange(int eventType, const EmscriptenPointerlockChangeEvent *pointerlockChangeEvent, void *userData);
EM_BOOL handlePointerLockError(int eventType, const void *reserved, void *userData);

void onConnectionStarted();
void onConnectionStopped(int errorCode);
