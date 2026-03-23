#!/usr/bin/env bash

set -euo pipefail

build_dir="${1:-/mnt/d/tkcp/ukcp/cpp/ukcp_server/build-wsl-cmake}"
listen="${2:-127.0.0.1:39620}"
clients="${3:-2000}"
rooms="${4:-200}"
room_size="${5:-10}"
seconds="${6:-60}"
client_workers="${7:-4}"
kcp_pps="${8:-3}"
udp_pps="${9:-5}"
warmup_ms="${10:-30000}"
drain_ms="${11:-8000}"
room_fps="${12:-15}"
prefix="${13:-perf_case}"

cd "$build_dir"

(
    /usr/bin/time -f 'elapsed_s=%e\nuser_s=%U\nsys_s=%S\ncpu_pct=%P\nmax_rss_kb=%M' \
        ./ukcp_perf_server \
        --mode room \
        --listen "$listen" \
        --seconds "$seconds" \
        --warmup-ms "$warmup_ms" \
        --drain-ms "$drain_ms" \
        --rooms "$rooms" \
        --room-size "$room_size" \
        --room-fps "$room_fps" \
        >"${prefix}_server.out" 2>"${prefix}_server.time"
) &
server_pid=$!

sleep 1

./ukcp_perf_client \
    --mode room \
    --server "$listen" \
    --clients "$clients" \
    --client-workers "$client_workers" \
    --seconds "$seconds" \
    --auth-warmup-ms "$warmup_ms" \
    --drain-ms "$drain_ms" \
    --room-size "$room_size" \
    --room-fps "$room_fps" \
    --kcp-pps "$kcp_pps" \
    --udp-pps "$udp_pps" \
    >"${prefix}_client.out"

wait "$server_pid"

echo '---SERVER---'
cat "${prefix}_server.out"
echo '---TIME---'
cat "${prefix}_server.time"
echo '---CLIENT---'
cat "${prefix}_client.out"
