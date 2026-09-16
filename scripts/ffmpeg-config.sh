# Sourced by the project build scripts; no system FFmpeg fallback.
FFMPEG_PREFIX="$ROOT/build/deps/ffmpeg"
FFMPEG_CMAKE_ARGS=(
    "-DFFMPEG_INCLUDE_DIR:PATH=$FFMPEG_PREFIX/include"
    "-DAVCODEC_LIBRARY:FILEPATH=$FFMPEG_PREFIX/lib/libavcodec.so"
    "-DAVUTIL_LIBRARY:FILEPATH=$FFMPEG_PREFIX/lib/libavutil.so"
)
