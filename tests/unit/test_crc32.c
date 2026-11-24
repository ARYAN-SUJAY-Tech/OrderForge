#include "check.h"
#include "common/crc32.h"

int main(void)
{
    CHECK(of_crc32(0, "", 0) == 0);
    CHECK(of_crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK(of_crc32(0, "The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u);

    /* Incremental use gives the same result as one call. */
    uint32_t c = of_crc32(0, "1234", 4);
    c = of_crc32(c, "56789", 5);
    CHECK(c == 0xCBF43926u);

    /* A single flipped bit changes the checksum. */
    char buf[] = "123456789";
    buf[4] ^= 1;
    CHECK(of_crc32(0, buf, 9) != 0xCBF43926u);

    CHECK_DONE();
}
