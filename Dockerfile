# Two processes, one container: the C dispatch engine on loopback:9090 and the
# Node bridge serving the UI on $PORT. Fly.io / Render / Railway all accept a
# plain Dockerfile, which is what this needs -- a C daemon plus a raw-socket
# bridge will not run on a static or serverless host.

# ---- build the engine ----------------------------------------------------
FROM node:22-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc make libc6-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
# NATIVE is deliberately left at 0: -march=native would target the builder's
# CPU and SIGILL on whatever the host actually runs.
RUN make clean && make server tests && ./tests

# ---- runtime -------------------------------------------------------------
FROM node:22-slim
WORKDIR /app
COPY --from=build /src/server ./server
COPY web ./web
COPY docker-start.sh ./
RUN chmod +x docker-start.sh
ENV PORT=8080
EXPOSE 8080
CMD ["./docker-start.sh"]
