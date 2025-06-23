#include <cstddef>

/*
 * Copyright (c) 2025 Seiden Group
 *
 * SPDX-License-Identifier: ISC
 */

/* Conversion table generated mechanically by recode 3.7.14
   for sequence ANSI_X3.4-1968..IBM037 (reversible).  */

constexpr unsigned char const ANSI_X3_4_1968_IBM037[256] =
  {
      0,   1,   2,   3,  55,  45,  46,  47,     /*   0 -   7  */
     22,   5,  37,  11,  12,  13,  14,  15,     /*   8 -  15  */
     16,  17,  18,  19,  60,  61,  50,  38,     /*  16 -  23  */
     24,  25,  63,  39,  28,  29,  30,  31,     /*  24 -  31  */
     64,  90, 127, 123,  91, 108,  80, 125,     /*  32 -  39  */
     77,  93,  92,  78, 107,  96,  75,  97,     /*  40 -  47  */
    240, 241, 242, 243, 244, 245, 246, 247,     /*  48 -  55  */
    248, 249, 122,  94,  76, 126, 110, 111,     /*  56 -  63  */
    124, 193, 194, 195, 196, 197, 198, 199,     /*  64 -  71  */
    200, 201, 209, 210, 211, 212, 213, 214,     /*  72 -  79  */
    215, 216, 217, 226, 227, 228, 229, 230,     /*  80 -  87  */
    231, 232, 233, 186, 224, 187, 176, 109,     /*  88 -  95  */
    121, 129, 130, 131, 132, 133, 134, 135,     /*  96 - 103  */
    136, 137, 145, 146, 147, 148, 149, 150,     /* 104 - 111  */
    151, 152, 153, 162, 163, 164, 165, 166,     /* 112 - 119  */
    167, 168, 169, 192,  79, 208, 161,   7,     /* 120 - 127  */
    128,  34,  98,  99, 100, 101, 102, 103,     /* 128 - 135  */
    104, 105, 138, 139, 140, 141, 142, 143,     /* 136 - 143  */
    144, 106,  44,  10,  95,  62,  26, 112,     /* 144 - 151  */
    113, 114, 154, 155, 156, 157, 158, 159,     /* 152 - 159  */
    160,  21, 115, 116, 117, 118, 119, 120,     /* 160 - 167  */
      9,  58, 170, 171, 172, 173, 174, 175,     /* 168 - 175  */
     59, 177, 178, 179, 180, 181, 182, 183,     /* 176 - 183  */
    184, 185,  36,  41, 188, 189, 190, 191,     /* 184 - 191  */
     35,  65,  66,  67,  68,  69,  70,  71,     /* 192 - 199  */
     72,  73, 202, 203, 204, 205, 206, 207,     /* 200 - 207  */
     27,  74,   6,  20,  40,  43,  32,  23,     /* 208 - 215  */
     81,  82, 218, 219, 220, 221, 222, 223,     /* 216 - 223  */
     42, 225,  83,  84,  85,  86,  87,  88,     /* 224 - 231  */
     89,  33, 234, 235, 236, 237, 238, 239,     /* 232 - 239  */
     48,  49,   8,  51,  52,  53,  54,   4,     /* 240 - 247  */
     56,  57, 250, 251, 252, 253, 254, 255,     /* 248 - 255  */
  };

static constexpr char a2e(char a)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wchar-subscripts"
	return ANSI_X3_4_1968_IBM037[a];
#pragma GCC diagnostic pop
}

static constexpr char operator ""_e(char a)
{
	return a2e(a);
}

/**
 * Wrapper for a fixed-length EBCDIC string that gets converted at compile
 * time. The length is decided in the template. The string itself is padded
 * with EBCDIC spaces (0x40, ASCII '@') if it doesn't fit into the buffer.
 *
 * The annoying part is C++14 means you must access the string buffer buffer
 * via the .value member. C++17 has better constexpr support thag woud allow
 * returning a string at compile time, even via operator_"", but we still
 * want to support IBM i 7.2 w/ gcc 6.
 */
template<size_t Len>
struct EF {
    constexpr EF(const char *a) : value() {
        bool pad = false;
        for (size_t i = 0; i < Len; i++) {
            if (pad || a[i] == '\0') {
                this->value[i] = ' '_e;
                pad = true;
            } else {
                this->value[i] = a2e(a[i]);
            }
        }
    }
    char value[Len];
};
