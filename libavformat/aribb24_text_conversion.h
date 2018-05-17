#ifndef AVFORMAT_LIBARIBB24_HELPERS_H
#define AVFORMAT_LIBARIBB24_HELPERS_H

#include <stdint.h>

/* ISO 10646 value used to signal invalid value.  */
#define __UNKNOWN_10646_CHAR    ((uint16_t) 0xfffd)

#include "jis0201.h"
#include "jis0208.h"
#include "jisx0213.h"

int aribb24_to_ucs2(void *state, const unsigned char *inptr, const unsigned char *inend,
                    const unsigned char *outptr, unsigned char *outend,
                    size_t *irreversible);

#endif /* AVFORMAT_LIBARIBB24_HELPERS_H */
