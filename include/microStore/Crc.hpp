#pragma once

#include <stdint.h>
#include <string.h>

namespace microStore {

	class Crc {

	public:
 		static uint32_t crc32(uint32_t crc, const uint8_t* buffer, size_t size) {
			const unsigned char *data = (const unsigned char *)buffer;
			if (data == NULL)
				return 0;
			crc ^= 0xffffffff;
			while (size--) {
				crc ^= *data++;
				for (unsigned k = 0; k < 8; k++)
					crc = crc & 1 ? (crc >> 1) ^ 0xedb88320 : crc >> 1;
			}
			return crc ^ 0xffffffff;
		}
 		static inline uint32_t crc32(uint32_t crc, uint8_t byte) { return crc32(crc, &byte, sizeof(byte)); }
 		static inline uint32_t crc32(uint32_t crc, const char* str) { return crc32(crc, (const uint8_t*)str, strlen(str)); }

	};

}
