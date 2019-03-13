#ifndef UTIL_H
#define UTIL_H

#include <limits.h>

#define __MAX_STRLEN_FOR_UNSIGNED_TYPE(t) \
	(((((sizeof(t) * CHAR_BIT)) * 1233) >> 12) + 1)

#define __MAX_STRLEN_FOR_SIGNED_TYPE(t) \
	(__MAX_STRLEN_FOR_UNSIGNED_TYPE(t) + 1)

#define MAX_STRLEN_FOR_INT_TYPE(t) \
	(((t) -1 < 0) ? __MAX_STRLEN_FOR_SIGNED_TYPE(t) \
				  : __MAX_STRLEN_FOR_UNSIGNED_TYPE(t))

#ifndef NDEBUG

	#define UNREACHABLE() do { \
		fprintf(stderr, "%s[%d]@%s(): reached unreachable code.\n", \
		    __FILE__, __LINE__, __func__); \
		abort(); \
	} while(0) \

#else

	#define UNREACHABLE()

#endif

#endif
