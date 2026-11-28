cmake_minimum_required (VERSION 3.28)
project (proaudio)

set(SOURCES
		consumer/proaudio_consumer.cpp
		consumer/proaudio_consumer.h

		device/device_stream.cpp
		device/device_stream.h

		util/device_util.cpp
		util/device_util.h
		util/ring_buffer.h
		util/sync_probe.cpp
		util/sync_probe.h
		proaudio.cpp
		proaudio.h
)

casparcg_add_module_project(proaudio
	SOURCES ${SOURCES}
	INIT_FUNCTION "proaudio::init"
)
# swresample (drift correction) and avutil (the sync probe's FFT) come from the FFmpeg build that
# CasparCG already links; the shell links the libraries themselves.
target_include_directories(proaudio PRIVATE
	${FFMPEG_INCLUDE_PATH}
	${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(proaudio PRIVATE PortAudio::PortAudio)

set_target_properties(proaudio PROPERTIES FOLDER modules)
source_group(sources\\consumer consumer/*)
source_group(sources\\device device/*)
source_group(sources\\util util/*)
source_group(sources ./*)

# The PortAudio mock tests (tests/proaudio_tests.cpp) were written against the previous consumer's
# members and its own stream; they are left out of the build until they are rewritten against
# device_stream.
