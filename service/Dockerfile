# syntax=docker/dockerfile:1
FROM --platform=$BUILDPLATFORM golang:1.25 AS build
ARG TARGETOS
ARG TARGETARCH
WORKDIR /src
COPY go.mod ./
COPY *.go ./
COPY data ./data
RUN CGO_ENABLED=0 GOOS=${TARGETOS} GOARCH=${TARGETARCH} go build -trimpath -ldflags="-s -w" -o /out/ledmatrix-api .

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/ledmatrix-api /ledmatrix-api
EXPOSE 8080
USER nonroot:nonroot
ENTRYPOINT ["/ledmatrix-api"]
