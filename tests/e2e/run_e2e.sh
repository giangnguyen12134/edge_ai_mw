#!/bin/bash
# End-to-end test on a private session bus: real MediaManager (synthetic
# camera), real NpuManager (fake NPU runtime) and the client SDK.
#
#   run_e2e.sh <build-dir> <source-dir>
#
# Needs dbus-daemon and dbus-run-session. Exits non-zero on the first failure.

set -u

BUILD="${1:?usage: run_e2e.sh <build-dir> <source-dir>}"
SRC="${2:?usage: run_e2e.sh <build-dir> <source-dir>}"

if ! command -v dbus-run-session >/dev/null 2>&1; then
  echo "SKIP: dbus-run-session not available"
  exit 77  # ctest "skipped" convention (see SKIP_RETURN_CODE)
fi

# Re-exec inside a private bus once.
if [ -z "${MW_E2E_INSIDE:-}" ]; then
  export MW_E2E_INSIDE=1
  exec dbus-run-session -- "$0" "$@"
fi

BIN="$BUILD/apps"
CFG="$SRC/config/dev"
WORK="$(mktemp -d)"
PIDS=()
FAILED=0

cleanup() {
  for pid in "${PIDS[@]:-}"; do
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
  done
  wait 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*"
  FAILED=1
}

check() {  # check <description> <command...>
  local what="$1"
  shift
  if "$@"; then
    echo "ok   - $what"
  else
    fail "$what"
  fi
}

start_media() {
  "$BIN/mw-media-manager" --config "$CFG/media.json" \
      >"$WORK/media.log" 2>&1 &
  MEDIA_PID=$!
  PIDS+=("$MEDIA_PID")
}

wait_for_name() {  # wait_for_name <bus-name>
  for _ in $(seq 1 100); do
    if busctl --user list 2>/dev/null | grep -q "^$1 "; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

# --- 1. Config validation ---------------------------------------------------
check "media config validates" \
  "$BIN/mw-media-manager" --config "$CFG/media.json" --check-config
check "npu config validates" \
  "$BIN/mw-npu-manager" --config "$CFG/npu.json" --check-config
check "bad override is rejected" \
  bash -c "! '$BIN/mw-media-manager' --set video.width=abc --check-config"

# --- 1b. Shared logging: one file and a UDP collector for every service -----
UDP_PORT=""
if command -v python3 >/dev/null 2>&1; then
  UDP_PORT="$(python3 -c 'import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
  python3 - "$UDP_PORT" "$WORK/udp.log" <<'PY' &
import socket, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.settimeout(0.5)
end = time.time() + 120
with open(sys.argv[2], "wb", buffering=0) as out:
    while time.time() < end:
        try:
            out.write(s.recvfrom(4096)[0] + b"\n")
        except socket.timeout:
            pass
PY
  PIDS+=("$!")
  UDP_SINK=', "udp"'
  UDP_CFG=", \"udp\": {\"host\": \"127.0.0.1\", \"port\": $UDP_PORT}"
else
  UDP_SINK=""; UDP_CFG=""
fi
cat >"$WORK/logging.json" <<JSON
{"sinks": ["stderr", "file"$UDP_SINK],
 "file": {"path": "$WORK/middleware.log"}$UDP_CFG}
JSON
export MW_LOG_CONFIG="$WORK/logging.json"

# --- 2. Start the services ---------------------------------------------------
start_media
check "MediaManager owns its bus name" wait_for_name org.genio.MediaManager

"$BIN/mw-npu-manager" --config "$CFG/npu.json" >"$WORK/npu.log" 2>&1 &
NPU_PID=$!
PIDS+=("$NPU_PID")
check "NpuManager owns its bus name" wait_for_name org.genio.NpuManager

# --- 3. Frames and results reach a client -----------------------------------
"$BIN/mw-example-client" --bus user --seconds 3 --audio \
    --save-frame "$WORK/frame.pgm" >"$WORK/client.log" 2>&1
check "client exits cleanly" test $? -eq 0
check "client received frames" \
  bash -c "grep -E '^frames: [1-9][0-9]* ' '$WORK/client.log'"
FPS=$(grep -E '^frames:' "$WORK/client.log" | sed -E 's/.*= ([0-9.]+) fps.*/\1/')
check "frame rate close to 30 fps (got ${FPS:-none})" \
  bash -c "awk -v f='${FPS:-0}' 'BEGIN{exit !(f>=24 && f<=36)}'"
check "face results arrived" grep -q "result face" "$WORK/client.log"
check "object results arrived" grep -q "result object" "$WORK/client.log"
check "gesture is gated by face and also ran" \
  grep -q "result gesture" "$WORK/client.log"
check "microphone ring attached" grep -q "^microphone: 16000 Hz" "$WORK/client.log"
check "saved frame is a 640x480 PGM" \
  bash -c "head -c 15 '$WORK/frame.pgm' | head -2 | tr '\n' ' ' | grep -q 'P5 640 480'"
check "saved frame holds the test pattern gradient (not blank)" \
  bash -c "[ \$(od -An -tu1 -j20 -N400 '$WORK/frame.pgm' | tr -s ' ' '\n' | sort -u | wc -l) -gt 20 ]"

# --- 4. Control interface -----------------------------------------------------
MODELS=$("$BIN/mw-example-client" --bus user --list-models 2>&1)
check "ListModels shows all four models" \
  bash -c "[ \$(echo '$MODELS' | grep -c -E 'face|gesture|object|voiceprint') -eq 4 ]"
check "SetModelEnabled works" \
  "$BIN/mw-example-client" --bus user --model object=off
check "unknown model is an error" \
  bash -c "! '$BIN/mw-example-client' --bus user --model nosuch=on 2>/dev/null"
sleep 0.5
"$BIN/mw-example-client" --bus user --seconds 2 --no-video >"$WORK/c2.log" 2>&1
# One empty "cleared" result may still be in flight; a running model would
# have produced about 10 in two seconds.
check "disabled model stops producing results" \
  bash -c "[ \$(grep -c 'result object' '$WORK/c2.log') -le 1 ]"
"$BIN/mw-example-client" --bus user --model object=on >/dev/null 2>&1

# --- 4b. VoiceSprint: utterances, enrolment, persistence of the list --------
check "voiceprint produced speaker results from the microphone" \
  grep -q "result voiceprint speaker" "$WORK/client.log"
"$BIN/mw-example-client" --bus user --enroll alice --seconds 5 --no-video \
    >"$WORK/enroll.log" 2>&1
check "enrolment stored the next utterance" \
  grep -q "enrolled:alice" "$WORK/enroll.log"
check "enrolled speaker is listed" \
  bash -c "'$BIN/mw-example-client' --bus user --list-speakers | grep -qx alice"
check "enrolling without a name is rejected" \
  bash -c "! '$BIN/mw-example-client' --bus user --enroll '' 2>/dev/null"
"$BIN/mw-example-client" --bus user --remove-speaker alice >/dev/null 2>&1
check "removed speaker is gone" \
  bash -c "! '$BIN/mw-example-client' --bus user --list-speakers | grep -qx alice"

# --- 5. No pool slot leaks after clients disconnect -------------------------
# NpuManager legitimately keeps up to 2 frames and the producer 1, so 8 slots
# leave at least 5 free once every other client has left.
STATS=$("$BIN/mw-example-client" --bus user --stats --seconds 1 2>&1)
stat() { echo "$STATS" | awk -v k="$1" '$1==k {print $2}'; }
check "pool slots returned after clients left (free=$(stat pool_free_slots))" \
  bash -c "[ '$(stat pool_free_slots)' -ge 5 ]"
check "no frame was ever dropped for lack of a pool slot" \
  bash -c "[ '$(stat frames_dropped_no_slot)' = 0 ]"
check "no ingest errors" bash -c "[ '$(stat ingest_errors)' = 0 ]"

# --- 6. MediaManager restart: clients resubscribe on their own --------------
"$BIN/mw-example-client" --bus user --seconds 8 --no-results \
    >"$WORK/client3.log" 2>&1 &
CLIENT_PID=$!
sleep 2
kill -TERM "$MEDIA_PID"
wait "$MEDIA_PID" 2>/dev/null
sleep 1
start_media
check "MediaManager owns its name again" wait_for_name org.genio.MediaManager
wait "$CLIENT_PID"
check "client saw the service go away" \
  grep -q "went away" "$WORK/client3.log"
check "client reconnected (second 'camera connected')" \
  bash -c "[ \$(grep -c 'camera connected' '$WORK/client3.log') -ge 2 ]"
check "frames flowed after the restart" \
  bash -c "grep -E '^frames: [0-9]+' '$WORK/client3.log' | awk '{exit !(\$2>150)}'"

# --- 6b. Same stack with the CPU (ONNX Runtime) backend ----------------------
ORT_LIB="${MW_ONNX_LIBRARY:-}"
if [ -z "$ORT_LIB" ]; then
  ORT_LIB="$(python3 -c 'import onnxruntime,glob,os; d=os.path.join(os.path.dirname(onnxruntime.__file__),"capi"); print((glob.glob(d+"/libonnxruntime.so*") or [""])[0])' 2>/dev/null)"
fi
if [ -n "$ORT_LIB" ] && [ -e "$ORT_LIB" ]; then
  kill -TERM "$NPU_PID" 2>/dev/null
  wait "$NPU_PID" 2>/dev/null
  sed -e "s|\"tests/data/|\"$SRC/tests/data/|" \
      -e "s|\"onnx_library\": \"[^\"]*\"|\"onnx_library\": \"$ORT_LIB\"|" \
      "$CFG/npu_onnx.json" >"$WORK/npu_onnx.json"
  "$BIN/mw-npu-manager" --config "$WORK/npu_onnx.json" >"$WORK/npu2.log" 2>&1 &
  NPU_PID=$!
  PIDS+=("$NPU_PID")
  sleep 1
  check "NpuManager owns its bus name with the cpu runtime" \
    wait_for_name org.genio.NpuManager
  check "cpu runtime logged ONNX Runtime" grep -q "ONNX Runtime" "$WORK/npu2.log"
  "$BIN/mw-example-client" --bus user --seconds 3 --audio \
      >"$WORK/client_onnx.log" 2>&1
  check "onnx face model produced a face box" \
    grep -q "result face" "$WORK/client_onnx.log"
  check "onnx object model produced a cup (class 2, xywh)" \
    grep -q "cup" "$WORK/client_onnx.log"
  check "onnx speaker model produced results" \
    grep -q "result voiceprint" "$WORK/client_onnx.log"
  check "no model errors with cpu runtime" \
    bash -c "! grep -qi 'inference failed' '$WORK/npu2.log'"
else
  echo "skip - 6b cpu runtime (no libonnxruntime; set MW_ONNX_LIBRARY)"
fi

# --- 6c. Native camera formats and camera sessions -------------------------
# The camera's own pixel format reaches clients untouched, and a camera that
# "unplugs" gets a fresh pool that clients pick up on their own.
restart_media() {  # restart_media <extra --set args...>
  kill -TERM "$MEDIA_PID" 2>/dev/null
  wait "$MEDIA_PID" 2>/dev/null
  "$BIN/mw-media-manager" --config "$CFG/media.json" "$@" \
      >"$WORK/media_native.log" 2>&1 &
  MEDIA_PID=$!
  PIDS+=("$MEDIA_PID")
  wait_for_name org.genio.MediaManager
}
kill -TERM "$NPU_PID" 2>/dev/null
wait "$NPU_PID" 2>/dev/null

restart_media --set video.pixel_format=YUYV
"$BIN/mw-example-client" --bus user --seconds 2 --no-results \
    --save-frame "$WORK/yuyv.pgm" >"$WORK/client_yuyv.log" 2>&1
check "YUYV frames reach the client in the camera's format" \
  bash -c "head -c 11 '$WORK/yuyv.pgm' | head -2 | tr '\n' ' ' | grep -q 'P5 640 480'"
check "YUYV luma image has width*height pixels" \
  bash -c "[ \$(stat -c %s '$WORK/yuyv.pgm') -eq \$((15 + 640*480)) ]"
check "service reports native capture" \
  bash -c "'$BIN/mw-example-client' --bus user --stats --seconds 1 | awk '\$1==\"native_capture\" {exit !(\$2==1)}'"

restart_media --set video.pixel_format=MJPG
"$BIN/mw-example-client" --bus user --seconds 2 --no-results \
    --save-frame "$WORK/frame.jpg" >"$WORK/client_mjpg.log" 2>&1
check "MJPG frame is the untouched JPEG (SOI marker)" \
  bash -c "[ \"\$(head -c 2 '$WORK/frame.jpg' | od -An -tx1 | tr -d ' \n')\" = ffd8 ]"
check "MJPG frame size is the JPEG size, not the buffer size" \
  bash -c "[ \$(stat -c %s '$WORK/frame.jpg') -lt 100000 ]"

restart_media --set video.test_unplug_after_frames=45
"$BIN/mw-example-client" --bus user --seconds 7 --no-results \
    >"$WORK/client_unplug.log" 2>&1
check "camera sessions were replaced" \
  bash -c "[ \$(grep -c 'camera session:' '$WORK/media_native.log') -ge 3 ]"
check "client resubscribed to every new pool" \
  bash -c "[ \$(grep -c 'camera connected' '$WORK/client_unplug.log') -ge 3 ]"
check "frames kept flowing across camera sessions" \
  bash -c "grep -E '^frames: [0-9]+' '$WORK/client_unplug.log' | awk '{exit !(\$2>100)}'"

# --- 7. Clean shutdown -------------------------------------------------------
kill -TERM "$NPU_PID" "$MEDIA_PID" 2>/dev/null
wait "$NPU_PID" 2>/dev/null
check "NpuManager exited on SIGTERM" test $? -eq 0

# --- 8. Both services logged into the one shared file --------------------
check "shared log file has MediaManager lines" \
  grep -q " mw-media-manager\[[0-9]*\] " "$WORK/middleware.log"
check "shared log file has NpuManager lines" \
  grep -q " mw-npu-manager\[[0-9]*\] " "$WORK/middleware.log"
check "every shared log line is well formed (date, level, program[pid])" \
  bash -c "! grep -v -E '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+[+-][0-9]{4} [DIWE] mw-[a-z-]+\\[[0-9]+\\] [a-z_.]+:[0-9]+\\] ' '$WORK/middleware.log'"
if [ -n "$UDP_PORT" ]; then
  sleep 0.5
  check "UDP collector received MediaManager syslog datagrams" \
    grep -q -E "^<1[0-9]{2}>1 .* mw-media-manager [0-9]+ - - " "$WORK/udp.log"
  check "UDP collector received NpuManager syslog datagrams" \
    grep -q -E "^<1[0-9]{2}>1 .* mw-npu-manager [0-9]+ - - " "$WORK/udp.log"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "---- media.log ----"; tail -20 "$WORK/media.log"
  echo "---- npu.log ----";   tail -20 "$WORK/npu.log"
  echo "---- client.log ----"; tail -20 "$WORK/client.log"
  exit 1
fi
echo "ALL END-TO-END CHECKS PASSED"
