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

add_library(opus STATIC
    opus/celt/bands.c
    opus/celt/celt.c
    opus/celt/celt_decoder.c
    opus/celt/celt_encoder.c
    opus/celt/celt_lpc.c
    opus/celt/cwrs.c
    opus/celt/entcode.c
    opus/celt/entdec.c
    opus/celt/entenc.c
    opus/celt/kiss_fft.c
    opus/celt/laplace.c
    opus/celt/mathops.c
    opus/celt/mdct.c
    opus/celt/modes.c
    opus/celt/pitch.c
    opus/celt/quant_bands.c
    opus/celt/rate.c
    opus/celt/vq.c
    opus/silk/A2NLSF.c
    opus/silk/CNG.c
    opus/silk/HP_variable_cutoff.c
    opus/silk/LPC_analysis_filter.c
    opus/silk/LPC_fit.c
    opus/silk/LPC_inv_pred_gain.c
    opus/silk/LP_variable_cutoff.c
    opus/silk/NLSF2A.c
    opus/silk/NLSF_VQ.c
    opus/silk/NLSF_VQ_weights_laroia.c
    opus/silk/NLSF_decode.c
    opus/silk/NLSF_del_dec_quant.c
    opus/silk/NLSF_encode.c
    opus/silk/NLSF_stabilize.c
    opus/silk/NLSF_unpack.c
    opus/silk/NSQ.c
    opus/silk/NSQ_del_dec.c
    opus/silk/PLC.c
    opus/silk/VAD.c
    opus/silk/VQ_WMat_EC.c
    opus/silk/ana_filt_bank_1.c
    opus/silk/biquad_alt.c
    opus/silk/bwexpander.c
    opus/silk/bwexpander_32.c
    opus/silk/check_control_input.c
    opus/silk/code_signs.c
    opus/silk/control_SNR.c
    opus/silk/control_audio_bandwidth.c
    opus/silk/control_codec.c
    opus/silk/debug.c
    opus/silk/dec_API.c
    opus/silk/decode_core.c
    opus/silk/decode_frame.c
    opus/silk/decode_indices.c
    opus/silk/decode_parameters.c
    opus/silk/decode_pitch.c
    opus/silk/decode_pulses.c
    opus/silk/decoder_set_fs.c
    opus/silk/enc_API.c
    opus/silk/encode_indices.c
    opus/silk/encode_pulses.c
    opus/silk/fixed/LTP_analysis_filter_FIX.c
    opus/silk/fixed/LTP_scale_ctrl_FIX.c
    opus/silk/fixed/apply_sine_window_FIX.c
    opus/silk/fixed/autocorr_FIX.c
    opus/silk/fixed/burg_modified_FIX.c
    opus/silk/fixed/corrMatrix_FIX.c
    opus/silk/fixed/encode_frame_FIX.c
    opus/silk/fixed/find_LPC_FIX.c
    opus/silk/fixed/find_LTP_FIX.c
    opus/silk/fixed/find_pitch_lags_FIX.c
    opus/silk/fixed/find_pred_coefs_FIX.c
    opus/silk/fixed/k2a_FIX.c
    opus/silk/fixed/k2a_Q16_FIX.c
    opus/silk/fixed/noise_shape_analysis_FIX.c
    opus/silk/fixed/pitch_analysis_core_FIX.c
    opus/silk/fixed/process_gains_FIX.c
    opus/silk/fixed/regularize_correlations_FIX.c
    opus/silk/fixed/residual_energy16_FIX.c
    opus/silk/fixed/residual_energy_FIX.c
    opus/silk/fixed/schur64_FIX.c
    opus/silk/fixed/schur_FIX.c
    opus/silk/fixed/vector_ops_FIX.c
    opus/silk/fixed/warped_autocorrelation_FIX.c
    opus/silk/gain_quant.c
    opus/silk/init_decoder.c
    opus/silk/init_encoder.c
    opus/silk/inner_prod_aligned.c
    opus/silk/interpolate.c
    opus/silk/lin2log.c
    opus/silk/log2lin.c
    opus/silk/pitch_est_tables.c
    opus/silk/process_NLSFs.c
    opus/silk/quant_LTP_gains.c
    opus/silk/resampler.c
    opus/silk/resampler_down2.c
    opus/silk/resampler_down2_3.c
    opus/silk/resampler_private_AR2.c
    opus/silk/resampler_private_IIR_FIR.c
    opus/silk/resampler_private_down_FIR.c
    opus/silk/resampler_private_up2_HQ.c
    opus/silk/resampler_rom.c
    opus/silk/shell_coder.c
    opus/silk/sigm_Q15.c
    opus/silk/sort.c
    opus/silk/stereo_LR_to_MS.c
    opus/silk/stereo_MS_to_LR.c
    opus/silk/stereo_decode_pred.c
    opus/silk/stereo_encode_pred.c
    opus/silk/stereo_find_predictor.c
    opus/silk/stereo_quant_pred.c
    opus/silk/sum_sqr_shift.c
    opus/silk/table_LSF_cos.c
    opus/silk/tables_LTP.c
    opus/silk/tables_NLSF_CB_NB_MB.c
    opus/silk/tables_NLSF_CB_WB.c
    opus/silk/tables_gain.c
    opus/silk/tables_other.c
    opus/silk/tables_pitch_lag.c
    opus/silk/tables_pulses_per_block.c
    opus/src/analysis.c
    opus/src/mlp.c
    opus/src/mlp_data.c
    opus/src/opus.c
    opus/src/opus_decoder.c
    opus/src/opus_encoder.c
    opus/src/opus_multistream.c
    opus/src/opus_multistream_decoder.c
    opus/src/opus_multistream_encoder.c
    opus/src/repacketizer.c
)
target_include_directories(opus PUBLIC
    opus/include
    opus/src
    opus/silk
    opus/celt
    opus/silk/fixed)
target_compile_definitions(opus PUBLIC
     USE_ALLOCA
     HAVE_LRINT
     HAVE_LRINTF
     FIXED_POINT
     OPUS_BUILD)

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
