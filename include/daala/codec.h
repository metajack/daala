/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*Daala video codec
Copyright (c) 2006-2013 Daala project contributors.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

/**\mainpage
 *
 * \section intro Introduction
 *
 * This is the documentation of the <tt>libdaala</tt> C API.
 * <tt>libdaala</tt> is the reference implementation for Daala, a free video
 *  codec.
 *
 * \subsection Organization
 * <tt>libdaala</tt> is actually subdivided into three separate libraries.
 * - <tt>libdaalaenc<tt>, containing routines exclusive to the encoder.
 *   You must link to this if you use any of the functions listed in
 *    \ref encfuncs.
 * - <tt>libdaaladec</tt>, containing routines exclusive to the decoder.
 *   You must link to this if you use any of the functions listed in
 *    \ref decfuncs.
 * - <tt>libdaalabase</tt>, containing routines shared by the encoder and the
 *    decoder.
 *   You must link to this if you use any of the functions in this API, not
 *    just those listed in \ref basefuncs.*/

/**\file
 * The shared <tt>libdaala<tt/> C API.*/
#if !defined(_daala_codec_H)
# define _daala_codec_H (1)
/*Pick up typedefs.*/
# include <ogg/ogg.h>

# if defined(__cplusplus)
extern "C" {
# endif

# if defined(__GNUC__) && defined(__GNUC_MINOR__)
#  define OD_GNUC_PREREQ(maj, min) \
  ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
# else
#  define OD_GNUC_PREREQ(maj, min) (0)
# endif

#if OD_GNUC_PREREQ(4, 0)
# pragma GCC visibility push(default)
#endif

#if OD_GNUC_PREREQ(3, 4)
# define OD_WARN_UNUSED_RESULT __attribute__((__warn_unused_result__))
#else
# define OD_WARN_UNUSED_RESULT
#endif

#if OD_GNUC_PREREQ(3, 4)
# define OD_ARG_NONNULL(x) __attribute__((__nonnull__(x)))
#else
# define OD_ARG_NONNULL(x)
#endif

/*TODO: remove this ugliness*/
# if defined(_MSC_VER)
#  pragma warning(disable:4100 4115 4125 4127 4152 4505 4554 4711)
# endif

/**\name Error codes*/
/*@{*/
/**An invalid pointer was provided.*/
# define OD_EFAULT (-1)
/**An invalid argument was provided.*/
# define OD_EINVAL (-10)
/**The contents of the header were incomplete, invalid, or unexpected.*/
# define OD_EBADHEADER (-20)
/**The header does not belong to a Daala stream.*/
# define OD_ENOTFORMAT (-21)
/**The bitstream version is too high.*/
# define OD_EVERSION (-22)
/**The specified function is not implemented.*/
# define OD_EIMPL (-23)
/**There were errors in the video data packet.*/
# define OD_EBADPACKET (-24)
/*@}*/

/**\name Colorspaces
 * The currently defined color space tags.*/
/*@{*/
/**The color space was not specified at the encoder.
 * It may be conveyed by an external means.*/
# define OD_CS_UNSPECIFIED (0)
/**A Y'CbCr color space designed for NTSC content*/
# define OD_CS_ITU_REC_470M (1)
/**A Y'CbCr color space designed for PAL/SECAM content.*/
# define OD_CS_ITU_REC_470BG (2)
/**A Y'CbCr color space designed for HD content.*/
# define OD_CS_ITU_REC_790 (3)
/**A Y'CgCo color space designed for sRGB content.*/
# define OD_CS_YCgCo (4)
/**The total number of currently defined color spaces.*/
# define OD_CS_NSPACES (5)
/*@}*/

/**The maximum number of color planes allowed in a single frame.*/
# define OD_NPLANES_MAX (4)

typedef struct od_img_plane od_img_plane;
typedef struct od_img od_img;
typedef struct daala_plane_info daala_plane_info;
typedef struct daala_info daala_info;
typedef struct daala_comment daala_comment;

const char *daala_version_string(void);

struct od_img_plane {
  unsigned char *data;
  /** The decimation factor in x direction. Pixesl are reduced by a factor of
      2^xdec so  0 is none, 1 is decimated by a factor of 2. ( YUV420 will
      have xdec of 1 and ydec also of 1. YUV444 will have xdec and ydec set to
      zero ). */
  unsigned char xdec;
  unsigned char ydec;
  /** Distance in memory between two pixels horizontally next to each other in
      (is always 1 in encoder). */
  int xstride;
  /** Distance in memory between two pixels vertically next to each other. */
  int ystride;
};

struct od_img {
  /** Typical 3 planes for Y, Cb, and  Cr. Can have a 4th plane for alpha */
  od_img_plane planes[OD_NPLANES_MAX];
  /** Number of planes (1 for greyscale, 3 for YCbCr, 4 for YCbCr+Alpha ) */
  int nplanes;
  /** Width and height in pixels */
  ogg_int32_t width;
  ogg_int32_t height;
};

struct daala_plane_info {
  unsigned char xdec;
  unsigned char ydec;
};

struct daala_info {
  unsigned char version_major;
  unsigned char version_minor;
  unsigned char version_sub;
  /** pic_width,_height form a region of interest to encode */
  ogg_int32_t pic_width;
  ogg_int32_t pic_height;
  ogg_uint32_t pixel_aspect_numerator;
  ogg_uint32_t pixel_aspect_denominator;
  ogg_uint32_t timebase_numerator;
  ogg_uint32_t timebase_denominator;
  ogg_uint32_t frame_duration;
  int keyframe_granule_shift;
  int nplanes;
  daala_plane_info plane_info[OD_NPLANES_MAX];
   /** key frame rate defined how often a key frame is emitted by encoder in
    * number of frames. So 10 means every 10th frame is a keyframe.  */ 
  int keyframe_rate;
};

void daala_info_init(daala_info *info);
void daala_info_clear(daala_info *info);

/**The comment information.
 *
 * This structure holds in the in-stream metadata corresponding to the
 *  'comment' header packet.
 * The comment header is meant to be used much like someone jotting a quick
 *  note on the label of a disc.
 * It should be a short, to the point text note that can be more than a couple
 *  words, but not more than a short paragraph.
 *
 * The metadata is stored as a series of (tag, value) pairs, in length-encoded
 *  string vectors.
 * The first occurrence of the '=' character delimits the tag and value.
 * A particular tag may occur more than once, and order is significant.
 * The character set encoding for the strings is always UTF-8, but the tag
 *  names are limited to ASCII, and treated as case-insensitive.
 * See the Daala specification for details.
 *
 * In filling in this structure, daala_decode_header() will null-terminate the
 *  user_comment strings for safety.
 * However, the bitstream format itself treats them as 8-bit clean vectors,
 *  possibly containing null characters, and so the length array should be
 *  treated as their authoritative length.*/
struct daala_comment {
  /**The array of comment string vectors.*/
  char **user_comments;
  /**An array of the corresponding lengths of each vector, in bytes.*/
  int *comment_lengths;
  /**The total number of comment strings.*/
  int comments;
  /**The null-terminated vendor string.
     This identifies the software used to encode the stream.*/
  char *vendor;
};

void daala_comment_init(daala_comment *dc);
void daala_comment_clear(daala_comment *dc);

ogg_int64_t daala_granule_basetime(void *encdec, ogg_int64_t granpos);
double daala_granule_time(void *encdec, ogg_int64_t granpos);

# if OD_GNUC_PREREQ(4, 0)
#  pragma GCC visibility pop
# endif
# if defined(__cplusplus)
}
# endif

#endif
