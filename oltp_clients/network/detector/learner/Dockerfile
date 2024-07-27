FROM alpine
RUN apk add --update make cmake gcc g++ gfortran
RUN apk add --update python3 py-pip python3-dev
RUN pip install numpy
RUN pip install --upgrade google-api-python-client
RUN pip install grpcio
RUN pip install grpcio-tools
WORKDIR /rl
COPY . .
