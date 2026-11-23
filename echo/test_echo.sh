#!/usr/bin/env bash
# 책 11.4절 hostinfo · echo 서버 · echo 클라이언트 동작 확인.
# 사용법: make test            (보통 빌드)
#         make asan test       (AddressSanitizer·UBSan 빌드로 메모리 오류까지 확인)
#         bash test_echo.sh [포트]
set -u
cd "$(dirname "$0")"

PORT="${1:-15213}"
TMP="$(mktemp -d)"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "  [PASS] $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  [FAIL] $1"; }

# 입력 파일을 echoclient 로 보내고, 돌아온 출력이 입력과 같은지 본다.
# 서버가 응답하지 않아도 테스트가 멈추지 않도록 5초 제한을 둔다.
echo_matches() { # $1 = 입력 파일, $2 = 결과 이름
    timeout 5 ./echoclient localhost "$PORT" <"$1" >"$TMP/$2.out" 2>"$TMP/$2.err" \
        && cmp -s "$1" "$TMP/$2.out"
}

./echoserveri "$PORT" >"$TMP/server.out" 2>"$TMP/server.err" &
SERVER=$!
cleanup() {
    kill "$SERVER" 2>/dev/null
    wait "$SERVER" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# 서버가 listen 할 때까지 기다린다.
# 연결에 성공한 시도는 "연결만 하고 데이터 없이 끊는 클라이언트" 역할도 한다.
ready=0
for _ in $(seq 50); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
        ready=1
        break
    fi
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then
    echo "서버가 포트 $PORT 에서 열리지 않았다"
    cat "$TMP/server.err"
    exit 1
fi

echo "1. hostinfo (그림 11.17)"
if ./hostinfo localhost >"$TMP/hostinfo.out" 2>&1 && grep -qx '127.0.0.1' "$TMP/hostinfo.out"; then
    pass "localhost 를 127.0.0.1 로 변환한다"
else
    fail "localhost 변환 결과: $(cat "$TMP/hostinfo.out")"
fi

echo "2. echo (그림 11.20 ~ 11.22)"
printf 'hello\n\nworld\n한글 줄도 그대로\n' >"$TMP/lines.in"
if echo_matches "$TMP/lines.in" lines; then
    pass "여러 줄(빈 줄, 한글 포함)이 그대로 돌아온다"
else
    fail "여러 줄 echo 결과가 다르다"
fi

# MAXLINE(8192) 보다 긴 줄은 클라이언트·서버 모두 8191 바이트씩 나눠 읽는다.
{ head -c 10000 /dev/zero | tr '\0' 'a'; echo; } >"$TMP/long.in"
if echo_matches "$TMP/long.in" long; then
    pass "MAXLINE 보다 긴 10000 바이트 줄도 그대로 돌아온다"
else
    fail "긴 줄 echo 결과가 다르다"
fi

echo "3. 연결이 끝난 뒤 다음 연결"
ok=1
for i in 1 2 3; do
    echo_matches "$TMP/lines.in" "seq$i" || ok=0
done
if [ "$ok" -eq 1 ]; then
    pass "클라이언트 3개를 차례로 받아 모두 echo 한다"
else
    fail "차례로 접속한 클라이언트 중 실패가 있다"
fi

# 한 줄을 주고받은 뒤 클라이언트 프로세스가 SIGKILL 로 죽는다.
# 커널이 소켓을 닫아 FIN 을 보내므로 서버의 Rio_readlineb 는 EOF(0) 를 받는다.
# (끝의 true 는 서브셸이 "Killed" 알림을 /dev/null 로 출력하게 하려는 것이다)
( bash -c 'exec 3<>"/dev/tcp/127.0.0.1/$1"; printf "bye\n" >&3; read -r -t 5 _ <&3; kill -9 $$' _ "$PORT"; true ) 2>/dev/null
if echo_matches "$TMP/lines.in" afterkill; then
    pass "통신 중 강제 종료된 클라이언트 다음 연결도 받는다"
else
    fail "강제 종료된 클라이언트 다음 연결이 실패한다"
fi

if kill -0 "$SERVER" 2>/dev/null; then
    pass "모든 테스트가 끝날 때까지 서버가 살아 있다"
else
    fail "서버가 도중에 종료됐다: $(cat "$TMP/server.err")"
fi

echo "4. sanitizer 보고"
if grep -lE 'AddressSanitizer|LeakSanitizer|runtime error' "$TMP"/*.err "$TMP/hostinfo.out" >/dev/null 2>&1; then
    fail "sanitizer 가 오류를 보고했다"
    grep -hE -A5 'AddressSanitizer|LeakSanitizer|runtime error' "$TMP"/*.err "$TMP/hostinfo.out"
else
    pass "sanitizer 오류 보고가 없다 (sanitizer 빌드가 아니면 항상 통과)"
fi

echo
echo "결과: 통과 $PASS, 실패 $FAIL"
[ "$FAIL" -eq 0 ]
