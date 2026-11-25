#!/usr/bin/env bash
# 부하 테스트: 앞 서버 1대 + 뒤 서버 2대를 띄우고 wrk·ab로 요청을 몰아 보낸 뒤 처리량과 오류를 출력한다.
# 사용법 (web-server 폴더에서): bash tests/load.sh
# 필요: wrk, ab(apache2-utils). 일반 빌드로 잰다(ASan 빌드는 느려서 숫자가 의미 없다).
# 주의: WSL에서 /mnt/c(Windows 디스크) 위에서 돌리면 파일을 열 때마다 Windows를 거쳐 처리량이 50배쯤 낮게 나온다.
#       서버 성능을 재려면 폴더를 리눅스 쪽(예: /tmp)에 복사해서 돌린다.

PORT=${PORT:-18600}
HOST=127.0.0.1
DURATION=${DURATION:-10s}
B1=$((PORT + 1))
B2=$((PORT + 2))
URL="http://$HOST:$PORT"

make -s clean && make -s all || exit 1

ROUTES=$(mktemp)
cat > "$ROUTES" <<EOF
/api/ /cgi-bin/ 127.0.0.1:$B1 127.0.0.1:$B2
/img/ / 127.0.0.1:$B1 127.0.0.1:$B2
EOF
./server "$B1" www cgi-bin > /dev/null 2>&1 &
B1_PID=$!
./server "$B2" www cgi-bin > /dev/null 2>&1 &
B2_PID=$!
./server "$PORT" www cgi-bin "$ROUTES" > /tmp/web-load.log 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID $B1_PID $B2_PID 2>/dev/null; wait 2>/dev/null; rm -f "$ROUTES"' EXIT
sleep 0.5

# wrk 결과에서 초당 요청 수, 평균 응답 시간, 오류(연결·읽기·쓰기·시간 초과, 2xx·3xx 아닌 응답)만 뽑아 한 줄로 출력한다.
run_wrk() {
    local name=$1 conns=$2 path=$3
    local out
    out=$(wrk -t4 -c"$conns" -d"$DURATION" --timeout 2s "$URL$path" 2>&1)
    local rps lat errs non2xx
    rps=$(echo "$out" | awk '/Requests\/sec/ {print $2}')
    lat=$(echo "$out" | awk '/Latency/ {print $2}')
    errs=$(echo "$out" | awk -F'Socket errors: ' '/Socket errors/ {print $2}')
    non2xx=$(echo "$out" | awk '/Non-2xx/ {print $NF}')
    printf '%-34s 연결 %-4s 초당 %-10s 평균 %-9s 오류 [%s] 2xx아님 %s\n' \
        "$name" "$conns" "$rps" "$lat" "${errs:-없음}" "${non2xx:-0}"
}

echo "== wrk: keep-alive로 ${DURATION} 동안 계속 보내기 (작업 스레드 8개)"
run_wrk "정적 index.html (401B)"        8   /index.html
run_wrk "정적 photo.jpg (786B)"         8   /photo.jpg
run_wrk "CGI adder (요청마다 fork)"     8   "/cgi-bin/adder?a=3&b=4"
run_wrk "프록시 → CGI adder"            8   "/api/adder?a=3&b=4"
run_wrk "프록시 → 정적 photo.jpg"       8   /img/photo.jpg
run_wrk "정적 index.html, 연결 > 스레드"  100 /index.html

echo
echo "== ab: 요청마다 연결을 새로 열기, 2만 개, 동시 100"
ab -q -n 20000 -c 100 "$URL/index.html" 2>&1 |
    awk '/Failed requests|Non-2xx|Requests per second|Time per request.*\(mean\)$/'

echo
if kill -0 $SERVER_PID 2>/dev/null; then echo "부하 뒤에도 서버가 살아 있다"; else echo "서버가 죽었다"; fi
