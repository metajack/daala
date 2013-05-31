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

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "encint.h"
#include "generic_code.h"
#include "filter.h"
#include "dct.h"
#include "intra.h"
#include "logging.h"
#include "pvq.h"
#include "pvq_code.h"
#include "block_size.h"
#include "block_size_enc.h"
#include "logging.h"
#if OD_DECODE_IN_ENCODE
# include "decint.h"
#endif

static double mode_bits = 0;
static double mode_count = 0;

static int od_enc_init(od_enc_ctx *enc, const daala_info *info) {
  int ret;
  ret = od_state_init(&enc->state, info);
  if (ret < 0) return ret;
  oggbyte_writeinit(&enc->obb);
  od_ec_enc_init(&enc->ec, 65025);
  enc->packet_state = OD_PACKET_INFO_HDR;
  enc->mvest = od_mv_est_alloc(enc);
  return 0;
}

static void od_enc_clear(od_enc_ctx *enc) {
  od_mv_est_free(enc->mvest);
  od_ec_enc_clear(&enc->ec);
  oggbyte_writeclear(&enc->obb);
  od_state_clear(&enc->state);
}

daala_enc_ctx *daala_encode_create(const daala_info *info) {
  od_enc_ctx *enc;
  if (info == NULL) return NULL;
  enc = (od_enc_ctx *)_ogg_malloc(sizeof(*enc));
  if (od_enc_init(enc,info) < 0) {
    _ogg_free(enc);
    return NULL;
  }
  return enc;
}

void daala_encode_free(daala_enc_ctx *enc) {
  if (enc != NULL) {
    od_enc_clear(enc);
    _ogg_free(enc);
  }
}

int daala_encode_ctl(daala_enc_ctx *enc, int req, void *buf,
 size_t buf_sz) {
  (void)enc;
  (void)buf;
  (void)buf_sz;
  switch (req) {
    default:return OD_EIMPL;
  }
}

void od_state_mc_predict(od_state *state, int ref) {
  unsigned char  __attribute__((aligned(16))) buf[16][16];
  od_img *img;
  int nhmvbs;
  int nvmvbs;
  int pli;
  int vx;
  int vy;
  nhmvbs = (state->nhmbs + 1) << 2;
  nvmvbs = (state->nvmbs + 1) << 2;
  img = state->io_imgs + OD_FRAME_REC;
  for (vy = 0; vy < nvmvbs; vy += 4) {
    for (vx = 0; vx < nhmvbs; vx += 4) {
      for (pli = 0; pli < img->nplanes; pli++) {
        od_img_plane *iplane;
        unsigned char *p;
        int blk_w;
        int blk_h;
        int blk_x;
        int blk_y;
        int y;
        od_state_pred_block(state, buf[0], sizeof(buf[0]), ref, pli, vx, vy,
         2);
        /*Copy the predictor into the image, with clipping.*/
        iplane = img->planes + pli;
        blk_w = 16 >> iplane->xdec;
        blk_h = 16 >> iplane->ydec;
        blk_x = (vx - 2) << (2 - iplane->xdec);
        blk_y = (vy - 2) << (2 - iplane->ydec);
        p = buf[0];
        if (blk_x < 0) {
          blk_w += blk_x;
          p -= blk_x;
          blk_x = 0;
        }
        if (blk_y < 0) {
          blk_h += blk_y;
          p -= blk_y*sizeof(buf[0]);
          blk_y = 0;
        }
        if (blk_x + blk_w > img->width >> iplane->xdec) {
          blk_w = (img->width >> iplane->xdec) - blk_x;
        }
        if (blk_y + blk_h > img->height >> iplane->ydec) {
          blk_h = (img->height >> iplane->ydec) - blk_y;
        }
        for (y = blk_y; y < blk_y + blk_h; y++) {
          memcpy(iplane->data + y*iplane->ystride + blk_x,
           p, blk_w);
          p += sizeof(buf[0]);
        }
      }
    }
  }
}

static void od_img_plane_copy_pad8(od_img_plane *dst_p,
 int plane_width, int plane_height, od_img_plane *src_p,
 int pic_width, int pic_height) {
  unsigned char *dst_data;
  ptrdiff_t dstride;
  int y;
  dstride = dst_p->ystride;
  /*If we have _no_ data, just encode a dull green.*/
  if (pic_width == 0 || pic_height == 0) {
    dst_data = dst_p->data;
    for (y = 0; y < plane_height; y++) {
      memset(dst_data, 0, plane_width*sizeof(*dst_data));
      dst_data += dstride;
    }
  }
  /*Otherwise, copy what we do have, and add our own padding.*/
  else{
    unsigned char *src_data;
    unsigned char *dst;
    ptrdiff_t sxstride;
    ptrdiff_t systride;
    int x;
    /*Step 1: Copy the data we do have.*/
    sxstride = src_p->xstride;
    systride = src_p->ystride;
    dst_data = dst_p->data;
    src_data = src_p->data;
    dst = dst_data;
    for (y = 0; y < pic_height; y++) {
      if (sxstride == 1) memcpy(dst, src_data, pic_width);
      else for (x = 0; x < pic_width; x++) dst[x] = *(src_data + sxstride*x);
      dst += dstride;
      src_data += systride;
    }
    /*Step 2: Perform a low-pass extension into the padding region.*/
    /*Right side.*/
    for (x = pic_width; x < plane_width; x++) {
      dst = dst_data + x - 1;
      for (y = 0; y < pic_height; y++) {
        dst[1] = 2*dst[0] + (dst - (dstride & -(y > 0)))[0]
         + (dst + (dstride & -(y + 1 < pic_height)))[0] + 2 >> 2;
        dst += dstride;
      }
    }
    /*Bottom.*/
    dst = dst_data + dstride*pic_height;
    for (y = pic_height; y < plane_height; y++) {
      for (x = 0; x < plane_width; x++) {
        dst[x] = 2*(dst - dstride)[x] + (dst - dstride)[x - (x > 0)]
         + (dst - dstride)[x + (x + 1 < plane_width)] + 2 >> 2;
      }
      dst += dstride;
    }
  }
}

/*Extend the edge into the padding.*/
static void od_img_plane_edge_ext8(od_img_plane *dst_p,
 int plane_width, int plane_height, int horz_padding, int vert_padding) {
  ptrdiff_t dstride;
  unsigned char *dst_data;
  unsigned char *dst;
  int x;
  int y;
  dstride = dst_p->ystride;
  dst_data = dst_p->data;
  /*Left side.*/
  for (y = 0; y < plane_height; y++) {
    dst = dst_data + dstride * y;
    for (x = 1; x <= horz_padding; x++) {
      (dst-x)[0] = dst[0];
    }
  }
  /*Right side.*/
  for (y = 0; y < plane_height; y++) {
    dst = dst_data + plane_width - 1 + dstride * y;
    for (x = 1; x <= horz_padding; x++) {
      dst[x] = dst[0];
    }
  }
  /*Top.*/
  dst = dst_data - horz_padding;
  for (y = 0; y < vert_padding; y++) {
    for (x = 0; x < plane_width + 2 * horz_padding; x++) {
      (dst - dstride)[x] = dst[x];
    }
    dst -= dstride;
  }
  /*Bottom.*/
  dst = dst_data - horz_padding + plane_height * dstride;
  for (y = 0; y < vert_padding; y++) {
    for (x = 0; x < plane_width + 2 * horz_padding; x++) {
      dst[x] = (dst - dstride)[x];
    }
    dst += dstride;
  }
}


struct od_mb_enc_ctx {
  GenericEncoder model_dc[OD_NPLANES_MAX];
  GenericEncoder model_g[OD_NPLANES_MAX];
  GenericEncoder model_ym[OD_NPLANES_MAX];
  od_adapt_ctx adapt;
  signed char *modes;
  od_coeff *c;
  od_coeff *d;
  od_coeff *md;
  od_coeff *mc;
  od_coeff *l;
  int ex_dc[OD_NPLANES_MAX];
  int ex_g[OD_NPLANES_MAX];
  int is_keyframe;
  int nk;
  int k_total;
  int sum_ex_total_q8;
  int ncount;
  int count_total_q8;
  int count_ex_total_q8;
  ogg_uint16_t mode_p0[OD_INTRA_NMODES];
};
typedef struct od_mb_enc_ctx od_mb_enc_ctx;

void od_mb_encode(daala_enc_ctx *enc, od_mb_enc_ctx *ctx, int scale, int pli,
 int mbx, int mby) {
  int by;
  int bx;
  int xdec;
  int ydec;
  int w;
  int frame_width;
  signed char *modes;
  od_coeff *c;
  od_coeff *d;
  od_coeff *md;
  od_coeff *mc;
  od_coeff *l;
  xdec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].xdec;
  ydec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ydec;
  frame_width = enc->state.frame_width;
  w = frame_width >> xdec;
  modes = ctx->modes;
  c = ctx->c;
  d = ctx->d;
  md = ctx->md;
  mc = ctx->mc;
  l = ctx->l;
  for (by = mby << (2 - ydec); by < (mby + 1) << (2 - ydec); by++) {
    for (bx = mbx << (2 - xdec); bx < (mbx + 1) << (2 - xdec); bx++) {
      int x;
      int y;
      od_coeff pred[4*4];
      od_coeff predt[4*4];
      ogg_int16_t pvq_scale[4*4];
      int sgn;
      int qg;
      int cblock[4*4];
      int zzi;
      int vk;
#ifdef OD_LOLOSSLESS
      od_coeff backup[4*4];
#endif
      vk = 0;
      /*fDCT a 4x4 block.*/
      od_bin_fdct4x4(d + (by << 2)*w + (bx << 2), w,
       c + (by << 2)*w + (bx << 2), w);
      if (!ctx->is_keyframe) {
        od_bin_fdct4x4(md + (by << 2)*w + (bx << 2), w,
         mc + (by << 2)*w + (bx << 2), w);
      }
      for (zzi = 0; zzi < 16; zzi++) pvq_scale[zzi] = 0;
      if (bx > 0 && by > 0) {
        if (pli == 0) {
          ogg_uint16_t mode_cdf[OD_INTRA_NMODES];
          ogg_uint32_t mode_dist[OD_INTRA_NMODES];
          int m_l;
          int m_ul;
          int m_u;
          int mode;
          od_coeff *ur;
          ur = (by > 0 && (((bx + 1) < (mbx + 1) << (2 - xdec))
           || (by == mby << (2 - ydec)))) ?
           d + ((by - 1) << 2)*w + ((bx + 1) << 2) :
           d + ((by - 1) << 2)*w + (bx << 2);
          m_l = modes[by*(w >> 2) + bx - 1];
          m_ul = modes[(by - 1)*(w >> 2) + bx - 1];
          m_u = modes[(by - 1)*(w >> 2) + bx];
          od_intra_pred_cdf(mode_cdf, OD_INTRA_PRED_PROB_4x4[pli],
           ctx->mode_p0, OD_INTRA_NMODES, m_l, m_ul, m_u);
          od_intra_pred4x4_dist(mode_dist, d + (by << 2)*w + (bx << 2),
           w, ur, w, pli);
          /*Lambda = 1*/
          mode = od_intra_pred_search(mode_cdf, mode_dist,
           OD_INTRA_NMODES, 128);
          od_intra_pred4x4_get(pred, d + (by << 2)*w + (bx << 2), w,
           ur, w, mode);
          od_ec_encode_cdf_unscaled(&enc->ec, mode, mode_cdf, OD_INTRA_NMODES);
          mode_bits -= M_LOG2E*log(
           (mode_cdf[mode] - (mode == 0 ? 0 : mode_cdf[mode - 1]))/
           (float)mode_cdf[OD_INTRA_NMODES - 1]);
          mode_count++;
          modes[by*(w >> 2) + bx] = mode;
          od_intra_pred_update(ctx->mode_p0, OD_INTRA_NMODES, mode, m_l, m_ul,
           m_u);
        }
        else {
          int chroma_weights_q8[3];
          int mode;
          mode = modes[(by << ydec)*(frame_width >> 2) + (bx << xdec)];
          chroma_weights_q8[0] = OD_INTRA_CHROMA_WEIGHTS_Q6[mode][0];
          chroma_weights_q8[1] = OD_INTRA_CHROMA_WEIGHTS_Q6[mode][1];
          chroma_weights_q8[2] = OD_INTRA_CHROMA_WEIGHTS_Q6[mode][2];
          mode = modes[(by << ydec)*(frame_width >> 2) + ((bx << xdec)
           + xdec)];
          chroma_weights_q8[0] += OD_INTRA_CHROMA_WEIGHTS_Q6[mode][0];
          chroma_weights_q8[1] += OD_INTRA_CHROMA_WEIGHTS_Q6[mode][1];
          chroma_weights_q8[2] += OD_INTRA_CHROMA_WEIGHTS_Q6[mode][2];
          mode = modes[((by << ydec) + ydec)*(frame_width >> 2)
           + (bx << xdec)];
          chroma_weights_q8[0] += OD_INTRA_CHROMA_WEIGHTS_Q6[mode][0];
          chroma_weights_q8[1] += OD_INTRA_CHROMA_WEIGHTS_Q6[mode][1];
          chroma_weights_q8[2] += OD_INTRA_CHROMA_WEIGHTS_Q6[mode][2];
          mode = modes[((by << ydec) + ydec)*(frame_width >> 2)
           + ((bx << xdec) + xdec)];
          chroma_weights_q8[0] += OD_INTRA_CHROMA_WEIGHTS_Q6[mode][0];
          chroma_weights_q8[1] += OD_INTRA_CHROMA_WEIGHTS_Q6[mode][1];
          chroma_weights_q8[2] += OD_INTRA_CHROMA_WEIGHTS_Q6[mode][2];
          od_chroma_pred4x4(pred, d + (by << 2)*w + (bx << 2),
           l + (by << 2)*w + (bx << 2), w, chroma_weights_q8);
        }
      }
      else{
        for (zzi = 0; zzi < 16; zzi++) pred[zzi] = 0;
        if (bx > 0) pred[0] = d[(by << 2)*w + ((bx - 1) << 2)];
        else if (by > 0) pred[0] = d[((by - 1) << 2)*w + (bx << 2)];
        if (pli == 0) modes[by*(w >> 2) + bx] = 0;
      }
      if (!ctx->is_keyframe) {
        int x;
        int y;
        int i;
        i = 0;
        for( y=0; y<4; y++ ) {
          for( x=0; x<4; x++ ) {
            pred[i++] = md[(y + (by << 2))*w + (x + (bx << 2))];
          }

        }
      }
      /*Zig-zag*/
      for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
          cblock[OD_ZIG4[y*4 + x]] = d[((by << 2) + y)*w + (bx << 2) + x];
          predt[OD_ZIG4[y*4 + x]] = pred[y*4 + x];
        }
      }
#ifdef OD_LOLOSSLESS
      for (zzi = 0; zzi < 16; zzi++) {
        backup[zzi] = cblock[zzi] + 32768;
        OD_ASSERT(backup[zzi] >= 0);
        OD_ASSERT(backup[zzi] < 65535);
        od_ec_enc_uint(&enc->ec, backup[zzi], 65536);
      }
#endif
      sgn = (cblock[0] - predt[0]) < 0;
      cblock[0] = (int)floor(pow(fabs(cblock[0] - predt[0])/scale,0.75));
      generic_encode(&enc->ec, ctx->model_dc + pli, cblock[0],
       ctx->ex_dc + pli, 0);
      if (cblock[0]) od_ec_enc_bits(&enc->ec, sgn, 1);
      cblock[0] = (int)(pow(cblock[0], 4.0/3)*scale);
      cblock[0] *= sgn ? -1 : 1;
      cblock[0] += predt[0];
      quant_pvq(cblock + 1, predt + 1, pvq_scale, pred + 1, 15, scale, &qg);
      for (zzi = 1; zzi < 16; zzi++) cblock[zzi] = pred[zzi];
      dequant_pvq(cblock + 1, predt + 1, pvq_scale, 15, scale, qg);
      generic_encode(&enc->ec, ctx->model_g + pli, abs(qg),
       ctx->ex_g + pli, 0);
      if (qg) od_ec_enc_bits(&enc->ec, qg < 0, 1);
      vk = 0;
      for (zzi = 0; zzi < 15; zzi++) vk += abs(pred[zzi + 1]);
      /*No need to code vk because we can get it from qg.*/
      /*Expectation is that half the pulses will go in y[m].*/
      if (vk != 0) {
        int ex_ym;
        ex_ym = (65536/2)*vk;
        generic_encode(&enc->ec, &ctx->model_ym[pli], vk - pred[1], &ex_ym, 0);
      }
      pvq_encoder(&enc->ec, pred + 2, 14, vk - abs(pred[1]), &ctx->adapt);
      if (ctx->adapt.curr[OD_ADAPT_K_Q8] >= 0) {
        ctx->nk++;
        ctx->k_total += ctx->adapt.curr[OD_ADAPT_K_Q8];
        ctx->sum_ex_total_q8 += ctx->adapt.curr[OD_ADAPT_SUM_EX_Q8];
      }
      if (ctx->adapt.curr[OD_ADAPT_COUNT_Q8] >= 0) {
        ctx->ncount++;
        ctx->count_total_q8 += ctx->adapt.curr[OD_ADAPT_COUNT_Q8];
        ctx->count_ex_total_q8 += ctx->adapt.curr[OD_ADAPT_COUNT_EX_Q8];
      }
#ifdef OD_LOLOSSLESS
      for (zzi = 0; zzi < 16; zzi++) {
        cblock[zzi] = backup[zzi] - 32768;
      }
#endif
      /*Dequantize*/
      for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
          d[((by << 2) + y)*w + (bx << 2) + x] = cblock[OD_ZIG4[y*4 + x]];
        }
      }
      /*iDCT the 4x4 block.*/
      od_bin_idct4x4(c + (by << 2)*w + (bx << 2), w, d + (by << 2)*w
       + (bx << 2), w);
    }
  }
}

int daala_encode_img_in(daala_enc_ctx *enc, od_img *img, int duration) {
  int refi;
  int nplanes;
  int pli;
  int scale;
  int frame_width;
  int frame_height;
  int pic_width;
  int pic_height;
  int i;
  int j;
  int k;
  int m;
  BlockSizeComp *bs;
  int nhsb;
  int nvsb;
  if (enc == NULL || img == NULL) return OD_EFAULT;
  if (enc->packet_state == OD_PACKET_DONE) return OD_EINVAL;
  /*Check the input image dimensions to make sure they're compatible with the
     declared video size.*/
  nplanes = enc->state.info.nplanes;
  if (img->nplanes != nplanes) return OD_EINVAL;
  for (pli = 0; pli < nplanes; pli++) {
    if (img->planes[pli].xdec != enc->state.info.plane_info[pli].xdec
      || img->planes[pli].ydec != enc->state.info.plane_info[pli].ydec) {
      return OD_EINVAL;
    }
  }
  frame_width = enc->state.frame_width;
  frame_height = enc->state.frame_height;
  pic_width = enc->state.info.pic_width;
  pic_height = enc->state.info.pic_height;
  nhsb = enc->state.nhsb;
  nvsb = enc->state.nvsb;
  if (img->width != frame_width || img->height != frame_height) {
    /*The buffer does not match the frame size.
      Check to see if it matches the picture size.*/
    if ( img->width != pic_width || img->height != pic_height) {
      /*It doesn't; we don't know how to handle it yet.*/
      return OD_EINVAL;
    }
  }
  /*Copy and pad the image.*/
  for (pli = 0; pli < nplanes; pli++) {
    od_img_plane plane;
    int plane_width;
    int plane_height;
    *&plane = *(img->planes + pli);
    plane_width = ((pic_width + (1 << plane.xdec) - 1) >> plane.xdec);
    plane_height = ((pic_height + (1 << plane.ydec) - 1) >>
     plane.ydec);
    od_img_plane_copy_pad8(&enc->state.io_imgs[OD_FRAME_INPUT].planes[pli],
     frame_width >> plane.xdec, frame_height >> plane.ydec,
     &plane, plane_width, plane_height);
    od_img_plane_edge_ext8(enc->state.io_imgs[OD_FRAME_INPUT].planes + pli,
     frame_width >> plane.xdec, frame_height >> plane.ydec,
     OD_UMV_PADDING >> plane.xdec, OD_UMV_PADDING >> plane.ydec);
  }
#if defined(OD_DUMP_IMAGES)
  if (od_logging_active(OD_LOG_GENERIC, OD_LOG_DEBUG)) {
    daala_info *info;
    od_img img;
    info=&enc->state.info;
    /*Modify the image offsets to include the padding.*/
    *&img=*(enc->state.io_imgs+OD_FRAME_INPUT);
    for (pli = 0; pli < nplanes; pli++) {
      img.planes[pli].data -= (OD_UMV_PADDING>>info->plane_info[pli].xdec)
       +img.planes[pli].ystride*(OD_UMV_PADDING>>info->plane_info[pli].ydec);
    }
    img.width+=OD_UMV_PADDING<<1;
    img.height+=OD_UMV_PADDING<<1;
    od_state_dump_img(&enc->state, &img, "pad");
  }
#endif
  /*Initialize the entropy coder.*/
  od_ec_enc_reset(&enc->ec);
  /*Write a bit to mark this as a data packet.*/
  od_ec_encode_bool_q15(&enc->ec,0,16384);
  /*set the top row and the left most column to three*/
  for(i = -4; i < nhsb*4; i++) {
    for(j = -4; j < 0; j++) {
      enc->state.bsize[(j*enc->state.bstride) + i] = 3;
    }
  }
  for(j = -4; j < nvsb*4; j++) {
    for(i = -4; i < 0; i++) {
      enc->state.bsize[(j*enc->state.bstride) + i] = 3;
    }
  }
  /* Allocate a blockSizeComp for scratch space and then calculate the block sizes
     eventually store them in bsize. */
  bs = _ogg_malloc(sizeof(BlockSizeComp));
  od_log_matrix_uchar(OD_LOG_GENERIC, OD_LOG_INFO, "bimg ", enc->state.io_imgs[OD_FRAME_INPUT].planes[0].data-16*enc->state.io_imgs[OD_FRAME_INPUT].planes[0].ystride-16,
      enc->state.io_imgs[OD_FRAME_INPUT].planes[0].ystride, (nvsb + 1)*32);
  for(i = 0; i < nvsb; i++) {
    unsigned char *img;
    int istride ;
    int bstride;
    bstride = enc->state.bstride;
    img = enc->state.io_imgs[OD_FRAME_INPUT].planes[0].data;
    istride = enc->state.io_imgs[OD_FRAME_INPUT].planes[0].ystride;
    for(j = 0; j < nhsb; j++) {
      int bsize[4][4];
      char *state_bsize;
      state_bsize = &enc->state.bsize[i*4*enc->state.bstride + j*4];
      process_block_size32(bs, img, img + i*istride*32 + j*32, istride, bsize);
      /* Grab the 4x4 information returned from process_block_size32 in bsize
         and store it in the od_state bsize. */
      for(k = 0; k < 4; k++) {
        for(m = 0; m < 4; m++) {
          state_bsize[k*bstride + m] = bsize[k][m];
        }
      }
      od_block_size_encode(&enc->ec, &state_bsize[0], bstride);
    }
  }
  od_log_matrix_char(OD_LOG_GENERIC, OD_LOG_INFO, "bsize ", enc->state.bsize, enc->state.bstride, (nvsb+1)*4);
  for(i = 0; i < nvsb*4; i++) {
    for(j = 0; j < nhsb*4; j++) {
      OD_LOG_PARTIAL((OD_LOG_GENERIC, OD_LOG_INFO, "%d ", enc->state.bsize[i*enc->state.bstride + j]));
    }
    OD_LOG_PARTIAL((OD_LOG_GENERIC, OD_LOG_INFO, "\n"));
  }
  _ogg_free(bs);
  /*Update the buffer state.*/
  if (enc->state.ref_imgi[OD_FRAME_SELF] >= 0) {
    enc->state.ref_imgi[OD_FRAME_PREV] =
     enc->state.ref_imgi[OD_FRAME_SELF];
    /*TODO: Update golden frame.*/
    if (enc->state.ref_imgi[OD_FRAME_GOLD] < 0) {
      enc->state.ref_imgi[OD_FRAME_GOLD] =
       enc->state.ref_imgi[OD_FRAME_SELF];
      /*TODO: Mark keyframe timebase.*/
    }
  }
  /*Select a free buffer to use for this reference frame.*/
  for (refi = 0; refi == enc->state.ref_imgi[OD_FRAME_GOLD]
   || refi == enc->state.ref_imgi[OD_FRAME_PREV]
   || refi == enc->state.ref_imgi[OD_FRAME_NEXT]; refi++);
  enc->state.ref_imgi[OD_FRAME_SELF] = refi;
  memcpy(&enc->state.input, img, sizeof(enc->state.input));
  /*TODO: Incrment frame count.*/
  if (0 && enc->state.ref_imgi[OD_FRAME_PREV] >= 0 /*
   && daala_granule_basetime(enc, enc->state.cur_time) >= 19*/) {
#if defined(OD_DUMP_IMAGES) && defined(OD_ANIMATE)
    enc->state.ani_iter = 0;
#endif
    OD_LOG((OD_LOG_ENCODER, OD_LOG_INFO, "Predicting frame %i:",
            (int)daala_granule_basetime(enc, enc->state.cur_time)));
#if 1
    od_mv_est(enc->mvest, OD_FRAME_PREV, 452/*118*/);
    /* output the motion vectors */
    {
      int nhmvbs;
      int nvmvbs;
      int vx;
      int vy;
      od_img *img;
      int width;
      int height;
      od_mv_grid_pt* mvp;
      nhmvbs = (enc->state.nhmbs + 1) << 2;
      nvmvbs = (enc->state.nvmbs + 1) << 2;
      img = enc->state.io_imgs + OD_FRAME_REC;
      width = img->width;
      height = img->height;
      for (vy = 0; vy < nvmvbs; vy += 4) {
        for (vx = 0; vx < nhmvbs; vx += 4) {
          mvp = &( enc->state.mv_grid[vy][vx] );
          /* TODO - need to tune probability distribution on next line */
          od_ec_encode_bool_q15( &enc->ec , mvp->valid, 32000 );
          if ( mvp->valid )
          {
            OD_ASSERT( mvp->mv[0] <  8*(width+32) );
            OD_ASSERT( mvp->mv[0] > -8*(width+32) );
            OD_ASSERT( mvp->mv[1] <  8*(height+32) );
            OD_ASSERT( mvp->mv[1] > -8*(height+32) );
            od_ec_enc_uint( &enc->ec , mvp->mv[0] + 8*(width+32),
             8*2*(width+32)  );
            od_ec_enc_uint( &enc->ec , mvp->mv[1] + 8*(height+32),
             8*2*(height+32) );
          }
        }
      }
    }
#endif
    od_state_mc_predict(&enc->state, OD_FRAME_PREV);
#if defined(OD_DUMP_IMAGES)
    /*Dump reconstructed frame.*/
    /*od_state_dump_img(&enc->state,enc->state.io_imgs + OD_FRAME_REC,"rec");*/
    od_state_fill_vis(&enc->state);
    od_state_dump_img(&enc->state, &enc->state.vis_img, "vis");
#endif
  }
  scale = 10; /*atoi(getenv("QUANT"));*/
  {
    od_mb_enc_ctx mbctx;
    od_coeff *ctmp[OD_NPLANES_MAX];
    od_coeff *dtmp[OD_NPLANES_MAX];
    od_coeff *mctmp[OD_NPLANES_MAX];
    od_coeff *mdtmp[OD_NPLANES_MAX];
    od_coeff *ltmp[OD_NPLANES_MAX];
    od_coeff *lbuf[OD_NPLANES_MAX];
    int xdec;
    int ydec;
    int nvmbs;
    int nhmbs;
    int mby;
    int mbx;
    int iyfill;
    int oyfill;
    int mi;
    int h;
    int w;
    int y;
    int x;
    mbctx.is_keyframe = 1;/*( enc->state.cur_time %
                            (enc->state.info.keyframe_rate) == 0) ? 1 : 0;*/
    OD_LOG((OD_LOG_ENCODER, OD_LOG_INFO,"is_keyframe=%d",mbctx.is_keyframe ));
    nhmbs = enc->state.nhmbs;
    nvmbs = enc->state.nvmbs;
    /*Initialize the data needed for each plane.*/
    mbctx.modes = _ogg_calloc((frame_width >> 2)*(frame_height >> 2),
     sizeof(*mbctx.modes));
    for (mi = 0; mi < OD_INTRA_NMODES; mi++) {
     mbctx.mode_p0[mi] = 32768/OD_INTRA_NMODES;
    }
    for (pli = 0; pli < nplanes; pli++) {
      generic_model_init(&mbctx.model_dc[pli]);
      generic_model_init(&mbctx.model_g[pli]);
      generic_model_init(&mbctx.model_ym[pli]);
      mbctx.ex_dc[pli] = pli > 0 ? 8 : 32768;
      mbctx.ex_g[pli] = 8;
      xdec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].xdec;
      ydec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ydec;
      w = frame_width >> xdec;
      h = frame_height >> ydec;
      od_ec_enc_uint(&enc->ec, scale, 512);
      ctmp[pli] = _ogg_calloc(w*h, sizeof(*ctmp[pli]));
      dtmp[pli] = _ogg_calloc(w*h, sizeof(*dtmp[pli]));
      mctmp[pli] = _ogg_calloc(w*h, sizeof(*mctmp[pli]));
      mdtmp[pli] = _ogg_calloc(w*h, sizeof(*mdtmp[pli]));
      /*We predict chroma planes from the luma plane.  Since chroma can be
        subsampled, we cache subsampled versions of the luma plane in the
        frequency domain.  We can share buffers with the same subsampling.*/
      if (pli > 0) {
        int plj;
        if (xdec || ydec) {
          for (plj = 1; plj < pli; plj++) {
            if (xdec == enc->state.io_imgs[OD_FRAME_INPUT].planes[plj].xdec
             && ydec == enc->state.io_imgs[OD_FRAME_INPUT].planes[plj].ydec) {
              ltmp[pli] = NULL;
              lbuf[pli] = ltmp[plj];
            }
          }
          if (plj >= pli) {
            lbuf[pli] = ltmp[pli] = _ogg_calloc(w*h, sizeof(*ltmp));
          }
        }
        else{
          ltmp[pli] = NULL;
          lbuf[pli] = ctmp[pli];
        }
      }
      else lbuf[pli] = ltmp[pli] = NULL;
      od_adapt_row_init(&enc->state.adapt_row[pli]);
    }
    iyfill = 0;
    oyfill = 0;
    for (mby = 0; mby < nvmbs; mby++) {
      int next_iyfill;
      int next_oyfill;
      int ixfill;
      int oxfill;
      od_adapt_ctx adapt_hmean[OD_NPLANES_MAX];
      for (pli = 0; pli < nplanes; pli++) {
        od_adapt_hmean_init(&adapt_hmean[pli]);
      }
      next_iyfill = mby + 1 < nvmbs ? ((mby + 1) << 4) + 8 : frame_height;
      next_oyfill = mby + 1 < nvmbs ? ((mby + 1) << 4) - 8 : frame_height;
      ixfill = 0;
      oxfill = 0;
      for (mbx = 0; mbx < nhmbs; mbx++) {
        int next_ixfill;
        int next_oxfill;
        next_ixfill = mbx + 1 < nhmbs ? ((mbx + 1) << 4) + 8 : frame_width;
        next_oxfill = mbx + 1 < nhmbs ? ((mbx + 1) << 4) - 8 : frame_width;
        for (pli = 0; pli < nplanes; pli++) {
          od_adapt_row_ctx *adapt_row;
          unsigned char *data;
          unsigned char *mdata;
          int ystride;
          int by;
          int bx;
          mbctx.c = ctmp[pli];
          mbctx.d = dtmp[pli];
          mbctx.mc = mctmp[pli];
          mbctx.md = mdtmp[pli];
          mbctx.l = lbuf[pli];
          xdec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].xdec;
          ydec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ydec;
          w = frame_width >> xdec;
          h = frame_height >> ydec;
          /*Construct the luma predictors for chroma planes.*/
          if (ltmp[pli] != NULL) {
            OD_ASSERT(pli > 0);
            OD_ASSERT(mbctx.l == ltmp[pli]);
            for (by = mby << (2 - ydec); by < (mby + 1) << (2 - ydec); by++) {
              for (bx = mbx << (2 - xdec); bx < (mbx + 1) << (2 - xdec);
               bx++) {
                od_resample_luma_coeffs(mbctx.l + (by << 2)*w + (bx<<2), w,
                 dtmp[0] + (by << (2 + ydec))*frame_width + (bx<<(2 + xdec)),
                 frame_width, xdec, ydec, 4);
              }
            }
          }
          /*Collect the image data needed for this macro block.*/
          data = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].data;
          mdata = enc->state.io_imgs[OD_FRAME_REC].planes[pli].data;
          ystride = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ystride;
          for (y = iyfill >> ydec; y < next_iyfill >> ydec; y++) {
            for (x = ixfill >> xdec; x < next_ixfill >> xdec; x++) {
              mbctx.c[y*w + x] = data[ystride*y + x] - 128;
              if (!mbctx.is_keyframe)
                mbctx.mc[y*w + x] = mdata[ystride*y + x] - 128;
            }
          }
          /*Apply the prefilter across the bottom block edges.*/
          for (by = mby << (2 - ydec); by < ((mby + 1) << (2 - ydec))
           - ( mby + 1 >= nvmbs); by++) {
            for (x = ixfill >> xdec; x < next_ixfill >> xdec; x++) {
              od_coeff p[4];
              for (y = 0; y < 4; y++) p[y] = mbctx.c[((by << 2) + y + 2)*w + x];
              od_pre_filter4(p, p);
              for (y = 0; y < 4; y++) mbctx.c[((by << 2) + y + 2)*w + x] = p[y];
              if ( !mbctx.is_keyframe ) {
                for (y = 0; y < 4; y++) p[y] =
                 mbctx.c[((by << 2) + y + 2)*w + x];
                od_pre_filter4(p, p);
                for (y = 0; y < 4; y++) mbctx.mc[((by << 2) + y + 2)*w + x] =
                 p[y];
              }
            }
          }
          /*Apply the prefilter across the right block edges.*/
          for (y = (mby << (4 - ydec)); y < (mby + 1) << (4 - ydec); y++) {
            for (bx = mbx << (2 - xdec); bx < ((mbx + 1) << (2 - xdec))
             - ((mbx + 1) >= nhmbs); bx++) {
              od_pre_filter4(mbctx.c + y*w + (bx << 2) + 2, mbctx.c + y*w + (bx << 2) + 2);
              if ( !mbctx.is_keyframe ) od_pre_filter4(mbctx.mc + y*w + (bx << 2) + 2,
               mbctx.mc + y*w + (bx << 2) + 2);
            }
          }
          mbctx.nk = mbctx.k_total = mbctx.sum_ex_total_q8 = 0;
          mbctx.ncount = mbctx.count_total_q8 = mbctx.count_ex_total_q8 = 0;
          adapt_row = &enc->state.adapt_row[pli];
          od_adapt_update_stats(adapt_row, mbx, &adapt_hmean[pli], &mbctx.adapt);
          od_mb_encode(enc, &mbctx, scale, pli, mbx, mby);
          if (mbctx.nk > 0) {
            mbctx.adapt.curr[OD_ADAPT_K_Q8] = OD_DIVU_SMALL(mbctx.k_total << 8, mbctx.nk);
            mbctx.adapt.curr[OD_ADAPT_SUM_EX_Q8] =
             OD_DIVU_SMALL(mbctx.sum_ex_total_q8, mbctx.nk);
          } else {
            mbctx.adapt.curr[OD_ADAPT_K_Q8] = OD_ADAPT_NO_VALUE;
            mbctx.adapt.curr[OD_ADAPT_SUM_EX_Q8] = OD_ADAPT_NO_VALUE;
          }
          if (mbctx.ncount > 0)
          {
            mbctx.adapt.curr[OD_ADAPT_COUNT_Q8] =
             OD_DIVU_SMALL(mbctx.count_total_q8, mbctx.ncount);
            mbctx.adapt.curr[OD_ADAPT_COUNT_EX_Q8] =
             OD_DIVU_SMALL(mbctx.count_ex_total_q8, mbctx.ncount);
          } else {
            mbctx.adapt.curr[OD_ADAPT_COUNT_Q8] = OD_ADAPT_NO_VALUE;
            mbctx.adapt.curr[OD_ADAPT_COUNT_EX_Q8] = OD_ADAPT_NO_VALUE;
          }
          od_adapt_mb(adapt_row, mbx, &adapt_hmean[pli], &mbctx.adapt);
          /*Apply the postfilter across the left block edges.*/
          for (y = mby << (4 - ydec); y < (mby + 1) << (4 - ydec); y++) {
            for (bx = (mbx<<(2 - xdec)) + (mbx <= 0); bx < (mbx + 1) <<
             (2 - xdec); bx++) {
              od_post_filter4(mbctx.c + y*w + (bx << 2) - 2, mbctx.c + y*w
               + (bx << 2) - 2);
            }
          }
          /*Apply the postfilter across the top block edges.*/
          for (by = (mby << (2 - ydec)) + (mby <= 0); by < (mby + 1) <<
           (2 - ydec); by++) {
            for (x = oxfill >> xdec; x < next_oxfill >> xdec; x++) {
              od_coeff p[4];
              for (y = 0; y < 4; y++) p[y] = mbctx.c[((by << 2) + y - 2)*w + x];
              od_post_filter4(p,p);
              for (y = 0; y < 4; y++) mbctx.c[((by << 2) + y - 2)*w + x] = p[y];
            }
          }
          data = enc->state.io_imgs[OD_FRAME_REC].planes[pli].data;
          for (y = oyfill>>ydec; y < next_oyfill >> ydec; y++) {
            for (x = oxfill >> xdec; x < next_oxfill >> xdec; x++) {
              data[ystride*y + x] = OD_CLAMP255(mbctx.c[y*w + x] + 128);
            }
          }
        }
        ixfill = next_ixfill;
        oxfill = next_oxfill;
      }
      for (pli = 0; pli < nplanes; pli++) {
        od_adapt_row(&enc->state.adapt_row[pli], &adapt_hmean[pli]);
      }
      iyfill = next_iyfill;
      oyfill = next_oyfill;
    }
    for (pli = nplanes; pli-- > 0;) {
      _ogg_free(ltmp[pli]);
      _ogg_free(dtmp[pli]);
      _ogg_free(ctmp[pli]);
      _ogg_free(mctmp[pli]);
      _ogg_free(mdtmp[pli]);
    }
    _ogg_free(mbctx.modes);
  }
  /*Dump YUV*/
  od_state_dump_yuv(&enc->state, enc->state.io_imgs + OD_FRAME_REC, "out");
#if OD_DECODE_IN_ENCODE
  {
    int ret;
    od_img out_img;
    ogg_packet packet;
    ogg_uint32_t nbytes;
    od_dec_ctx dec;
    memcpy(&dec.state, &enc->state, sizeof(dec.state));
    memset(&packet, 0, sizeof(ogg_packet));
    packet.packet = od_ec_enc_done(&enc->ec, &nbytes);
    packet.bytes = nbytes;
    dec.packet_state = OD_PACKET_DATA;
    ret = daala_decode_packet_in(&dec, &out_img, &packet);
    OD_ASSERT(ret==0);
  }
#endif
  for (pli = 0; pli < nplanes; pli++) {
    unsigned char *data;
    ogg_int64_t mc_sqerr;
    ogg_int64_t enc_sqerr;
    ogg_uint32_t npixels;
    int ystride;
    int xdec;
    int ydec;
    int w;
    int h;
    int x;
    int y;
#ifdef OD_DPCM
    int err_accum;
    err_accum = 0;
#endif
    mc_sqerr = 0;
    enc_sqerr = 0;
    data = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].data;
    ystride = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ystride;
    xdec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].xdec;
    ydec = enc->state.io_imgs[OD_FRAME_INPUT].planes[pli].ydec;
    w = frame_width >> xdec;
    h = frame_height >> ydec;
    npixels = w*h;
    for (y = 0; y < h; y++) {
      unsigned char *prev_rec_row;
      unsigned char *rec_row;
      unsigned char *inp_row;
      rec_row = enc->state.io_imgs[OD_FRAME_REC].planes[pli].data +
       enc->state.io_imgs[OD_FRAME_REC].planes[pli].ystride*y;
      prev_rec_row = rec_row
       - enc->state.io_imgs[OD_FRAME_REC].planes[pli].ystride;
      inp_row = data + ystride*y;
      memcpy(enc->state.ref_line_buf[1], rec_row, w);
      for (x = 0; x < w; x++) {
        int rec_val;
        int inp_val;
        int diff;
        rec_val = rec_row[x];
        inp_val = inp_row[x];
        diff = inp_val - rec_val;
        mc_sqerr += diff*diff;
#ifdef OD_DPCM
        {
          int pred_diff;
          int qdiff;g
          /*DPCM code the residual with uniform quantization.
            This provides simulated residual coding errors, without
             introducing blocking artifacts.*/
          if (x > 0) {
            pred_diff = rec_row[x - 1] - enc->state.ref_line_buf[1][x - 1];
          }
          else pred_diff = 0;
          if (y > 0) {
            if (x > 0) {
              pred_diff += prev_rec_row[x - 1]
               - enc->state.ref_line_buf[0][x - 1];
            }
            pred_diff += prev_rec_row[x] - enc->state.ref_line_buf[0][x];
            if (x + 1 < w) {
              pred_diff += prev_rec_row[x + 1]
               - enc->state.ref_line_buf[0][x + 1];
            }
          }
          pred_diff = OD_DIV_ROUND_POW2(pred_diff, 2, 2);
          qdiff = (((diff - pred_diff) + ((diff - pred_diff) >> 31)
           + (5 + err_accum))/10)*10 + pred_diff;
          /*qdiff = (OD_DIV_ROUND_POW2(diff - pred_diff, 3, 4 + err_accum) << 3)
            + pred_diff;*/
          OD_LOG((OD_LOG_ENCODER, OD_LOG_DEBUG,
                  "d-p_d: %3i  e_a: %3i  qd-p_d: %3i  e_a: %i",
                  diff - pred_diff, err_accum, qdiff - pred_diff, diff - qdiff));
          err_accum += diff - qdiff;
          rec_row[x] = OD_CLAMP255(rec_val + qdiff);
        }
#else
/*        rec_row[x] = inp_val;*/
#endif
        diff = inp_val - rec_row[x];
        enc_sqerr += diff*diff;
      }
      prev_rec_row = enc->state.ref_line_buf[0];
      enc->state.ref_line_buf[0] = enc->state.ref_line_buf[1];
      enc->state.ref_line_buf[1] = prev_rec_row;
    }
    /* Commented out because these variables don't seem to exist.

       TODO: re-add this?

    OD_LOG((OD_LOG_ENCODER, OD_LOG_DEBUG,
            "Bytes: %d  ex_dc: %d ex_g: %d ex_k: %d",
            (od_ec_enc_tell(&enc->ec) + 7) >> 3, ex_dc, ex_g, ex_k));*/
    if (enc->state.ref_imgi[OD_FRAME_PREV] >= 0) {
      OD_LOG((OD_LOG_ENCODER, OD_LOG_DEBUG,
              "Plane %i, Squared Error: %12lli  Pixels: %6u  PSNR:  %5.2f",
              pli, (long long)mc_sqerr, npixels,
              10*log10(255*255.0*npixels/mc_sqerr)));
    }
    OD_LOG((OD_LOG_ENCODER, OD_LOG_DEBUG,
            "Encoded Plane %i, Squared Error: %12lli  Pixels: %6u  PSNR:  %5.2f",
            pli,(long long)enc_sqerr,npixels,10*log10(255*255.0*npixels/enc_sqerr)));
  }
  OD_LOG((OD_LOG_ENCODER, OD_LOG_INFO,
          "mode bits: %f/%f=%f", mode_bits, mode_count,
          mode_bits/mode_count));
  enc->packet_state = OD_PACKET_READY;
  od_state_upsample8(&enc->state,
   enc->state.ref_imgs + enc->state.ref_imgi[OD_FRAME_SELF],
   enc->state.io_imgs + OD_FRAME_REC);
#if defined(OD_DUMP_IMAGES)
  /*Dump reference frame.*/
  /*od_state_dump_img(&enc->state,
   enc->state.ref_img + enc->state.ref_imigi[OD_FRAME_SELF], "ref");*/
#endif
  if (enc->state.info.frame_duration == 0) enc->state.cur_time += duration;
  else enc->state.cur_time += enc->state.info.frame_duration;
  return 0;
}

int daala_encode_packet_out(daala_enc_ctx *enc, int last, ogg_packet *op) {
  ogg_uint32_t nbytes;
  if (enc == NULL || op == NULL) return OD_EFAULT;
  else if (enc->packet_state <= 0 || enc->packet_state == OD_PACKET_DONE) {
    return 0;
  }
  op->packet = od_ec_enc_done(&enc->ec, &nbytes);
  op->bytes = nbytes;
  OD_LOG((OD_LOG_ENCODER, OD_LOG_INFO, "Output Bytes: %ld", op->bytes));
  op->b_o_s = 0;
  op->e_o_s = last;
  op->packetno = 0;
  op->granulepos = enc->state.cur_time;
  if (last) enc->packet_state = OD_PACKET_DONE;
  else enc->packet_state = OD_PACKET_EMPTY;
  return 1;
}
