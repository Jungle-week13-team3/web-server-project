#!/usr/bin/env bash
# 기능 테스트: 서버를 실제로 띄우고 curl·nc로 요청을 보내 상태 코드를 확인한다.
# 사용법 (web-server 폴더에서): bash tests/integration.sh
# Ubuntu의 nc(netcat-openbsd) 기준이다. -q1 = 보낼 것을 다 보낸 뒤 1초 기다렸다 닫기.

PORT=${PORT:-18080}
HOST=127.0.0.1
pass=0
fail=0

make -s all || exit 1

# 테스트용 정적 루트: www/를 복사하고, 권한 없는 파일과 폴더를 더한다.
# /mnt/c 같은 Windows 드라이브에서는 chmod가 안 먹으니 /tmp에 만든다.
ROOT=$(mktemp -d)
cp -r www/. "$ROOT/"
echo secret > "$ROOT/noperm.txt"
chmod 000 "$ROOT/noperm.txt"
mkdir "$ROOT/sub"

# 테스트용 CGI 폴더: adder와 일부러 잘못 만든 프로그램들
CGI=$(mktemp -d)
cp cgi-bin/adder "$CGI/"
printf '#!/bin/sh\necho "Content-Type: text/plain"\necho\necho oops\nexit 1\n' > "$CGI/fail.sh"
printf '#!/bin/sh\necho "no blank line"\n' > "$CGI/badout.sh"
printf '#!/bin/sh\necho hi\n' > "$CGI/noexec.sh"
printf '#!/bin/sh\nsleep 1\necho "Content-Type: text/plain"\necho\necho slept\n' > "$CGI/sleep.sh"
# CGI 프로세스가 물려받은 소켓 수와 파이프 수. 소켓 0개, 파이프는 표준입력·출력 2개여야 한다.
# (전체 개수는 세지 않는다. 서버를 띄운 터미널이 넘겨준 fd처럼 서버와 무관한 것도 섞여 있다)
printf '#!/bin/sh\necho "Content-Type: text/plain"\necho\nfds=$(ls -l /proc/self/fd)\necho "socket=$(echo "$fds" | grep -c socket:) pipe=$(echo "$fds" | grep -c pipe:)"\n' > "$CGI/fds.sh"
# 끝나지 않는 CGI. 셸이 띄운 sleep(손자 프로세스)까지 같이 끝나는지 보려고 흔치 않은 숫자 31을 쓴다
printf '#!/bin/sh\nsleep 31\necho "Content-Type: text/plain"\necho\necho never\n' > "$CGI/hang.sh"
chmod 755 "$CGI/fail.sh" "$CGI/badout.sh" "$CGI/sleep.sh" "$CGI/fds.sh" "$CGI/hang.sh"
chmod 644 "$CGI/noexec.sh"

# 프록시 테스트: 뒤 서버 두 대 + 규칙 파일. 18099는 아무도 안 듣는 포트(죽은 서버 흉내)
B1=$((PORT + 11))
B2=$((PORT + 12))
DEAD=$((PORT + 19))
HANG=$((PORT + 18)) # 접속은 받지만 영원히 답하지 않는 뒤 서버
ROUTES="$CGI/routes.conf"
cat > "$ROUTES" <<EOF
# 테스트용 프록시 규칙
/api/ /cgi-bin/ 127.0.0.1:$B1 127.0.0.1:$B2
/img/ / 127.0.0.1:$B1 127.0.0.1:$B2
/dead/ / 127.0.0.1:$DEAD
/failover/ /cgi-bin/ 127.0.0.1:$DEAD 127.0.0.1:$B1
/hang/ / 127.0.0.1:$HANG
EOF
python3 -c "
import socket, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', $HANG)); s.listen()
conns = []
while True:
    c, _ = s.accept(); conns.append(c)   # 받기만 하고 아무 답도 안 한다
" &
HANG_PID=$!
head -c 3000000 /dev/urandom > "$ROOT/big.bin"   # 3MB 바이너리: 1MB 본문 제한과 상관없이 중계되는지

./server "$B1" "$ROOT" "$CGI" > /tmp/web-backend1.log 2>&1 &
B1_PID=$!
./server "$B2" "$ROOT" "$CGI" > /tmp/web-backend2.log 2>&1 &
B2_PID=$!
./server "$PORT" "$ROOT" "$CGI" "$ROUTES" > /tmp/web-server-test.log 2>&1 &
SERVER_PID=$!
# 끝낼 때 SIGTERM으로 끄고 다 끝날 때까지 기다린다. 서버가 정상 종료해야 ASan 빌드의 누수 보고가 로그에 남는다
trap 'kill $SERVER_PID $B1_PID $B2_PID $HANG_PID 2>/dev/null; wait 2>/dev/null; chmod 644 "$ROOT/noperm.txt"; rm -rf "$ROOT" "$CGI"' EXIT
sleep 0.5

# raw 요청 문자열을 nc로 보내고, 응답 첫 줄의 상태 코드를 출력한다. 응답이 없으면 빈 문자열.
send_raw() {
    printf '%b' "$1" | timeout 3 nc -q1 "$HOST" "$PORT" | head -n 1 | awk '{print $2}'
}

# 이름, 기대한 값, 실제 값을 받아 PASS/FAIL을 출력한다.
check() {
    if [ "$2" = "$3" ]; then
        echo "PASS  $1"
        pass=$((pass + 1))
    else
        echo "FAIL  $1 (기대: '$2', 실제: '$3')"
        fail=$((fail + 1))
    fi
}

# URI의 응답 본문이 루트의 파일과 바이트까지 같으면 "same"을 출력한다.
same_as_file() {
    curl -s -o "$ROOT/.got" "http://$HOST:$PORT$1"
    cmp -s "$ROOT/.got" "$ROOT/$2" && echo same
}

# URI의 응답에서 Content-Type 값을 출력한다.
content_type_of() {
    curl -s -o /dev/null -w '%{content_type}' "http://$HOST:$PORT$1"
}

# URI의 응답에서 Content-Length 값을 출력한다.
content_length_of() {
    curl -s -D - -o /dev/null "http://$HOST:$PORT$1" | tr -d '\r' | awk -F': ' 'tolower($1)=="content-length"{print $2}'
}

# 정상 GET이 200을 받는지로 서버가 살아 있는지 확인한다.
check_alive() {
    check "  └ 서버가 계속 돈다" "200" "$(curl -s -o /dev/null -w '%{http_code}' "http://$HOST:$PORT/")"
}

check "정상 GET → 200" "200" "$(curl -s -o /dev/null -w '%{http_code}' "http://$HOST:$PORT/")"
check "GET / 본문 = index.html" "same" "$(same_as_file / index.html)"
check "정적 파일에 POST → 405" "405" "$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://$HOST:$PORT/")"
check "HTTP/1.1 기본 → Connection: keep-alive" "1" \
    "$(curl -s -D - -o /dev/null "http://$HOST:$PORT/" | grep -ci '^connection: keep-alive')"
check "Connection: close 요청 → close" "1" \
    "$(curl -s -D - -o /dev/null -H 'Connection: close' "http://$HOST:$PORT/" | grep -ci '^connection: close')"
check "HTTP/1.0 → close" "1" \
    "$(curl -s -0 -D - -o /dev/null "http://$HOST:$PORT/" | grep -ci '^connection: close')"

check "없는 메서드 DELETE → 501" "501" "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "http://$HOST:$PORT/")"
check_alive
check "HTTP/1.0, Host 없음 → 200" "200" "$(send_raw 'GET / HTTP/1.0\r\n\r\n')"
check "요청 줄 칸 부족 → 400" "400" "$(send_raw 'GET /\r\n\r\n')"
check_alive
check "틀린 버전 HTTP/9.9 → 505" "505" "$(send_raw 'GET / HTTP/9.9\r\nHost: x\r\n\r\n')"
check_alive
check "HTTP/1.1인데 Host 없음 → 400" "400" "$(send_raw 'GET / HTTP/1.1\r\n\r\n')"
check_alive
check "헤더에 ':' 없음 → 400" "400" "$(send_raw 'GET / HTTP/1.1\r\nHost x\r\n\r\n')"
check_alive
check "헤더 도중 끊김 → 응답 없음" "" "$(send_raw 'GET / HTTP/1.1\r\nHo')"
check_alive
check "아무것도 안 보내고 끊김 → 응답 없음" "" "$(send_raw '')"
check_alive

LONG_URI="/$(head -c 9000 < /dev/zero | tr '\0' 'a')"
check "8KB 넘는 요청 줄 → 414" "414" "$(send_raw "GET $LONG_URI HTTP/1.1\r\nHost: x\r\n\r\n")"
check_alive

# --- 5단계: 정적 파일 ---
check "MIME .html" "text/html" "$(content_type_of /index.html)"
check "MIME .css" "text/css" "$(content_type_of /style.css)"
check "MIME .js" "application/javascript" "$(content_type_of /app.js)"
check "MIME .png" "image/png" "$(content_type_of /image.png)"
check "MIME .jpg" "image/jpeg" "$(content_type_of /photo.jpg)"
check "PNG 바이트가 그대로" "same" "$(same_as_file /image.png image.png)"
check "JPG 바이트가 그대로" "same" "$(same_as_file /photo.jpg photo.jpg)"
check "Content-Length = 파일 크기" "$(stat -c %s "$ROOT/photo.jpg")" "$(content_length_of /photo.jpg)"
check "쿼리 붙은 URI → 200" "200" "$(curl -s -o /dev/null -w '%{http_code}' "http://$HOST:$PORT/style.css?v=1")"
check "없는 파일 → 404" "404" "$(curl -s -o /dev/null -w '%{http_code}' "http://$HOST:$PORT/nope.html")"
check "읽기 권한 없는 파일 → 403" "403" "$(curl -s -o /dev/null -w '%{http_code}' "http://$HOST:$PORT/noperm.txt")"
check "폴더 (index.html 없음) → 404" "404" "$(curl -s -o /dev/null -w '%{http_code}' "http://$HOST:$PORT/sub/")"
check "폴더를 파일처럼 → 403" "403" "$(curl -s -o /dev/null -w '%{http_code}' "http://$HOST:$PORT/sub")"
# curl은 기본으로 ../를 정리해 버려서 --path-as-is로 그대로 보낸다
check "루트 밖 /../ → 403" "403" \
    "$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "http://$HOST:$PORT/../etc/passwd")"
check "루트 밖 (nc로 직접)" "403" "$(send_raw 'GET /sub/../../etc/passwd HTTP/1.0\r\n\r\n')"
check_alive

# --- 6단계: 본문과 keep-alive ---
URL="http://$HOST:$PORT"
# curl은 한 번 실행에서 URL 여러 개를 같은 연결로 보낸다. num_connects가 0이면 연결을 다시 썼다는 뜻
check "keep-alive: 두 요청이 한 연결" "1 0 " \
    "$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' "$URL/" "$URL/style.css")"
check "POST 본문 뒤 같은 연결로 GET" "200/1 200/0" \
    "$(curl -s -o /dev/null -w '%{http_code}/%{num_connects} ' -d 'a=3&b=4' "$URL/cgi-bin/adder" \
        --next -s -o /dev/null -w '%{http_code}/%{num_connects}' "$URL/")"
check "  └ 서버가 본문 7바이트를 읽음" "1" \
    "$(grep -c 'POST /cgi-bin/adder HTTP/1.1 (body 7) -> 200' /tmp/web-server-test.log)"
# 100KB 본문: 버퍼(8KB)보다 커서 소켓에서 여러 번 나눠 읽는다. Expect 헤더를 빼야 curl이 1초 기다리지 않는다
{ printf 'a=1&b=2&pad='; head -c 100000 /dev/zero | tr '\0' 'x'; } > "$ROOT/.big"
check "100KB 본문 → adder 결과 3" "3" \
    "$(curl -s -H 'Expect:' --data-binary @"$ROOT/.big" "$URL/cgi-bin/adder")"
check "100KB 본문 뒤 같은 연결로 GET" "200/1 200/0" \
    "$(curl -s -H 'Expect:' -o /dev/null -w '%{http_code}/%{num_connects} ' --data-binary @"$ROOT/.big" "$URL/cgi-bin/adder" \
        --next -s -o /dev/null -w '%{http_code}/%{num_connects}' "$URL/")"
check "본문이 나뉘어 도착 → adder 결과 23" "23" \
    "$( (printf 'POST /cgi-bin/adder HTTP/1.1\r\nHost: x\r\nContent-Length: 8\r\nConnection: close\r\n\r\na=1&b='; sleep 0.5; printf '22') \
        | timeout 3 nc -q1 "$HOST" "$PORT" | tail -n 1)"
check "본문 도중 끊김 → 응답 없음" "" \
    "$(send_raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nhello')"
check_alive
check "요청 두 개를 한 번에 (파이프라이닝)" "2" \
    "$(printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' \
        | timeout 3 nc -q1 "$HOST" "$PORT" | grep -c '^HTTP/1.1 200')"
check "Content-Length가 숫자 아님 → 400" "400" \
    "$(send_raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n')"
check "본문 1MB 초과 → 413" "413" \
    "$(send_raw 'POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n')"
check "chunked 본문 → 501" "501" \
    "$(send_raw 'POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n')"
URI_5K="/$(head -c 5000 < /dev/zero | tr '\0' 'a')"
check "URI 4KB 초과 → 414" "414" "$(send_raw "GET $URI_5K HTTP/1.1\r\nHost: x\r\n\r\n")"
check_alive

# 연결만 하고 아무것도 안 보내면 서버가 KEEP_ALIVE_TIMEOUT_SEC(5초) 뒤에 닫는다
idle_close_seconds() {
    exec 3<>/dev/tcp/$HOST/$PORT
    local start=$SECONDS
    cat <&3 > /dev/null   # 서버가 닫으면 EOF로 끝난다
    exec 3<&-
    echo $((SECONDS - start))
}
# 요청만 보내고 응답을 안 읽은 채 끊는 클라 두 번. SIGPIPE를 무시하지 않으면 여기서 서버가 죽는다
python3 - "$HOST" "$PORT" <<'EOF'
import socket, struct, sys, time
for linger in (True, False):  # RST로 끊기, FIN으로 끊기
    s = socket.create_connection((sys.argv[1], int(sys.argv[2])))
    if linger:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
    s.sendall(b'GET /photo.jpg HTTP/1.1\r\nHost: x\r\n\r\n')
    s.close()
    time.sleep(0.3)
EOF
check_alive

idle=$(idle_close_seconds)
check "가만히 있는 연결 → 4~6초 뒤 서버가 닫음" "ok" "$( [ "$idle" -ge 4 ] && [ "$idle" -le 6 ] && echo ok || echo "${idle}s")"
check_alive

# --- 7단계: CGI ---
check "GET adder?a=3&b=4 → 7" "7" "$(curl -s "$URL/cgi-bin/adder?a=3&b=4")"
check "POST adder 본문 a=10&b=-4 → 6" "6" "$(curl -s -d 'a=10&b=-4' "$URL/cgi-bin/adder")"
check "adder 입력 없음 → 200 + 안내" "200" "$(curl -s -o /dev/null -w '%{http_code}' "$URL/cgi-bin/adder")"
check "CGI 응답 Content-Type" "text/plain" "$(content_type_of '/cgi-bin/adder?a=1&b=1')"
check "CGI 응답도 keep-alive" "1 0 " \
    "$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' "$URL/cgi-bin/adder?a=1&b=1" "$URL/cgi-bin/adder?a=2&b=2")"
check "없는 프로그램 → 404" "404" "$(curl -s -o /dev/null -w '%{http_code}' "$URL/cgi-bin/nope")"
check "실행 권한 없음 → 403" "403" "$(curl -s -o /dev/null -w '%{http_code}' "$URL/cgi-bin/noexec.sh")"
check "프로그램이 실패(exit 1) → 500" "500" "$(curl -s -o /dev/null -w '%{http_code}' "$URL/cgi-bin/fail.sh")"
check "출력 형식이 틀림 → 500" "500" "$(curl -s -o /dev/null -w '%{http_code}' "$URL/cgi-bin/badout.sh")"
check "CGI 폴더 밖 → 404" "404" \
    "$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "$URL/cgi-bin/../server")"
check_alive
# 자식 프로세스를 waitpid로 정리하지 않으면 좀비(<defunct>)가 남는다
check "좀비 프로세스 없음" "0" "$(ps --ppid $SERVER_PID -o stat= | grep -c Z)"

# --- 8단계: Reverse Proxy ---
check "프록시 GET /api/adder → 7" "7" "$(curl -s "$URL/api/adder?a=3&b=4")"
check "프록시 POST /api/adder → 6" "6" "$(curl -s -d 'a=10&b=-4' "$URL/api/adder")"
check "프록시 응답 = 직접 요청 응답" "same" \
    "$(cmp -s <(curl -s "$URL/api/adder?a=5&b=6") <(curl -s "http://$HOST:$B1/cgi-bin/adder?a=5&b=6") && echo same)"
# 요청 4개를 보내고 뒤 서버 로그에서 각각 몇 개를 받았는지 센다
before1=$(grep -c 'adder?a=9' /tmp/web-backend1.log)
before2=$(grep -c 'adder?a=9' /tmp/web-backend2.log)
for i in 1 2 3 4; do curl -s -o /dev/null "$URL/api/adder?a=9&b=$i"; done
got1=$(( $(grep -c 'adder?a=9' /tmp/web-backend1.log) - before1 ))
got2=$(( $(grep -c 'adder?a=9' /tmp/web-backend2.log) - before2 ))
check "Round-Robin: 4개가 2/2로 나뉨" "2/2" "$got1/$got2"
check "프록시 JPG 바이트 그대로" "same" \
    "$(curl -s -o "$ROOT/.pgot" "$URL/img/photo.jpg"; cmp -s "$ROOT/.pgot" "$ROOT/photo.jpg" && echo same)"
check "프록시 JPG Content-Type" "image/jpeg" "$(content_type_of /img/photo.jpg)"
check "프록시 3MB 바이너리 그대로" "same" \
    "$(curl -s -o "$ROOT/.pgot" "$URL/img/big.bin"; cmp -s "$ROOT/.pgot" "$ROOT/big.bin" && echo same)"
check "뒤 서버의 404를 그대로 전달" "404" "$(curl -s -o /dev/null -w '%{http_code}' "$URL/api/nope")"
check "클라와는 keep-alive 유지" "1 0 " \
    "$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' "$URL/api/adder?a=1&b=1" "$URL/img/style.css")"
check "뒤 서버가 죽음 → 502" "502" "$(curl -s -o /dev/null -w '%{http_code}' "$URL/dead/")"
check "죽은 서버는 건너뛰고 다음 서버로" "200 200 " \
    "$(curl -s -o /dev/null -o /dev/null -w '%{http_code} ' "$URL/failover/adder?a=1&b=1" "$URL/failover/adder?a=2&b=2")"
check "규칙 밖 경로는 앞 서버가 직접 처리" "same" "$(same_as_file /index.html index.html)"
check_alive
# --- 9단계: 동시 처리 (스레드 풀) ---
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

# 가만히 있는 연결 하나를 붙잡아 둔 채 다른 요청을 보낸다. 반복 서버였다면 5초를 기다렸다
exec 4<>/dev/tcp/$HOST/$PORT
t0=$(now_ms)
code=$(curl -s -o /dev/null -w '%{http_code}' "$URL/")
t1=$(now_ms)
check "유휴 연결이 있어도 바로 응답 (1초 안)" "200 fast" "$code $( [ $((t1 - t0)) -lt 1000 ] && echo fast || echo "$((t1 - t0))ms")"
check "CGI에 소켓·파이프가 새지 않음" "socket=0 pipe=2" "$(curl -s "$URL/cgi-bin/fds.sh")"
exec 4<&-

check "동시 요청 50개 모두 200" "50" \
    "$(seq 50 | xargs -P 50 -I{} curl -s -o /dev/null -w '%{http_code}\n' "$URL/style.css?n={}" | grep -c '^200$')"

# 1초 걸리는 CGI 4개를 동시에: 하나씩이면 4초, 동시면 약 1초
t0=$(now_ms)
slept=$(seq 4 | xargs -P 4 -I{} curl -s "$URL/cgi-bin/sleep.sh?n={}" | grep -c slept)
t1=$(now_ms)
check "1초짜리 CGI 4개 동시 → 2초 안에 끝" "4 fast" "$slept $( [ $((t1 - t0)) -lt 2000 ] && echo fast || echo "$((t1 - t0))ms")"

# 동시에 들어온 프록시 요청 40개도 잠금 덕분에 정확히 20/20으로 나뉘어야 한다
before1=$(grep -c 'adder?a=77' /tmp/web-backend1.log)
before2=$(grep -c 'adder?a=77' /tmp/web-backend2.log)
seq 40 | xargs -P 20 -I{} curl -s -o /dev/null "$URL/api/adder?a=77&b={}"
got1=$(( $(grep -c 'adder?a=77' /tmp/web-backend1.log) - before1 ))
got2=$(( $(grep -c 'adder?a=77' /tmp/web-backend2.log) - before2 ))
check "동시 프록시 40개 → Round-Robin 20/20" "20/20" "$got1/$got2"
check_alive

# --- 10단계: 시간 제한과 안전한 종료 ---
t0=$(now_ms)
code=$(curl -s -o /dev/null -w '%{http_code}' "$URL/cgi-bin/hang.sh")
t1=$(now_ms)
check "멈춘 CGI → 504 (약 5초)" "504 ok" "$code $( [ $((t1 - t0)) -ge 4500 ] && [ $((t1 - t0)) -lt 7000 ] && echo ok || echo "$((t1 - t0))ms")"
sleep 0.2
check "  └ CGI가 띄운 sleep까지 끝남" "0" "$(pgrep -fc 'sleep 31')"
t0=$(now_ms)
code=$(curl -s -o /dev/null -w '%{http_code}' "$URL/hang/x")
t1=$(now_ms)
check "답 없는 뒤 서버 → 504 (약 5초)" "504 ok" "$code $( [ $((t1 - t0)) -ge 4500 ] && [ $((t1 - t0)) -lt 7000 ] && echo ok || echo "$((t1 - t0))ms")"
check_alive

# 따로 띄운 서버에 1초짜리 요청을 보내 둔 채로 Ctrl+C(SIGINT)를 보낸다
S_PORT=$((PORT + 30))
./server "$S_PORT" "$ROOT" "$CGI" > /tmp/web-shutdown.log 2>&1 &
S_PID=$!
sleep 0.5
curl -s "http://$HOST:$S_PORT/cgi-bin/sleep.sh" > "$ROOT/.inflight" &
CURL_PID=$!
sleep 0.3
kill -INT $S_PID
sleep 0.2
check "종료 중에는 새 연결을 안 받음" "000" "$(curl -s -o /dev/null -w '%{http_code}' "http://$HOST:$S_PORT/")"
timeout 10 tail --pid=$S_PID -f /dev/null
wait $S_PID; s_exit=$?
wait $CURL_PID
check "SIGINT → 하던 요청은 끝까지 응답" "slept" "$(cat "$ROOT/.inflight")"
check "SIGINT → 종료 코드 0, 'stopped' 출력" "0 1" "$s_exit $(grep -c '^stopped$' /tmp/web-shutdown.log)"

./server "$S_PORT" "$ROOT" "$CGI" > /tmp/web-shutdown.log 2>&1 &
S_PID=$!
sleep 0.5
curl -s -o /dev/null "http://$HOST:$S_PORT/"
kill -TERM $S_PID
timeout 10 tail --pid=$S_PID -f /dev/null
wait $S_PID; s_exit=$?
check "SIGTERM → 종료 코드 0" "0" "$s_exit"

check "틀린 규칙 파일 → 서버 시작 거부" "1" \
    "$(printf '/api/ /cgi-bin/\n' > "$CGI/bad.conf"; ./server $((PORT + 20)) "$ROOT" "$CGI" "$CGI/bad.conf" >/dev/null 2>&1; echo $?)"

echo
echo "통과 $pass, 실패 $fail"
[ "$fail" -eq 0 ]
