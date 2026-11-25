/* From the repository root:
 * Linux: cc -std=c11 -Wall -Wextra -Werror lab/tests/sha256_test.c -o /tmp/sha256_test && /tmp/sha256_test
 * Windows (VS Developer Command Prompt): cl /nologo /std:c11 /W4 /WX lab\tests\sha256_test.c /Fe:sha256_test.exe && sha256_test.exe
 */
#include <stdio.h>
#include <string.h>
#include "../../firmware/hv_sha256.h"

static int check(const char *name, const uint8_t *data, size_t bytes, const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[32];
    char actual[65];
    hv_sha256(data, bytes, digest);
    for (unsigned i = 0; i < 32; ++i) {
        actual[2 * i] = hex[digest[i] >> 4];
        actual[2 * i + 1] = hex[digest[i] & 15];
    }
    actual[64] = '\0';
    if (strcmp(actual, expected) == 0) return 0;
    fprintf(stderr, "%s: expected %s, got %s\n", name, expected, actual);
    return 1;
}

int main(void)
{
    static const uint8_t abc[] = "abc";
    static const uint8_t nist56[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t a55[55], binary[257];
    int failed = 0;
    memset(a55, 'a', sizeof(a55));
    for (unsigned i = 0; i < 256; ++i) binary[i + 1] = (uint8_t)i;

    failed |= check("empty", NULL, 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    failed |= check("NIST abc", abc, sizeof(abc) - 1,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    /* hashlib.sha256(b'a' * 55).hexdigest(): padding just fits in one block. */
    failed |= check("55 bytes", a55, sizeof(a55),
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    failed |= check("NIST 56 bytes", nist56, sizeof(nist56) - 1,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    /* hashlib.sha256(bytes(range(256))).hexdigest(): four full input blocks. */
    failed |= check("binary 0..255", binary + 1, 256,
        "40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880");
    return failed;
}
