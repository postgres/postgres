FROM golang:alpine AS builder
RUN apk add --no-cache make gcc vim musl-dev
RUN apk update
RUN apk add iproute2
WORKDIR /app
ADD go.mod .
ADD go.sum .
RUN go mod download
COPY . .
RUN make build