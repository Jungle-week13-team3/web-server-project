FROM ubuntu:24.04

# 빌드·디버그·테스트 도구와 git
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential gdb valgrind \
      curl telnet apache2-utils \
      git ca-certificates \
    && rm -rf /var/lib/apt/lists/*