/*
 * This code implements the BASE64 algorithm.
 * This code is in the public domain; do with it what you wish.
 *
 * @file base64.c
 * @brief This code implements the BASE64 algorithm
 * @author Matthieu Speder
 */

#include <memory.h>
#include <stdlib.h>
#include "base64.h"

static const char base64_chars[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const char base64_digits[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 62, 0, 0, 0, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
		0, 0, 0, -1, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
		14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 0, 0, 0, 0, 0, 0, 26,
		27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
		45, 46, 47, 48, 49, 50, 51, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

#if 0
char* BASE64Encode(const char* src) {
	unsigned int in_len = strlen(src);
	char* dest = malloc((in_len + 2 - ((in_len + 2) % 3)) / 3 * 4 + 1);
	char* result = dest;

	if (dest == NULL)
	   return NULL;
	while (*src) {
		dest[0] = base64_chars[(src[0] & 0xfc) >> 2];
		dest[1] = base64_chars[((src[0] & 0x03) << 4) + ((src[1] & 0xf0) >> 4)];
		dest[2] = base64_chars[((src[1] & 0x0f) << 2) + ((src[2] & 0xc0) >> 6)];
		dest[3] = base64_chars[src[2] & 0x3f];
		if (!*(++src)) {
			dest[2] = dest[3] = '=';
			dest[4] = 0;
			return result;
		}
		if (!*(++src)) {
			dest[3] = '=';
			dest[4] = 0;
			return result;
		}
		src++;
		dest += 4;
	}
	*dest = 0;
	return result;

}
#endif

char* BASE64Decode(const char* src) {
	unsigned int in_len = strlen(src);
	char* dest;
	char* result;

	if (in_len % 4) {
		/* Wrong base64 string length */
		return NULL;
	}
	result = dest = (char*) malloc(in_len / 4 * 3 + 1);

	while (*src) {
		char a = base64_digits[(unsigned char)*(src++)];
		char b = base64_digits[(unsigned char)*(src++)];
		char c = base64_digits[(unsigned char)*(src++)];
		char d = base64_digits[(unsigned char)*(src++)];
		*(dest++) = (a << 2) | ((b & 0x30) >> 4);
		if (c == -1)
			break;
		*(dest++) = ((b & 0x0f) << 4) | ((c & 0x3c) >> 2);
		if (d == -1)
			break;
		*(dest++) = ((c & 0x03) << 6) | d;
	}
	*dest = 0;
	return result;
}

/* end of base64.c */
