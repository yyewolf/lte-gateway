# Build: docker build -t ltegtw-server .
FROM golang:1.25-trixie AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends libopus-dev pkg-config curl unzip \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
# libdave (Discord voice end-to-end encryption), prebuilt release from discord/libdave.
# It needs glibc 2.38+ and GLIBCXX 3.4.32+, hence Debian trixie for both stages.
RUN NON_INTERACTIVE=1 SHELL=/bin/sh sh "$(go env GOMODCACHE)/github.com/disgoorg/godave@v0.3.0/scripts/libdave_install.sh" v1.1.0
ENV PKG_CONFIG_PATH=/root/.local/lib/pkgconfig
COPY . .
RUN CGO_ENABLED=1 go build -trimpath -ldflags="-s -w" -o /out/ltegtw-server ./cmd/ltegtw-server

FROM debian:trixie-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libopus0 libstdc++6 ca-certificates \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /root/.local/lib/libdave.so /usr/local/lib/
RUN ldconfig
COPY --from=build /out/ltegtw-server /usr/local/bin/ltegtw-server
RUN useradd --system --uid 10001 ltegtw && mkdir -p /data/recordings && chown -R ltegtw /data
USER ltegtw
WORKDIR /data
EXPOSE 4000/udp
ENTRYPOINT ["ltegtw-server"]
CMD ["-config", "/config/config.yaml"]
