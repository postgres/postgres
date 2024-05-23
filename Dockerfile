# Usa la imagen base de Ubuntu 22.04
FROM ubuntu:22.04


RUN apt-get update && apt-get upgrade -y && \
    apt-get install -y curl git build-essential

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y

ENV PATH=/root/.cargo/bin:$PATH

RUN git clone https://github.com/isoprophlex/distributed-postgres.git /app

WORKDIR /app

CMD ["bash"]

## TODO: Figure out how to properly run .sh's