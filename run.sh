#!/bin/bash
set -e

FFMPEG_BUILD="$(pwd)/build/ffmpeg"
FFMPEG_SOURCE="$(pwd)/deps/FFmpeg"
OPUS_BUILD="$(pwd)/build/opus"
OPUS_SOURCE="$(pwd)/deps/opus"

function ask_remove_folder {
  read -p "Do you want to remove $1? (y/n) " answer &&
    [[ $answer =~ ^[Yy]$ ]] && rm -r $1 && echo "$1 removed." || echo "$1 removal cancelled."
}

function replace {
  if ! grep -q "$2" "$3"; then sed -i '' "s/$1/$2 $1/g" "$3"; else echo "[replace] skip $1"; fi
}

ask_remove_folder $OPUS_BUILD
[ -d $OPUS_BUILD ] || (
  rm -rf $OPUS_BUILD || true
  mkdir -p $OPUS_BUILD
  cd $OPUS_BUILD
  CONF_FLAGS=(
    --prefix=$OPUS_BUILD     # install library in a build directory for FFmpeg to include
    --host=i686-gnu          # use i686 linux
    --enable-shared=no       # not to build shared library
    --disable-asm            # not to use asm
    --disable-rtcd           # not to detect cpu capabilities
    --disable-doc            # not to build docs
    --disable-extra-programs # not to build demo and tests
    --disable-stack-protector
  )
  echo "CONF_FLAGS=${CONF_FLAGS[@]}"
  (cd $OPUS_SOURCE &&
    emconfigure ./autogen.sh &&
    CFLAGS=$CFLAGS emconfigure ./configure "${CONF_FLAGS[@]}")
  emmake make -C $OPUS_SOURCE clean
  emmake make -C $OPUS_SOURCE install -j
)

ask_remove_folder $FFMPEG_BUILD
[ -d $FFMPEG_BUILD ] || (
  mkdir -p $FFMPEG_BUILD
  cd $FFMPEG_SOURCE
  export CFLAGS="-I/opt/homebrew/include -I$OPUS_BUILD/include"
  export LDFLAGS="-L$OPUS_BUILD/lib -s INITIAL_MEMORY=33554432" # -L${PREFIX}/lib
  export EM_PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$OPUS_BUILD/lib/pkgconfig"
  replace "if (read_random" "return get_generic_seed(); \/\* _NOSYS_ \*\/" "$FFMPEG_SOURCE/libavutil/random_seed.c"
  emconfigure ./configure \
    --prefix=$FFMPEG_BUILD \
    --enable-cross-compile \
    --target-os=none \
    --arch=x86_32 \
    --disable-all \
    --disable-runtime-cpudetect \
    --disable-asm \
    --disable-fast-unaligned \
    --disable-pthreads \
    --disable-w32threads \
    --disable-os2threads \
    --disable-debug \
    --disable-stripping \
    --disable-x86asm \
    --disable-inline-asm \
    --disable-programs \
    --disable-doc \
    --disable-autodetect \
    --disable-network \
    --disable-d3d11va \
    --disable-dxva2 \
    --disable-vaapi \
    --disable-vdpau \
    --disable-protocol=file \
    --disable-bzlib \
    --disable-iconv \
    --disable-libxcb \
    --disable-lzma \
    --disable-sdl2 \
    --disable-securetransport \
    --disable-xlib \
    --disable-zlib \
    \
    --extra-cflags="$CFLAGS" \
    --extra-cxxflags="$CFLAGS" \
    --extra-ldflags="$LDFLAGS" \
    --pkg-config-flags="--static" \
    --nm="llvm-nm" \
    --ar=emar \
    --ranlib=emranlib \
    --cc=emcc \
    --cxx=em++ \
    --objcc=emcc \
    --dep-cc=emcc \
    \
    --enable-avcodec \
    --enable-swresample \
    --enable-avformat \
    \
    --enable-libopus \
    --enable-encoder=libopus,aac,mp2 \
    --enable-muxer=webm,ogg,adts,mp2
  # \
  #
  emmake make -j8
  emmake make install
)

em++ main.cpp $FFMPEG_BUILD/lib/*.a $OPUS_BUILD/lib/*.a \
  -I $FFMPEG_BUILD/include \
  -o ./dist/main.js \
  -s STRICT=1 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s ASSERTIONS=1 \
  -s MODULARIZE=1 \
  -s FILESYSTEM=0 \
  -s EXPORT_ES6=1 \
  -s IMPORTED_MEMORY \
  -s MALLOC=emmalloc \
  -s USE_ES6_IMPORT_META=1 \
  -s EXPORTED_FUNCTIONS="['_malloc', '_free', '_modify_array']" \
  -s EXPORTED_RUNTIME_METHODS=stringToUTF8 \
  -g4 -s ASSERTIONS=2 -s SAFE_HEAP=1 -s STACK_OVERFLOW_CHECK=1 -s DEMANGLE_SUPPORT=1 \
  -s NO_DISABLE_EXCEPTION_CATCHING

cat <<EOF | cat - dist/main.js >temp && mv temp dist/main.js
class XMLHttpRequest{open(t,e){"GET"!=t&&console.error("Method not implemented:",t),this._url=e}get status(){return this._status}get statusText(){return this._statusText}set onprogress(t){this._onprogress=t}set onerror(t){this._onerror=t}set onload(t){this._onload=t}set responseType(t){"arraybuffer"!=t&&console.error("Response type not implemented",t)}send(){fetch(this._url).then(t=>(this._status=t.status,this._statusText=t.statusText,t.arrayBuffer())).then(t=>{this._arrayBuffer=t,this._onload&&this._onload({})}).catch(t=>{this._onerror&&(console.error("Could not fetch:",t),this._onerror({}))})}get responseText(){console.error("Not implemented: getting responseText")}get response(){return this._arrayBuffer}}
EOF

echo "Build complete"
deno run -A main.ts
