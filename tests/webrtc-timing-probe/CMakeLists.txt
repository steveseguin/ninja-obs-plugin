cmake_minimum_required(VERSION 3.20)
project(webrtc_timing_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(WEBRTC_SOURCE "" CACHE PATH "Pinned WebRTC source checkout")
set(ABSEIL_SOURCE "" CACHE PATH "Abseil 20250814.1 checkout")
execute_process(COMMAND git -C "${WEBRTC_SOURCE}" rev-parse HEAD
    OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT revision STREQUAL "b47e68e6966d5a5a0e4bc861ff364221600f31c3")
    message(FATAL_ERROR "Use the Chromium 145 pinned WebRTC revision")
endif()
execute_process(COMMAND git -C "${ABSEIL_SOURCE}" rev-parse HEAD
    OUTPUT_VARIABLE abseil_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT abseil_revision STREQUAL "d38452e1ee03523a208362186fd42248ff2609f6")
    message(FATAL_ERROR "Use Abseil 20250814.1")
endif()
add_subdirectory("${ABSEIL_SOURCE}" abseil EXCLUDE_FROM_ALL)
set(W "${WEBRTC_SOURCE}")
add_library(timing-support STATIC
 ${W}/modules/video_coding/timing/timestamp_extrapolator.cc
 ${W}/modules/video_coding/timing/decode_time_percentile_filter.cc
 ${W}/rtc_base/checks.cc
 ${W}/rtc_base/logging.cc
 ${W}/rtc_base/string_encode.cc
 ${W}/rtc_base/string_utils.cc
 ${W}/rtc_base/strings/string_builder.cc
 ${W}/rtc_base/platform_thread_types.cc
 ${W}/rtc_base/time_utils.cc
 ${W}/rtc_base/system_time.cc
 ${W}/rtc_base/experiments/field_trial_parser.cc
 ${W}/rtc_base/experiments/field_trial_units.cc
 ${W}/rtc_base/experiments/struct_parameters_parser.cc
 ${W}/api/units/time_delta.cc
 ${W}/api/units/timestamp.cc
 ${W}/api/units/data_rate.cc
 ${W}/api/units/data_size.cc
 ${W}/system_wrappers/source/clock.cc
 ${W}/system_wrappers/source/metrics.cc
)
target_include_directories(timing-support PUBLIC ${W})
target_compile_definitions(timing-support PUBLIC WEBRTC_POSIX WEBRTC_LINUX NDEBUG)
target_compile_options(timing-support PRIVATE -ffunction-sections -fdata-sections)
target_link_options(timing-support INTERFACE -Wl,--gc-sections)
target_link_libraries(timing-support PUBLIC absl::strings absl::optional absl::inlined_vector pthread)

foreach(variant baseline bootstrap)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/instrument.py"
        "${W}/modules/video_coding/timing/timing.cc" "${CMAKE_CURRENT_BINARY_DIR}/timing-${variant}.cc" "${variant}"
        COMMAND_ERROR_IS_FATAL ANY)
    add_executable(timing-${variant} main.cpp "${CMAKE_CURRENT_BINARY_DIR}/timing-${variant}.cc")
    target_link_libraries(timing-${variant} PRIVATE timing-support)
    if(variant STREQUAL "bootstrap")
        target_compile_definitions(timing-${variant} PRIVATE PROBE_BOOTSTRAP=1)
    else()
        target_compile_definitions(timing-${variant} PRIVATE PROBE_BOOTSTRAP=0)
    endif()
endforeach()
enable_testing()
add_test(NAME upstream-starvation COMMAND timing-baseline)
add_test(NAME experimental-bootstrap COMMAND timing-bootstrap)

add_library(controller-support STATIC
 ${W}/api/video/color_space.cc
 ${W}/common_video/generic_frame_descriptor/generic_frame_info.cc
 ${W}/modules/video_coding/utility/decoded_frames_history.cc
 ${W}/modules/video_coding/include/video_codec_interface.cc
 ${W}/modules/video_coding/timing/frame_delay_variation_kalman_filter.cc
 ${W}/video/video_stream_buffer_controller.cc
 ${W}/video/frame_decode_timing.cc
 ${W}/video/task_queue_frame_decode_scheduler.cc
 ${W}/video/video_receive_stream_timeout_tracker.cc
 ${W}/api/video/frame_buffer.cc
 ${W}/api/video/encoded_frame.cc
 ${W}/api/video/encoded_image.cc
 ${W}/api/task_queue/task_queue_base.cc
 ${W}/api/task_queue/pending_task_safety_flag.cc
 ${W}/modules/video_coding/frame_helpers.cc
 ${W}/modules/video_coding/timing/jitter_estimator.cc
 ${W}/modules/video_coding/timing/inter_frame_delay_variation_calculator.cc
 ${W}/modules/video_coding/timing/rtt_filter.cc
 ${W}/rtc_base/task_utils/repeating_task.cc
)
target_compile_options(controller-support PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(controller-support PUBLIC timing-support)
foreach(variant baseline bootstrap)
 add_executable(controller-${variant} controller.cpp "${CMAKE_CURRENT_BINARY_DIR}/timing-${variant}.cc")
 target_link_libraries(controller-${variant} PRIVATE controller-support)
 if(variant STREQUAL "bootstrap")
  target_compile_definitions(controller-${variant} PRIVATE PROBE_BOOTSTRAP=1)
 else()
  target_compile_definitions(controller-${variant} PRIVATE PROBE_BOOTSTRAP=0)
 endif()
 add_test(NAME controller-${variant} COMMAND controller-${variant})
endforeach()

# Diagnostic comparison: feed all received frame timestamps to the unmodified
# estimator. This intentionally changes sample selection, not sender behavior.
file(READ "${W}/video/video_stream_buffer_controller.cc" controller_source)
string(REPLACE "!metadata.delayed_by_retransmission && metadata.receive_time" "metadata.receive_time"
    controller_all_arrivals "${controller_source}")
if(controller_all_arrivals STREQUAL controller_source)
 message(FATAL_ERROR "Expected retransmission filter was not found")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/controller-all-arrivals.cc" "${controller_all_arrivals}")
add_executable(controller-all-arrivals controller.cpp "${CMAKE_CURRENT_BINARY_DIR}/controller-all-arrivals.cc"
 "${CMAKE_CURRENT_BINARY_DIR}/timing-baseline.cc")
target_compile_definitions(controller-all-arrivals PRIVATE PROBE_BOOTSTRAP=0 PROBE_ALL_ARRIVALS=1)
target_link_libraries(controller-all-arrivals PRIVATE controller-support)
add_test(NAME controller-all-arrivals COMMAND controller-all-arrivals)
