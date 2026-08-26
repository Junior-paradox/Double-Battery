#!/bin/sh
# Start the engine, wait for it to accept connections, then hand the terminal
# to the bridge so the container's lifetime tracks the web process.
set -e

./server 9090 &
ENGINE_PID=$!
trap 'kill $ENGINE_PID 2>/dev/null || true' TERM INT

i=0
until node -e "require('net').connect(9090,'127.0.0.1')
        .on('connect',function(){process.exit(0)})
        .on('error',function(){process.exit(1)})" 2>/dev/null; do
  i=$((i+1))
  if [ "$i" -gt 60 ]; then echo "engine did not come up" >&2; exit 1; fi
  if ! kill -0 "$ENGINE_PID" 2>/dev/null; then echo "engine died on startup" >&2; exit 1; fi
  sleep 0.5
done
echo "engine ready, starting bridge on port ${PORT:-8080}"

exec node web/bridge.js
