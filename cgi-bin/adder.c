/* 테스트용 CGI 프로그램: 입력 "a=3&b=4"를 받아 a+b를 출력한다.
 * GET이면 환경변수 QUERY_STRING에서, POST면 표준입력(본문)에서 입력을 읽는다.
 * 서버가 실행하는 프로그램이라 서버 코드와 따로 빌드한다. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_MAX (1024 * 1024)

/* "a=3&b=4" 같은 입력에서 이름이 key인 값을 정수로 꺼낸다. 찾으면 1, 없거나 숫자가 아니면 0. */
static int find_int_param(const char *input, const char *key, long *out)
{
    size_t key_len = strlen(key);
    const char *p = input;
    while (*p != '\0') {
        /* 칸 하나는 '&'까지. 칸이 "key="로 시작하면 그 뒤가 값이다 */
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            char *end;
            long v = strtol(p + key_len + 1, &end, 10);
            if (end == p + key_len + 1 || (*end != '\0' && *end != '&'))
                return 0;
            *out = v;
            return 1;
        }
        const char *amp = strchr(p, '&');
        if (amp == NULL)
            break;
        p = amp + 1;
    }
    return 0;
}

/* 메서드에 따라 입력을 읽고, a+b 또는 안내 문구를 CGI 형식(머리, 빈 줄, 본문)으로 출력한다. */
int main(void)
{
    const char *method = getenv("REQUEST_METHOD");
    char *input = NULL;

    if (method != NULL && strcmp(method, "POST") == 0) {
        const char *len_str = getenv("CONTENT_LENGTH");
        long len = len_str ? strtol(len_str, NULL, 10) : 0;
        if (len < 0 || len > INPUT_MAX)
            return 1;
        input = calloc((size_t)len + 1, 1);
        if (input == NULL)
            return 1;
        if (fread(input, 1, (size_t)len, stdin) != (size_t)len) {
            free(input);
            return 1; /* 본문이 덜 왔다 */
        }
    } else {
        const char *query = getenv("QUERY_STRING");
        input = strdup(query ? query : "");
        if (input == NULL)
            return 1;
    }

    long a, b;
    printf("Content-Type: text/plain\r\n\r\n");
    if (find_int_param(input, "a", &a) && find_int_param(input, "b", &b))
        printf("%ld\n", a + b);
    else
        printf("need a and b, e.g. a=3&b=4\n");

    free(input);
    return 0;
}
