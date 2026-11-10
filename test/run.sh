#!/bin/sh
# The PC tests: builds them with a native gcc (e.g. PATH=/c/msys64/ucrt64/bin:$PATH)
# and runs them. The crypto tests need node (its OpenSSL is the reference);
# the MLS tests use the MLS working group's vectors in vectors/mls.
set -e
cd "$(dirname "$0")"
mkdir -p out
S=../sysmodule/source
CXX="g++ -O1 -g -Wall -std=c++17 -I../common -I$S"
CRYPTO="$S/sha256.cpp $S/aes.cpp $S/chacha.cpp $S/p256.cpp $S/hpke.cpp"
$CXX -o out/unit_test.exe unit_test.cpp $S/util.cpp $S/json.cpp $S/ws.cpp $S/store.cpp $S/gateway.cpp $S/mixer.cpp $S/mic.cpp
./out/unit_test.exe
node crypto_vectors.js
$CXX -o out/crypto_test.exe crypto_test.cpp $S/daveframe.cpp $S/rtp.cpp $S/mlscrypto.cpp $S/mlskeys.cpp $S/mlswire.cpp $CRYPTO
./out/crypto_test.exe
node crypto_vectors.js verify out/chirp_sigs.txt
node mls_vectors.js
$CXX -o out/mls_test.exe mls_test.cpp $S/mlswire.cpp $S/mlscrypto.cpp $S/mlskeys.cpp $S/ratchet.cpp $S/mlsgroup.cpp $CRYPTO
./out/mls_test.exe
$CXX -o out/dave_test.exe dave_test.cpp $S/dave.cpp $S/daveframe.cpp $S/mlsgroup.cpp $S/ratchet.cpp $S/mlswire.cpp $S/mlscrypto.cpp $S/mlskeys.cpp $CRYPTO
./out/dave_test.exe | grep -v "^    \["
# The Opus encoder through libopus's decoder (in opus-ref/: the 1.5.2 source, built with
# make -f Makefile.unix libopus.a, and glue/mdct_ref.c compiled beside it)
O=opus-ref/opus-1.5.2
if [ -f $O/libopus.a ] && [ -f opus-ref/glue/mdct_ref.o ]; then
    $CXX -O2 -I$O/include -o out/opus_test.exe opus_test.cpp $S/opusenc.cpp $S/fft.cpp opus-ref/glue/mdct_ref.o $O/libopus.a
    ./out/opus_test.exe | grep -E "MDCT|speed|passed|FAIL"
fi
# The noise suppression against xiph's RNNoise v0.1 (in rnnoise-ref/: its source, built into
# build/librnnoise.a: each of src/{denoise,rnn,rnn_data,pitch,celt_lpc,kiss_fft}.c with gcc -O2).
# (extras/denoise-v0.2 has the larger models' code, and how its references were built)
if [ -f rnnoise-ref/build/librnnoise.a ]; then
    $CXX -O2 -Wno-psabi -o out/denoise_test.exe denoise_test.cpp $S/denoise.cpp $S/fft.cpp rnnoise-ref/build/librnnoise.a
    ./out/denoise_test.exe | grep -E "matches|quieter|speed|passed|FAIL"
fi
# Interop with ts-mls, an independent MLS (in interop/: npm install there, then node patch_ts_mls.mjs)
if [ -d interop/node_modules/ts-mls ]; then
    $CXX -o interop/out/chirp_cli.exe interop/chirp_cli.cpp $S/mlswire.cpp $S/mlscrypto.cpp $S/mlskeys.cpp $S/ratchet.cpp $S/mlsgroup.cpp $CRYPTO
    (cd interop && node patch_ts_mls.mjs > /dev/null && node interop.mjs | tail -1)
fi
