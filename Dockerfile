# Build the C server
FROM gcc:13-bookworm AS build
WORKDIR /src
COPY server.c .
RUN gcc -O2 -o dinesync server.c

# Run it in a small image
FROM debian:bookworm-slim
WORKDIR /app
COPY --from=build /src/dinesync .
COPY public ./public
# Render provides the PORT variable; fall back to 8080 locally
CMD ./dinesync ${PORT:-8080}
