#include <string.h>

size_t strnlen(const char* str, size_t max)
{
	const char* p = str;
	const char* e = str + max;
	size_t len = 0;

	while(p < e && *p++) len++;

	return len;
}
