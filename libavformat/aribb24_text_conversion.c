/*
 * Conversion module for ARIB-STD-B24.
 * Copyright (C) 2014 Akihiro Tsukada
 *
 * Adapted from linuxtv v4l-utils' contrib/gconv/arib-std-b24.c by
 * Jan Ekström.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Conversion module for the character encoding
 * defined in ARIB STD-B24 Volume 1, Part 2, Chapter 7.
 *    http://www.arib.or.jp/english/html/overview/doc/6-STD-B24v5_2-1p3-E1.pdf
 *    http://www.arib.or.jp/english/html/overview/sb_ej.html
 *    https://sites.google.com/site/unicodesymbols/Home/japanese-tv-symbols/
 * It is based on ISO-2022, and used in Japanese digital televsion.
 *
 * Note 1: "mosaic" characters are not supported in this module.
 * Note 2: Control characters (for subtitles) are discarded.
 */

#include <aribb24/aribb24.h>
#include <aribb24/decoder.h>

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"

#include "aribb24_text_conversion.h"

/* This makes obvious what everybody knows: 0x1b is the Esc character.  */
#define ESC 0x1b
/* other control characters */
#define SS2 0x19
#define SS3 0x1d
#define LS0 0x0f
#define LS1 0x0e

#define LS2 0x6e
#define LS3 0x6f
#define LS1R 0x7e
#define LS2R 0x7d
#define LS3R 0x7c

#define LF 0x0a
#define CR 0x0d
#define BEL 0x07
#define BS 0x08
#define COL 0x90
#define CDC 0x92
#define MACRO_CTRL 0x95
#define CSI 0x9b
#define TIME 0x9d


/* First define the conversion function from ARIB-STD-B24 to UCS-4.  */

/* code sets */
enum g_set
{
  KANJI_set = '\x42',         /* 2Byte set */
  ASCII_set = '\x40',
  ASCII_x_set = '\x4a',
  HIRAGANA_set = '\x30',
  KATAKANA_set = '\x31',
  MOSAIC_A_set = '\x32',
  MOSAIC_B_set = '\x33',
  MOSAIC_C_set = '\x34',
  MOSAIC_D_set = '\x35',
  PROP_ASCII_set = '\x36',
  PROP_HIRA_set = '\x37',
  PROP_KATA_set = '\x38',
  JIS0201_KATA_set = '\x49',
  JISX0213_1_set = '\x39',    /* 2Byte set */
  JISX0213_2_set = '\x3a',    /* 2Byte set */
  EXTRA_SYMBOLS_set = '\x3b', /* 2Byte set */

  DRCS0_set = 0x40 | 0x80,    /* 2Byte set */
  DRCS1_set = 0x41 | 0x80,
  DRCS15_set = 0x4f | 0x80,
  MACRO_set = 0x70 | 0x80,
};

enum mode_e
{
  NORMAL,
  ESCAPE,
  G_SEL_1B,
  G_SEL_MB,
  CTRL_SEQ,
  DESIGNATE_MB,
  DRCS_SEL_1B,
  DRCS_SEL_MB,
  MB_2ND,
};

/*
 * __GCONV_INPUT_INCOMPLETE is never used in this conversion, thus
 * we can re-use mbstate_t.__value and .__count:3 for the other purpose.
 */
struct state_from {
  /* __count */
  uint8_t cnt:3;    /* for use in skelton.c. always 0 */
  uint8_t pad0:1;
  uint8_t gl:2;     /* idx of the G-set invoked into GL */
  uint8_t gr:2;     /*  ... to GR */
  uint8_t ss:2;     /* SS state. 0: no shift, 2:SS2, 3:SS3 */
  uint8_t gidx:2;   /* currently designated G-set */
  uint8_t mode:4;   /* current input mode. see below. */
  uint8_t skip;     /* [CTRL_SEQ] # of char to skip */
  uint8_t prev;     /* previously input char [in MB_2ND] or,*/
            /* input char to wait for. [CTRL_SEQ (.skip == 0)] */

  /* __value */
  uint8_t g[4];     /* code set for G0..G3 */
} __attribute__((packed));

static const struct state_from def_state_from = {
  .cnt = 0,
  .gl = 0,
  .gr = 2,
  .ss = 0,
  .gidx = 0,
  .mode = NORMAL,
  .skip = 0,
  .prev = '\0',
  .g[0] = KANJI_set,
  .g[1] = ASCII_set,
  .g[2] = HIRAGANA_set,
  .g[3] = KATAKANA_set,
};

/* tables and functions used in BODY */

static const uint16_t kata_punc[] = {
  0x30fd, 0x30fe, 0x30fc, 0x3002, 0x300c, 0x300d, 0x3001, 0x30fb
};

static const uint16_t hira_punc[] = {
  0x309d, 0x309e
};

static const uint16_t nonspacing_symbol[] = {
  0x0301, 0x0300, 0x0308, 0x0302, 0x0304, 0x0332
};

static const uint32_t extra_kanji[] = {
  /* row 85 */
  /* col 0..15 */
  0, 0x3402, 0x20158, 0x4efd, 0x4eff, 0x4f9a, 0x4fc9, 0x509c,
  0x511e, 0x51bc, 0x351f, 0x5307, 0x5361, 0x536c, 0x8a79, 0x20bb7,
  /* col. 16..31 */
  0x544d, 0x5496, 0x549c, 0x54a9, 0x550e, 0x554a, 0x5672, 0x56e4,
  0x5733, 0x5734, 0xfa10, 0x5880, 0x59e4, 0x5a23, 0x5a55, 0x5bec,
  /* col. 32..47 */
  0xfa11, 0x37e2, 0x5eac, 0x5f34, 0x5f45, 0x5fb7, 0x6017, 0xfa6b,
  0x6130, 0x6624, 0x66c8, 0x66d9, 0x66fa, 0x66fb, 0x6852, 0x9fc4,
  /* col. 48..63 */
  0x6911, 0x693b, 0x6a45, 0x6a91, 0x6adb, 0x233cc, 0x233fe, 0x235c4,
  0x6bf1, 0x6ce0, 0x6d2e, 0xfa45, 0x6dbf, 0x6dca, 0x6df8, 0xfa46,
  /* col. 64..79 */
  0x6f5e, 0x6ff9, 0x7064, 0xfa6c, 0x242ee, 0x7147, 0x71c1, 0x7200,
  0x739f, 0x73a8, 0x73c9, 0x73d6, 0x741b, 0x7421, 0xfa4a, 0x7426,
  /* col. 80..96 */
  0x742a, 0x742c, 0x7439, 0x744b, 0x3eda, 0x7575, 0x7581, 0x7772,
  0x4093, 0x78c8, 0x78e0, 0x7947, 0x79ae, 0x9fc6, 0x4103, 0,

  /* row 86 */
  /* col 0..15 */
  0, 0x9fc5, 0x79da, 0x7a1e, 0x7b7f, 0x7c31, 0x4264, 0x7d8b,
  0x7fa1, 0x8118, 0x813a, 0xfa6d, 0x82ae, 0x845b, 0x84dc, 0x84ec,
  /* col. 16..31 */
  0x8559, 0x85ce, 0x8755, 0x87ec, 0x880b, 0x88f5, 0x89d2, 0x8af6,
  0x8dce, 0x8fbb, 0x8ff6, 0x90dd, 0x9127, 0x912d, 0x91b2, 0x9233,
  /* col. 32..43 */
  0x9288, 0x9321, 0x9348, 0x9592, 0x96de, 0x9903, 0x9940, 0x9ad9,
  0x9bd6, 0x9dd7, 0x9eb4, 0x9eb5
};

static const uint32_t extra_symbols[5][96] = {
  /* row 90 */
  {
    /* col 0..15 */
    0, 0x26cc, 0x26cd, 0x2762, 0x26cf, 0x26d0, 0x26d1, 0,
    0x26d2, 0x26d5, 0x26d3, 0x26d4, 0, 0, 0, 0,
    /* col 16..31 */
    0x1f17f, 0x1f18a, 0, 0, 0x26d6, 0x26d7, 0x26d8, 0x26d9,
    0x26da, 0x26db, 0x26dc, 0x26dd, 0x26de, 0x26df, 0x26e0, 0x26e1,
    /* col 32..47 */
    0x2b55, 0x3248, 0x3249, 0x324a, 0x324b, 0x324c, 0x324d, 0x324e,
    0x324f, 0, 0, 0, 0, 0x2491, 0x2492, 0x2493,
    /* col 48..63 */
    0x1f14a, 0x1f14c, 0x1f13F, 0x1f146, 0x1f14b, 0x1f210, 0x1f211, 0x1f212,
    0x1f213, 0x1f142, 0x1f214, 0x1f215, 0x1f216, 0x1f14d, 0x1f131, 0x1f13d,
    /* col 64..79 */
    0x2b1b, 0x2b24, 0x1f217, 0x1f218, 0x1f219, 0x1f21a, 0x1f21b, 0x26bf,
    0x1f21c, 0x1f21d, 0x1f21e, 0x1f21f, 0x1f220, 0x1f221, 0x1f222, 0x1f223,
    /* col 80..95 */
    0x1f224, 0x1f225, 0x1f14e, 0x3299, 0x1f200, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
  },
  /* row 91 */
  {
    /* col 0..15 */
    0, 0x26e3, 0x2b56, 0x2b57, 0x2b58, 0x2b59, 0x2613, 0x328b,
    0x3012, 0x26e8, 0x3246, 0x3245, 0x26e9, 0x0fd6, 0x26ea, 0x26eb,
    /* col 16..31 */
    0x26ec, 0x2668, 0x26ed, 0x26ee, 0x26ef, 0x2693, 0x1f6e7, 0x26f0,
    0x26f1, 0x26f2, 0x26f3, 0x26f4, 0x26f5, 0x1f157, 0x24b9, 0x24c8,
    /* col 32..47 */
    0x26f6, 0x1f15f, 0x1f18b, 0x1f18d, 0x1f18c, 0x1f179, 0x26f7, 0x26f8,
    0x26f9, 0x26fa, 0x1f17b, 0x260e, 0x26fb, 0x26fc, 0x26fd, 0x26fe,
    /* col 48..63 */
    0x1f17c, 0x26ff,
  },
  /* row 92 */
  {
    /* col 0..15 */
    0, 0x27a1, 0x2b05, 0x2b06, 0x2b07, 0x2b2f, 0x2b2e, 0x5e74,
    0x6708, 0x65e5, 0x5186, 0x33a1, 0x33a5, 0x339d, 0x33a0, 0x33a4,
    /* col 16..31 */
    0x1f100, 0x2488, 0x2489, 0x248a, 0x248b, 0x248c, 0x248d, 0x248e,
    0x248f, 0x2490, 0, 0, 0, 0, 0, 0,
    /* col 32..47 */
    0x1f101, 0x1f102, 0x1f103, 0x1f104, 0x1f105, 0x1f106, 0x1f107, 0x1f108,
    0x1f109, 0x1f10a, 0x3233, 0x3236, 0x3232, 0x3231, 0x3239, 0x3244,
    /* col 48..63 */
    0x25b6, 0x25c0, 0x3016, 0x3017, 0x27d0, 0x00b2, 0x00b3, 0x1f12d,
    0, 0, 0, 0, 0, 0, 0, 0,
    /* col 64..79 */
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    /* col 80..95 */
    0, 0, 0, 0, 0, 0, 0x1f12c, 0x1f12b,
    0x3247, 0x1f190, 0x1f226, 0x213b, 0, 0, 0, 0
  },
  /* row 93 */
  {
    /* col 0..15 */
    0, 0x322a, 0x322b, 0x322c, 0x322d, 0x322e, 0x322f, 0x3230,
    0x3237, 0x337e, 0x337d, 0x337c, 0x337b, 0x2116, 0x2121, 0x3036,
    /* col 16..31 */
    0x26be, 0x1f240, 0x1f241, 0x1f242, 0x1f243, 0x1f244, 0x1f245, 0x1f246,
    0x1f247, 0x1f248, 0x1f12a, 0x1f227, 0x1f228, 0x1f229, 0x1f214, 0x1f22a,
    /* col 32..47 */
    0x1f22b, 0x1f22c, 0x1f22d, 0x1f22e, 0x1f22f, 0x1f230, 0x1f231, 0x2113,
    0x338f, 0x3390, 0x33ca, 0x339e, 0x33a2, 0x3371, 0, 0,
    /* col 48..63 */
    0x00bd, 0x2189, 0x2153, 0x2154, 0x00bc, 0x00be, 0x2155, 0x2156,
    0x2157, 0x2158, 0x2159, 0x215a, 0x2150, 0x215b, 0x2151, 0x2152,
    /* col 64..79 */
    0x2600, 0x2601, 0x2602, 0x26c4, 0x2616, 0x2617, 0x26c9, 0x26ca,
    0x2666, 0x2665, 0x2663, 0x2660, 0x26cb, 0x2a00, 0x203c, 0x2049,
    /* col 80..95 */
    0x26c5, 0x2614, 0x26c6, 0x2603, 0x26c7, 0x26a1, 0x26c8, 0,
    0x269e, 0x269f, 0x266c, 0x260e, 0, 0, 0, 0
  },
  /* row 94 */
  {
    /* col 0..15 */
    0, 0x2160, 0x2161, 0x2162, 0x2163, 0x2164, 0x2165, 0x2166,
    0x2167, 0x2168, 0x2169, 0x216a, 0x216b, 0x2470, 0x2471, 0x2472,
    /* col 16..31 */
    0x2473, 0x2474, 0x2475, 0x2476, 0x2477, 0x2478, 0x2479, 0x247a,
    0x247b, 0x247c, 0x247d, 0x247e, 0x247f, 0x3251, 0x3252, 0x3253,
    /* col 32..47 */
    0x3254, 0x1f110, 0x1f111, 0x1f112, 0x1f113, 0x1f114, 0x1f115, 0x1f116,
    0x1f117, 0x1f118, 0x1f119, 0x1f11a, 0x1f11b, 0x1f11c, 0x1f11d, 0x1f11e,
    /* col 48..63 */
    0x1f11f, 0x1f120, 0x1f121, 0x1f122, 0x1f123, 0x1f124, 0x1f125, 0x1f126,
    0x1f127, 0x1f128, 0x1f129, 0x3255, 0x3256, 0x3257, 0x3258, 0x3259,
    /* col 64..79 */
    0x325a, 0x2460, 0x2461, 0x2462, 0x2463, 0x2464, 0x2465, 0x2466,
    0x2467, 0x2468, 0x2469, 0x246a, 0x246b, 0x246c, 0x246d, 0x246e,
    /* col 80..95 */
    0x246f, 0x2776, 0x2777, 0x2778, 0x2779, 0x277a, 0x277b, 0x277c,
    0x277d, 0x277e, 0x277f, 0x24eb, 0x24ec, 0x325b, 0, 0
  },
};

struct mchar_entry {
  uint32_t len;
  uint32_t to[4];
};

/* list of transliterations. */

/* small/subscript-ish KANJI. map to the normal sized version */
static const struct mchar_entry ext_sym_smallk[] = {
  {.len = 1, .to = { 0x6c0f }},
  {.len = 1, .to = { 0x526f }},
  {.len = 1, .to = { 0x5143 }},
  {.len = 1, .to = { 0x6545 }},
  {.len = 1, .to = { 0x52ed }},
  {.len = 1, .to = { 0x65b0 }},
};

/* symbols of music instruments */
static const struct mchar_entry ext_sym_music[] = {
  {.len = 4, .to = { 0x0028, 0x0076, 0x006e, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x006f, 0x0062, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x0063, 0x0062, 0x0029 }},
  {.len = 3, .to = { 0x0028, 0x0063, 0x0065 }},
  {.len = 3, .to = { 0x006d, 0x0062, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x0068, 0x0070, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x0062, 0x0072, 0x0029 }},
  {.len = 3, .to = { 0x0028, 0x0070, 0x0029 }},

  {.len = 3, .to = { 0x0028, 0x0073, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x006d, 0x0073, 0x0029 }},
  {.len = 3, .to = { 0x0028, 0x0074, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x0062, 0x0073, 0x0029 }},
  {.len = 3, .to = { 0x0028, 0x0062, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x0074, 0x0062, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x0076, 0x0070, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x0064, 0x0073, 0x0029 }},

  {.len = 4, .to = { 0x0028, 0x0061, 0x0067, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x0065, 0x0067, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x0076, 0x006f, 0x0029 }},
  {.len = 4, .to = { 0x0028, 0x0066, 0x006c, 0x0029 }},
  {.len = 3, .to = { 0x0028, 0x006b, 0x0065 }},
  {.len = 2, .to = { 0x0079, 0x0029 }},
  {.len = 3, .to = { 0x0028, 0x0073, 0x0061 }},
  {.len = 2, .to = { 0x0078, 0x0029 }},

  {.len = 3, .to = { 0x0028, 0x0073, 0x0079 }},
  {.len = 2, .to = { 0x006e, 0x0029 }},
  {.len = 3, .to = { 0x0028, 0x006f, 0x0072 }},
  {.len = 2, .to = { 0x0067, 0x0029 }},
  {.len = 3, .to = { 0x0028, 0x0070, 0x0065 }},
  {.len = 2, .to = { 0x0072, 0x0029 }},
};


static int b24_char_conv (enum g_set set, unsigned char c1, unsigned char c2, uint32_t *out)
{
  int len;
  uint32_t ch;

  if (set > DRCS0_set && set <= DRCS15_set)
    set = DRCS0_set;

  switch (set)
    {
      case ASCII_set:
      case ASCII_x_set:
      case PROP_ASCII_set:
    if (c1 == 0x7e)
      *out = 0x203e;
    else if (c1 == 0x5c)
      *out = 0xa5;
    else
      *out = c1;
    return 1;

      case KATAKANA_set:
      case PROP_KATA_set:
    if (c1 <= 0x76)
      *out = 0x3080 + c1;
    else
      *out = kata_punc[c1 - 0x77];
    return 1;

      case HIRAGANA_set:
      case PROP_HIRA_set:
    if (c1 <= 0x73)
      *out = 0x3020 + c1;
    else if (c1 == 0x77 || c1 == 0x78)
      *out = hira_punc[c1 - 0x77];
    else if (c1 >= 0x79)
      *out = kata_punc[c1 - 0x77];
    else
      return 0;
    return 1;

      case JIS0201_KATA_set:
    if (c1 > 0x5f)
      return 0;
    *out = 0xff40 + c1;
    return 1;

      case EXTRA_SYMBOLS_set:
    if (c1 == 0x75 || (c1 == 0x76 && (c2 - 0x20) <=43))
      {
        *out = extra_kanji[(c1 - 0x75) * 96 + (c2 - 0x20)];
        return 1;
      }
    /* fall through */
      case KANJI_set:
    /* check extra symbols */
    if (c1 >= 0x7a && c1 <= 0x7e)
      {
        const struct mchar_entry *entry;

        c1 -= 0x20;
        c2 -= 0x20;
        if (c1 == 0x5c && c2 >= 0x1a && c2 <= 0x1f)
          entry = &ext_sym_smallk[c2 - 0x1a];
        else if (c1 == 0x5c && c2 >= 0x38 && c2 <= 0x55)
          entry = &ext_sym_music[c2 - 0x38];
        else
          entry = NULL;

        if (entry)
          {
        int i;

        for (i = 0; i < entry->len; i++)
          out[i] = entry->to[i];
        return i;
          }

        *out = extra_symbols[c1 - 0x5a][c2];
        if (*out == 0)
          return 0;

        return 1;
      }
    if (set == EXTRA_SYMBOLS_set)
      return 0;

    /* non-JISX0213 modification. (combining chars) */
    if (c1 == 0x22 && c2 == 0x7e)
      {
        *out = 0x20dd;
        return 1;
      }
    else if (c1 == 0x21 && c2 >= 0x2d && c2 <= 0x32)
      {
        *out = nonspacing_symbol[c2 - 0x2d];
        return 1;
      }
    /* fall through */
      case JISX0213_1_set:
      case JISX0213_2_set:
    len = 1;
    ch = jisx0213_to_ucs4(c1 | (set == JISX0213_2_set ? 0x0200 : 0x0100),
                  c2);
    if (ch == 0)
      return 0;
    if (ch < 0x80)
      {
        len = 2;
        out[0] = __jisx0213_to_ucs_combining[ch - 1][0];
        out[1] = __jisx0213_to_ucs_combining[ch - 1][1];
      }
    else
      *out = ch;
    return len;

      case MOSAIC_A_set:
      case MOSAIC_B_set:
      case MOSAIC_C_set:
      case MOSAIC_D_set:
      case DRCS0_set:
      case MACRO_set:
    *out = __UNKNOWN_10646_CHAR;
    return 1;

      default:
    break;
    }

  return 0;
}

int aribb24_to_ucs2(void *state, const unsigned char *inptr, const unsigned char *inend,
                    const unsigned char *outptr, unsigned char *outend,
                    size_t *irreversible)
{
    struct state_from st = *(struct state_from *)state;
    int result = AVERROR_INVALIDDATA;

    if (st.g[0] == 0)
        st = def_state_from;

    while (inptr != inend) {
        uint32_t ch = *inptr;
        if (ch == 0)
          {
        st.mode = NORMAL;
        ++ inptr;
        continue;
          }
        if (st.mode == CTRL_SEQ)
          {
        if (st.skip)
          {
            --st.skip;
            if (st.skip == 0)
              st.mode = NORMAL;
            if (ch < 0x40 || ch > 0x7f)
              // if ignoring errors skip 1 bytes
              break;
          }
        else if (st.prev == MACRO_CTRL)
          {
            if (ch == MACRO_CTRL)
              st.skip = 1;
            else if (ch == LF || ch == CR) {
              st = def_state_from;
              AV_WB32(outptr, ch);
              outptr += 4;
            }
          }
        else if (st.prev == CSI && (ch == 0x5b || ch == 0x5c || ch == 0x6f))
          st.mode = NORMAL;
        else if (st.prev == TIME || st.prev == CSI)
          {
            if (ch == 0x20 || (st.prev == TIME && ch == 0x28))
              st.skip = 1;
            else if (!((st.prev == TIME && ch == 0x29)
                   || ch == 0x3b || (ch >= 0x30 && ch <= 0x39)))
              {
            st.mode = NORMAL;
            // if ignoring errors skip 1 bytes
            break;
              }
          }
        else if (st.prev == COL || st.prev == CDC)
          {
            if (ch == 0x20)
              st.skip = 1;
            else
              {
            st.mode = NORMAL;
            if (ch < 0x40 || ch > 0x7f)
              // if ignoring errors skip 1 bytes
              break;
              }
          }
        ++ inptr;
        continue;
          }
        if (ch == LF)
          {
        st = def_state_from;
        AV_WB32 (outptr, ch);
        outptr += 4;
        ++ inptr;
        continue;
          }
        if (st.mode == ESCAPE)
          {
        if (ch == LS2 || ch == LS3)
          {
            st.mode = NORMAL;
            st.gl = (ch == LS2) ? 2 : 3;
            st.ss = 0;
          }
        else if (ch == LS1R || ch == LS2R || ch == LS3R)
          {
            st.mode = NORMAL;
            st.gr = (ch == LS1R) ? 1 : (ch == LS2R) ? 2 : 3;
            st.ss = 0;
          }
        else if (ch == 0x24)
          st.mode = DESIGNATE_MB;
        else if (ch >= 0x28 && ch <= 0x2b)
          {
            st.mode = G_SEL_1B;
            st.gidx = ch - 0x28;
          }
        else
          {
            st.mode = NORMAL;
            // if ignoring errors skip 1 bytes
            break;
          }
        ++ inptr;
        continue;
          }
        if (st.mode == DESIGNATE_MB)
          {
        if (ch == KANJI_set || ch == JISX0213_1_set || ch == JISX0213_2_set
            || ch == EXTRA_SYMBOLS_set)
          {
            st.mode = NORMAL;
            st.g[0] = ch;
          }
        else if (ch >= 0x28 && ch <= 0x2b)
          {
          st.mode = G_SEL_MB;
          st.gidx = ch - 0x28;
          }
        else
          {
            st.mode = NORMAL;
            // if ignoring errors skip 1 bytes
            break;
          }
        ++ inptr;
        continue;
          }
        if (st.mode == G_SEL_1B)
          {
        if (ch == ASCII_set || ch == ASCII_x_set || ch == JIS0201_KATA_set
            || (ch >= 0x30 && ch <= 0x38))
          {
            st.g[st.gidx] = ch;
            st.mode = NORMAL;
          }
        else if (ch == 0x20)
            st.mode = DRCS_SEL_1B;
        else
          {
            st.mode = NORMAL;
            // if ignoring errors skip 1 bytes
            break;
          }
        ++ inptr;
        continue;
          }
        if (st.mode == G_SEL_MB)
          {
        if (ch == KANJI_set || ch == JISX0213_1_set || ch == JISX0213_2_set
            || ch == EXTRA_SYMBOLS_set)
          {
            st.g[st.gidx] = ch;
            st.mode = NORMAL;
          }
        else if (ch == 0x20)
          st.mode = DRCS_SEL_MB;
        else
          {
            st.mode = NORMAL;
            // if ignoring errors skip 1 bytes
            break;
          }
        ++ inptr;
        continue;
          }
        if (st.mode == DRCS_SEL_1B)
          {
        st.mode = NORMAL;
        if (ch == 0x70 || (ch >= 0x41 && ch <= 0x4f))
          st.g[st.gidx] = ch | 0x80;
        else
          // if ignoring errors skip 1 bytes
          break;
        ++ inptr;
        continue;
          }
        if (st.mode == DRCS_SEL_MB)
          {
        st.mode = NORMAL;
        if (ch == 0x40)
          st.g[st.gidx] = ch | 0x80;
        else
          // if ignoring errors skip 1 bytes
          break;
        ++ inptr;
        continue;
          }
        if (st.mode == MB_2ND)
          {
        int gidx;
        int i, len;
        uint32_t out[4 * 4];
        gidx = (st.ss) ? st.ss : (ch & 0x80) ? st.gr : st.gl;
        st.mode = NORMAL;
        st.ss = 0;
        if (!(ch & 0x60)) /* C0/C1 */
          // if ignoring errors skip 1 bytes
          break;
        if (st.ss > 0 && (ch & 0x80))
          // if ignoring errors skip 1 bytes
          break;
        if ((st.prev & 0x80) != (ch & 0x80))
          // if ignoring errors skip 1 bytes
          break;
        len = b24_char_conv(st.g[gidx], (st.prev & 0x7f), (ch & 0x7f), out);
        if (len == 0)
          // if ignoring errors skip 1 bytes
          break;
        if (outptr + 4 * len > outend)
          {
            result = AVERROR(ENOMEM);
            break;
          }
        for (i = 0; i < len; i++)
          {
            if (irreversible && out[i] == __UNKNOWN_10646_CHAR)
              ++ *irreversible;
            AV_WB32 (outptr, out[i]);
            outptr += 4;
          }
        ++ inptr;
        continue;
          }
        if (st.mode == NORMAL)
          {
        int gidx, set;
        if (!(ch & 0x60)) /* C0/C1 */
          {
            if (ch == ESC)
              st.mode = ESCAPE;
            else if (ch == SS2)
              st.ss = 2;
            else if (ch == SS3)
              st.ss = 3;
            else if (ch == LS0)
              {
            st.ss = 0;
            st.gl = 0;
              }
            else if (ch == LS1)
              {
            st.ss = 0;
            st.gl = 1;
              }
            else if (ch == BEL || ch == BS || ch == CR)
              {
            st.ss = 0;
            AV_WB32 (outptr, ch);
            outptr += 4;
              }
            else if (ch == 0x09 || ch == 0x0b || ch == 0x0c || ch == 0x18
                 || ch == 0x1e || ch == 0x1f || (ch >= 0x80 && ch <= 0x8a)
                 || ch == 0x99 || ch == 0x9a)
              {
            /* do nothing. just skip */
              }
            else if (ch == 0x16 || ch == 0x8b || ch == 0x91 || ch == 0x93
                 || ch == 0x94 || ch == 0x97 || ch == 0x98)
              {
            st.mode = CTRL_SEQ;
            st.skip = 1;
              }
            else if (ch == 0x1c)
              {
            st.mode = CTRL_SEQ;
            st.skip = 2;
              }
            else if (ch == COL || ch == CDC || ch == MACRO_CTRL
                 || ch == CSI ||ch == TIME)
              {
            st.mode = CTRL_SEQ;
            st.skip = 0;
            st.prev = ch;
              }
            else
              // if ignoring errors skip 1 bytes
              break;
            ++ inptr;
            continue;
          }
        if ((ch & 0x7f) == 0x20 || ch == 0x7f)
          {
            st.ss = 0;
            AV_WB32 (outptr, ch);
            outptr += 4;
            ++ inptr;
            continue;
          }
        if (ch == 0xff)
          {
            st.ss = 0;
            AV_WB32 (outptr, __UNKNOWN_10646_CHAR);
            if (irreversible)
              ++ *irreversible;
            outptr += 4;
            ++ inptr;
            continue;
          }
        if (st.ss > 0 && (ch & 0x80))
          // if ignoring errors skip 1 bytes
          break;
        gidx = (st.ss) ? st.ss : (ch & 0x80) ? st.gr : st.gl;
        set = st.g[gidx];
        if (set == DRCS0_set || set == KANJI_set || set == JISX0213_1_set
            || set == JISX0213_2_set || set == EXTRA_SYMBOLS_set)
          {
            st.mode = MB_2ND;
            st.prev = ch;
          }
        else
          {
            uint32_t out;
            st.ss = 0;
            if (b24_char_conv(set, (ch & 0x7f), 0, &out) == 0)
              // if ignoring errors skip 1 bytes
              break;
            if (out == __UNKNOWN_10646_CHAR && irreversible)
              ++ *irreversible;
            AV_WB32 (outptr, out);
            outptr += 4;
          }
        ++ inptr;
        continue;
          }
    }

    return result;
}