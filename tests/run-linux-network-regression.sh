#!/usr/bin/env bash
# Opt-in live regression. Requires an idle, isolated OBS profile with this build
# loaded and authenticated WebSocket enabled. Uses synthetic media only.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
: "${OBS_WEBSOCKET_URL:?Set the isolated OBS WebSocket URL}"
: "${OBS_WEBSOCKET_PASSWORD:?Set its WebSocket password}"
: "${VDONINJA_MEDIA_SEQUENCE_PATH:?Supply at least 120 seconds of synthetic 720p30 motion video}"
trials="${NETWORK_TRIALS:-3}"
[[ "$trials" =~ ^[1-9][0-9]*$ ]] || { echo 'NETWORK_TRIALS must be a positive integer' >&2; exit 1; }
output="${NETWORK_OUTPUT_DIR:-$repo/artifacts/network-regression}"
mkdir -p "$output"
container="vdoninja-reorder-$$-$RANDOM"
cleanup() { docker rm -f "$container" >/dev/null 2>&1 || true; }
trap cleanup EXIT
# NET_ADMIN applies only to this isolated container. Never impair the host NIC.
docker run -d --name "$container" --cap-add=NET_ADMIN ubuntu:24.04 sleep infinity >/dev/null
docker exec "$container" bash -c 'apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq coturn iproute2' > "$output/turn-setup.log" 2>&1
address="$(docker inspect "$container" --format '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')"
docker exec -d "$container" turnserver --no-cli --no-tls --no-dtls --realm=ninja-e2e.test \
    --user=e2e:syntheticNetwork42 --lt-cred-mech --fingerprint --allow-loopback-peers \
    --min-port=49160 --max-port=49200 --log-file=stdout
ice="$(node -e 'process.stdout.write(JSON.stringify([{urls:`turn:${process.argv[1]}:3478`,username:"e2e",credential:"syntheticNetwork42"}]))' "$address")"
for ((trial=0; trial<=trials; trial++)); do
    if ((trial == 0)); then
        condition=control
        loss_budget=0
        # The first run also gives the isolated relay time to finish starting.
    else
        condition="reorder-loss-$trial"
        loss_budget=100
        docker exec "$container" tc qdisc replace dev eth0 root netem delay 25ms 8ms distribution normal loss 0.5%
    fi
    echo "Testing $condition"
    VDONINJA_SOURCE_MODE=media-sequence VDONINJA_MEDIA_SEQUENCE_LOOP=0 \
    VDONINJA_VIDEO_WIDTH=1280 VDONINJA_VIDEO_HEIGHT=720 \
    VDONINJA_VIDEO_FPS_NUMERATOR=30 VDONINJA_VIDEO_FPS_DENOMINATOR=1 \
    VDONINJA_VIDEO_BITRATE_KBPS=3000 VDONINJA_SOAK_MS=60000 \
    VDONINJA_VIEWER_STABILIZE_MS=10000 VDONINJA_VIEW_BUFFER_MS=300 \
    VDONINJA_VIDEO_PROTECTION_MODE=0 VDONINJA_AUDIO_RED=0 \
    VDONINJA_BASE_URL=https://vdo.ninja/alpha/ VDONINJA_FORCE_TURN=1 \
    VDONINJA_ICE_SERVERS="turn:$address:3478|e2e|syntheticNetwork42" \
    VDONINJA_VIEWER_ICE_SERVERS_JSON="$ice" \
    VDONINJA_REQUIRE_STABLE_VIDEO=1 VDONINJA_REQUIRE_OBS_PERFORMANCE=1 \
    VDONINJA_MAX_LOST_VIDEO_PACKETS="$loss_budget" \
    VDONINJA_OUTPUT_DIR="$output/$condition" \
        node "$repo/scripts/obs-websocket-vdoninja-publish-check.cjs" "NetworkRegression${RANDOM}${trial}" false \
        > "$output/$condition.log" 2>&1
    # Packet loss is deliberately injected; only that counter has an allowance.
    # Frame drops, freezes, render/encoder skips and timing limits remain strict.
    docker exec "$container" tc -s qdisc show dev eth0 > "$output/$condition-qdisc.txt"
done
echo "PASS: control and $trials impaired-network runs; reports in $output"
