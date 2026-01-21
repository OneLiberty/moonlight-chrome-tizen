cmake_minimum_required(VERSION 3.10)

project(MoonlightWasm VERSION 1.0)

add_compile_options(-w -s WASM=1 -fno-ident -Os -flto -DSAMSUNG_WRT -DOS_WASM -DNDEBUG)

# Preload the CA bundle
set(CA_BUNDLE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/static/curl/ca-bundle.crt)
add_custom_command(
    OUTPUT ${CA_BUNDLE_PATH}
    COMMAND ${CMAKE_COMMAND} -E copy ${CA_BUNDLE_PATH} ${CMAKE_CURRENT_BINARY_DIR}
)
add_custom_target(preload_ca_bundle ALL DEPENDS ${CA_BUNDLE_PATH})
set(EMSCRIPTEN_PRELOAD_FILES "${CA_BUNDLE_PATH}@/static/curl/ca-bundle.crt")

add_library(moonlight-rs STATIC
    moonlight-common-c/reedsolomon/rs.c)
target_include_directories(moonlight-rs PUBLIC
    moonlight-common-c/reedsolomon)
target_compile_features(moonlight-rs PUBLIC c_std_99)

add_library(moonlight-enet STATIC
    moonlight-common-c/enet/callbacks.c
    moonlight-common-c/enet/compress.c
    moonlight-common-c/enet/host.c
    moonlight-common-c/enet/list.c
    moonlight-common-c/enet/packet.c
    moonlight-common-c/enet/peer.c
    moonlight-common-c/enet/protocol.c
    moonlight-common-c/enet/unix.c
)
target_compile_definitions(moonlight-enet PUBLIC
    LC_CHROME
    HAS_SOCKLEN_T=1
    HAS_FCNTL=1
    NO_MSGAPI=1
    HAS_SOCKLEN_T=1)
target_include_directories(moonlight-enet PUBLIC
    moonlight-common-c/enet/include)

add_library(moonlight-common-c STATIC
    moonlight-common-c/src/AudioStream.c
    moonlight-common-c/src/ByteBuffer.c
    moonlight-common-c/src/Connection.c
    moonlight-common-c/src/ControlStream.c
    moonlight-common-c/src/FakeCallbacks.c
    moonlight-common-c/src/InputStream.c
    moonlight-common-c/src/LinkedBlockingQueue.c
    moonlight-common-c/src/Misc.c
    moonlight-common-c/src/Platform.c
    moonlight-common-c/src/PlatformCrypto.c
    moonlight-common-c/src/PlatformSockets.c
    moonlight-common-c/src/RtpAudioQueue.c
    moonlight-common-c/src/RtpVideoQueue.c
    moonlight-common-c/src/RtspConnection.c
    moonlight-common-c/src/RtspParser.c
    moonlight-common-c/src/SdpGenerator.c
    moonlight-common-c/src/SimpleStun.c
    moonlight-common-c/src/VideoDepacketizer.c
    moonlight-common-c/src/VideoStream.c
)
target_compile_definitions(moonlight-common-c PUBLIC
    LC_CHROME)
target_include_directories(moonlight-common-c PUBLIC
    moonlight-common-c/src
    moonlight-common-c/enet/include
    moonlight-common-c/reedsolomon)
target_compile_features(moonlight-common-c PUBLIC c_std_99)
set_target_properties(moonlight-common-c
    PROPERTIES
        COMPILE_FLAGS
            "-s USE_CRYPTO=1 -s USE_SSL=1")

add_library(libgamestream STATIC
    libgamestream/http.c
    libgamestream/mkcert.c
    libgamestream/pairing.c
)
target_include_directories(libgamestream PUBLIC
    libgamestream)
set_target_properties(libgamestream
    PROPERTIES
        COMPILE_FLAGS
            "-s USE_CRYPTO=1 -s USE_SSL=1 -s USE_CURL=1")

add_library(h264bitstream STATIC
    h264bitstream/h264_nal.c
    h264bitstream/h264_sei.c
    h264bitstream/h264_stream.c
)
target_include_directories(h264bitstream PUBLIC
    h264bitstream)
target_compile_features(h264bitstream PUBLIC c_std_99)

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)

set(OPUS_DISABLE_FLOAT_API ON CACHE BOOL "" FORCE)
set(OPUS_FIXED_POINT ON CACHE BOOL "" FORCE)
set(OPUS_USE_ALLOCA OFF CACHE BOOL "" FORCE)
set(OPUS_USE_CONFIG_H OFF CACHE BOOL "" FORCE)

add_subdirectory(opus)

target_compile_definitions(opus PUBLIC
    OPUS_BUILD
    FIXED_POINT
    DISABLE_FLOAT_API
)

add_executable(moonlight-wasm
    libchelper.c
    profiling.cpp
    wasm/auddec.cpp
    wasm/connectionlistener.cpp
    wasm/gamepad.cpp
    wasm/http.cpp
    wasm/input.cpp
    wasm/main.cpp
    wasm/wasmplayer.cpp
)
target_include_directories(moonlight-wasm PUBLIC
    wasm
    wasm/dispatcher
    moonlight-common-c/src
    moonlight-common-c/enet/include
    moonlight-common-c/reedsolomon
    libgamestream
    h264bitstream
    opus/include
    opus/src
    opus/silk
    opus/silk/fixed
    opus/celt
)
set_target_properties(moonlight-wasm
    PROPERTIES
        COMPILE_FLAGS
            "-s USE_CRYPTO=1 -s USE_SSL=1 -s USE_CURL=1")
target_compile_features(moonlight-wasm PUBLIC cxx_std_17)
set_target_properties(moonlight-wasm
    PROPERTIES
        LINK_FLAGS
            "--bind \
             -pthread \
             -Os \
             -flto \
             -s TOTAL_MEMORY=134217728 \
             -s TOTAL_STACK=16777216 \
             -s NO_EXIT_RUNTIME=1 \
             -s USE_PTHREADS=1 \
             -s PTHREAD_POOL_SIZE=20 \
             -s WASM=1 \
             -s ENVIRONMENT_MAY_BE_TIZEN \
             -s USE_CRYPTO=1 \
             -s USE_SSL=1 \
             -s USE_CURL=1")
target_link_libraries(moonlight-wasm PUBLIC
    libgamestream
    h264bitstream
    opus
    moonlight-common-c
    moonlight-enet
    moonlight-rs
)

# TODO(samsung) These files may change.
set(EMSCRIPTEN_OUTPUT_FILES
  "${CMAKE_CURRENT_BINARY_DIR}/moonlight-wasm.js.mem"
  "${CMAKE_CURRENT_BINARY_DIR}/moonlight-wasm.wasm"
  "${CMAKE_CURRENT_BINARY_DIR}/moonlight-wasm.worker.js"
)

install(TARGETS moonlight-wasm RUNTIME DESTINATION widget)
install(FILES ${EMSCRIPTEN_OUTPUT_FILES} DESTINATION widget OPTIONAL)
install(DIRECTORY static DESTINATION widget)
install(DIRECTORY wasm/platform DESTINATION widget)
install(FILES wasm/index.html DESTINATION widget)
install(FILES res/config.xml DESTINATION widget)
install(FILES res/platform.js DESTINATION widget)
