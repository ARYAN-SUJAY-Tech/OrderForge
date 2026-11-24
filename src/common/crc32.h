/*
 * crc32.h - CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), used by the WAL to
 * detect torn or corrupted records. crc32("123456789") == 0xCBF43926.
 */
#ifndef OF_CRC32_H
#define OF_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Start with crc = 0. Feed the previous result back in to checksum data in pieces. */
uint32_t of_crc32(uint32_t crc, const void *data, size_t len);

#endif
