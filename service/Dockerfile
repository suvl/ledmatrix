# syntax=docker/dockerfile:1
FROM --platform=$BUILDPLATFORM golang:1.27 AS build
ARG TARGETOS
ARG TARGETARCH
WORKDIR /src
COPY go.mod ./
COPY *.go ./
COPY data ./data
RUN CGO_ENABLED=0 GOOS=${TARGETOS} GOARCH=${TARGETARCH} go build -trimpath -ldflags="-s -w" -o /out/ledmatrix-api .

FROM gcr.io/distroless/static-debian13:nonroot
COPY --from=build /out/ledmatrix-api /ledmatrix-api
# The firmware images (read-only data). The build workflow fills service/firmware/ with manifest.json and
# ledmatrix-<version>.bin first; without them the folder holds only its README and the update routes answer 404.
COPY firmware /firmware
EXPOSE 8080
USER 65532:65532
ENTRYPOINT ["/ledmatrix-api"]
