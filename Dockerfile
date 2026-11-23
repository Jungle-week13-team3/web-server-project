# 빌드·테스트용 POSIX 환경. Windows 에서는 이 컨테이너 안에서 빌드하고 실행한다.
#   docker build -t web-server-dev .
#   docker run --rm -it --name web-dev -v "${PWD}:/work" web-server-dev
FROM ubuntu:24.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["bash"]
