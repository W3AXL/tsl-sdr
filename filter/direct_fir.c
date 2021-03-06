/*
 *  direct_fir.c - A direct FIR, with arbitrary complex coefficients
 *
 *  Copyright (c)2017 Phil Vachon <phil@security-embedded.com>
 *
 *  This file is a part of The Standard Library (TSL)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <filter/filter.h>
#include <filter/direct_fir.h>
#include <filter/sample_buf.h>
#include <filter/complex.h>

#include <tsl/errors.h>
#include <tsl/assert.h>
#include <tsl/diag.h>
#include <tsl/safe_alloc.h>

#include <string.h>
#include <math.h>
#include <complex.h>

#if defined(_USE_ARM_NEON)
/* Use ARM NEON because configuration told us to */
#define _NEON_FIR_IMPLEMENTATION
#else
#define _DIRECT_FIR_IMPLEMENTATION
#endif /* determine which FIR implementation to use */

aresult_t direct_fir_init(struct direct_fir *fir, size_t nr_coeffs, const int16_t *fir_real_coeff,
        const int16_t *fir_imag_coeff, unsigned decimation_factor,
        bool derotate, uint32_t sampling_rate, int32_t freq_shift)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG(NULL != fir);
    TSL_ASSERT_ARG(0 != nr_coeffs);
    TSL_ASSERT_ARG(NULL != fir_real_coeff);
    TSL_ASSERT_ARG(NULL != fir_imag_coeff);
    TSL_ASSERT_ARG(0 != decimation_factor);

    DIAG("FIR: Preparing %zu coefficients, decimation by %u, with%s derotation, sampling rate = %u frequency_shift = %d",
            nr_coeffs, decimation_factor, true == derotate ? "" : "out", sampling_rate, freq_shift);

    memset(fir, 0, sizeof(struct direct_fir));

    TSL_BUG_IF_FAILED(TACALLOC((void **)&fir->fir_real_coeff, nr_coeffs, sizeof(int16_t), 16));
    memcpy(fir->fir_real_coeff, fir_real_coeff, nr_coeffs * sizeof(int16_t));
    TSL_BUG_IF_FAILED(TACALLOC((void **)&fir->fir_imag_coeff, nr_coeffs, sizeof(int16_t), 16));
    memcpy(fir->fir_imag_coeff, fir_imag_coeff, nr_coeffs * sizeof(int16_t));

    fir->decimate_factor = decimation_factor;
    fir->nr_coeffs = nr_coeffs;

    fir->rot_phase_re = 0;
    fir->rot_phase_im = 0;

    if (true == derotate) {
        double fwt0 = 2.0 * M_PI * (double)freq_shift / (double)sampling_rate,
               q15 = 1ll << Q_15_SHIFT;
        complex double derotate_incr = cexp(CMPLX(0, -fwt0 * (double)decimation_factor));
        fir->rot_phase_incr_re = (int32_t)(creal(derotate_incr) * q15);
        fir->rot_phase_incr_im = (int32_t)(cimag(derotate_incr) * q15);
        fir->rot_phase_re = 1ul << Q_15_SHIFT;
        fir->rot_phase_im = 0;
        DIAG("Derotation factor: %f, %f (%08x, %08x -> %f, %f)", creal(derotate_incr), cimag(derotate_incr),
                fir->rot_phase_incr_re, fir->rot_phase_incr_im,
                (double)fir->rot_phase_incr_re/q15, (double)fir->rot_phase_incr_im/q15);
    }


    return ret;
}

aresult_t direct_fir_cleanup(struct direct_fir *fir)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG(NULL != fir);

    if (NULL != fir->fir_real_coeff) {
        TFREE(fir->fir_real_coeff);
    }

    if (NULL != fir->fir_imag_coeff) {
        TFREE(fir->fir_imag_coeff);
    }

    if (NULL != fir->sb_active) {
        sample_buf_decref(fir->sb_active);
        fir->sb_active = NULL;
    }

    if (NULL != fir->sb_next) {
        sample_buf_decref(fir->sb_next);
        fir->sb_next = NULL;
    }

    fir->decimate_factor = 0;

    return ret;
}

aresult_t direct_fir_push_sample_buf(struct direct_fir *fir, struct sample_buf *buf)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG(NULL != fir);
    TSL_ASSERT_ARG(NULL != buf);

    TSL_BUG_ON(fir->sb_active == buf);
    TSL_BUG_ON(fir->sb_next == buf);

    if (NULL == fir->sb_active) {
        fir->sb_active = buf;
        TSL_BUG_ON(NULL != fir->sb_next);
    } else {
        if (NULL == fir->sb_next) {
            fir->sb_next = buf;
        } else {
            ret = A_E_BUSY;
            goto done;
        }
    }

    DIAG("PUSH(active = %p next = %p)", fir->sb_active, fir->sb_next);

    fir->nr_samples += buf->nr_samples;

done:
    return ret;
}

/**
 * Perform a phase derotation for the current sample.
 */
static
aresult_t _direct_fir_apply_derotation(struct direct_fir *fir, int32_t acc_re_in, int32_t acc_im_in,
        int32_t *acc_re_out, int32_t *acc_im_out)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG_DEBUG(NULL != fir);
    TSL_ASSERT_ARG_DEBUG(NULL != acc_re_out);
    TSL_ASSERT_ARG_DEBUG(NULL != acc_im_out);

    /* Apply the phase derotation to the sample */
    cmul_q15_q30(acc_re_in, acc_im_in, fir->rot_phase_re, fir->rot_phase_im,
            acc_re_out, acc_im_out);

    /* Now add the phase rotation increment to the phase rotation for the next sample */
    cmul_q15_q15(fir->rot_phase_re, fir->rot_phase_im, fir->rot_phase_incr_re, fir->rot_phase_incr_im,
            &fir->rot_phase_re, &fir->rot_phase_im);

    fir->rot_counter++;

    return ret;
}

#if defined(_NEON_FIR_IMPLEMENTATION)
#include <arm_neon.h>

static
aresult_t _direct_fir_process_sample(struct direct_fir *fir, int16_t *psample_real, int16_t *psample_imag)
{
    aresult_t ret = A_OK;

    size_t coeffs_remain = 0,
           buf_offset = 0;
    struct sample_buf *cur_buf = NULL;

    int32_t acc_re = 0,
            acc_im = 0;

    TSL_ASSERT_ARG_DEBUG(NULL != fir);
    TSL_ASSERT_ARG_DEBUG(NULL != psample_real);
    TSL_ASSERT_ARG_DEBUG(NULL != psample_imag);
    TSL_BUG_ON(NULL == fir->sb_active);

    coeffs_remain = fir->nr_coeffs;
    cur_buf = fir->sb_active;
    buf_offset = fir->sample_offset;

    /* Check if we have enough samples available */
    if (fir->sample_offset + fir->nr_coeffs > fir->sb_active->nr_samples && fir->sb_next == NULL) {
        ret = A_E_DONE;
        goto done;
    }

    do {
        /* Temporary vector accumulators */
        int32x4_t acc_re_v = { 0, 0, 0, 0 },
                  acc_im_v = { 0, 0, 0, 0 };

        /* Figure out how many samples to pull out */
        size_t nr_samples_in = cur_buf->nr_samples - buf_offset,
               start_coeff = fir->nr_coeffs - coeffs_remain;

        /* Snap to either the number of coefficients in the FIR or the number of remaining
         * coefficients, whichever is smaller.
         */
        nr_samples_in = BL_MIN2(nr_samples_in, coeffs_remain);

        if (nr_samples_in != fir->nr_coeffs) {
            DIAG("Samples from buffer %p: %zu, start coefficient: %zu (%zu remain) start offset %zu (of %u)",
                    cur_buf, nr_samples_in, start_coeff, coeffs_remain, buf_offset, fir->sb_active->nr_samples);
        }

        for (size_t i = 0; i < nr_samples_in / 4; i++) {
            size_t start_samp = i * 4;
            int16_t *sample_base =
                (int16_t *)((uint8_t *)cur_buf->data_buf + (sizeof(int16_t) * 2 * (buf_offset + start_samp)));

            /* Samples loaded at offset */
            int16x4x2_t samples;
            int32x4_t f_acc;
            int16x4_t c_re,
                      c_im;

            __builtin_prefetch(sample_base);
            __builtin_prefetch(fir->fir_real_coeff + start_samp + start_coeff);
            __builtin_prefetch(fir->fir_imag_coeff + start_samp + start_coeff);

            samples = vld2_s16(sample_base);

            /* c_re = vec4(fir_real_coeff + start_samp + start_coeff) */
            c_re = vld1_s16(fir->fir_real_coeff + start_samp + start_coeff);
            /* c_im = vec4(fir_imag_coeff + start_samp + start_coeff) */
            c_im = vld1_s16(fir->fir_imag_coeff + start_samp + start_coeff);

            /* f_re = s_re * c_re */
            f_acc = vmull_s16(samples.val[0], c_re);
            /* f_re -= s_im * c_im */
            f_acc = vmlsl_s16(f_acc, samples.val[1], c_im);
            acc_re_v = vaddq_s32(acc_re_v, f_acc);

            /* f_im = s_im * c_re */
            f_acc = vmull_s16(samples.val[1], c_re);
            /* f_im += c_im * s_re */
            f_acc = vmlal_s16(f_acc, c_im, samples.val[0]);
            acc_im_v = vaddq_s32(acc_im_v, f_acc);
        }

        /* Reduce the accumulators */
        acc_re += acc_re_v[0] + acc_re_v[1] + acc_re_v[2] + acc_re_v[3];
        acc_im += acc_im_v[0] + acc_im_v[1] + acc_im_v[2] + acc_im_v[3];

        size_t res_start = nr_samples_in & ~((size_t)4 - 1);

        for (size_t i = 0; i < nr_samples_in % 4; i++) {
            TSL_BUG_ON(i + res_start + start_coeff >= fir->nr_coeffs);
            TSL_BUG_ON(i + res_start + buf_offset >= cur_buf->nr_samples);
            DIAG("Processing sample %zu (base = %zu)", i + res_start, res_start);

            int16_t *sample = &((int16_t *)cur_buf->data_buf)[2 * (buf_offset + res_start + i)];

            int32_t s_re = sample[0],
                    s_im = sample[1],
                    c_re = fir->fir_real_coeff[i + start_coeff + res_start],
                    c_im = fir->fir_imag_coeff[i + start_coeff + res_start],
                    f_re = 0,
                    f_im = 0;

            /* Filter the sample */
            cmul_q15_q30(c_re, c_im, s_re, s_im, &f_re, &f_im);

            /* Accumulate the sample */
            acc_re += f_re;
            acc_im += f_im;
        }

        /* If we iterate through, we'll start at the beginning of the next buffer */
        buf_offset = 0;
        cur_buf = fir->sb_next;
        coeffs_remain -= nr_samples_in;

        if (0 != coeffs_remain) {
            DIAG("COEFF: %zu remain, buffer = %p (prev = %p), %zu samples in",
                    coeffs_remain, cur_buf, fir->sb_active, nr_samples_in);
        }
    } while (0 != coeffs_remain);

    /* Check if the next sample will start in the following buffer; if so, move along */
    if (fir->sample_offset + fir->decimate_factor >= fir->sb_active->nr_samples) {
        size_t cur_nr_samples = fir->sb_active->nr_samples;

        TSL_BUG_IF_FAILED(sample_buf_decref(fir->sb_active));

        fir->sb_active = fir->sb_next;
        fir->sb_next = NULL;
        fir->sample_offset = (fir->sample_offset + fir->decimate_factor) - cur_nr_samples;
    } else {
        fir->sample_offset += fir->decimate_factor;
    }

    fir->nr_samples -= fir->decimate_factor;

    /* Apply a phase (de)rotation, if appropriate */
    if (!(0 == fir->rot_phase_incr_re && 0 == fir->rot_phase_incr_im)) {
        /* Convert the accumulated sample to Q.15 */
        TSL_BUG_IF_FAILED(_direct_fir_apply_derotation(fir, round_q30_q15(acc_re), round_q30_q15(acc_im),
                    &acc_re, &acc_im));
    }

    /* Return the computed sample, in Q.15 (currently in Q.30 due to the prior multiplication) */
    *psample_real = round_q30_q15(acc_re);
    *psample_imag = round_q30_q15(acc_im);

done:
    return ret;
}

#elif defined(_DIRECT_FIR_IMPLEMENTATION)
static
aresult_t _direct_fir_process_sample(struct direct_fir *fir, int16_t *psample_real, int16_t *psample_imag)
{
    aresult_t ret = A_OK;

    int32_t acc_re = 0,
            acc_im = 0;
    size_t coeffs_remain = 0,
           buf_offset = 0;
    struct sample_buf *cur_buf = NULL;

    TSL_ASSERT_ARG_DEBUG(NULL != fir);
    TSL_ASSERT_ARG_DEBUG(NULL != psample_real);
    TSL_ASSERT_ARG_DEBUG(NULL != psample_imag);

    coeffs_remain = fir->nr_coeffs;
    cur_buf = fir->sb_active;
    buf_offset = fir->sample_offset;

    /* Check if we have enough samples available */
    if (fir->sample_offset + fir->nr_coeffs > fir->sb_active->nr_samples &&
            fir->sb_next == NULL)
    {
        ret = A_E_DONE;
        goto done;
    }

    /* Walk the number of samples in the current buffer up to the filter size */
    do {
        /* Figure out how many samples to pull out */
        size_t nr_samples_in = cur_buf->nr_samples - buf_offset,
               start_coeff = fir->nr_coeffs - coeffs_remain;

        /* Snap to either the number of coefficients in the FIR or the number of remaining
         * coefficients, whichever is smaller.
         */
        nr_samples_in = BL_MIN2(nr_samples_in, coeffs_remain);

        for (size_t i = 0; i < nr_samples_in; i++) {
            TSL_BUG_ON(i + start_coeff >= fir->nr_coeffs);
            TSL_BUG_ON(i + buf_offset >= cur_buf->nr_samples);

            int16_t *sample = &((int16_t *)cur_buf->data_buf)[2 * (buf_offset + i)];

            int32_t s_re = (int32_t)sample[0],
                    s_im = (int32_t)sample[1],
                    c_re = fir->fir_real_coeff[i + start_coeff],
                    c_im = fir->fir_imag_coeff[i + start_coeff],
                    f_re = 0,
                    f_im = 0;

            /* Filter the sample */
            cmul_q15_q30(c_re, c_im, s_re, s_im, &f_re, &f_im);

            /* Accumulate the sample */
            acc_re += f_re;
            acc_im += f_im;
        }

        /* If we iterate through, we'll start at the beginning of the next buffer */
        buf_offset = 0;
        cur_buf = fir->sb_next;
        coeffs_remain -= nr_samples_in;
    } while (coeffs_remain != 0);

    /* Check if the next sample will start in the following buffer; if so, move along */
    if (fir->sample_offset + fir->decimate_factor > fir->sb_active->nr_samples) {
        TSL_BUG_IF_FAILED(sample_buf_decref(fir->sb_active));
        fir->sb_active = fir->sb_next;
        fir->sb_next = NULL;
        fir->sample_offset = (fir->sample_offset + fir->decimate_factor) - fir->sb_active->nr_samples;
    } else {
        fir->sample_offset += fir->decimate_factor;
    }

    fir->nr_samples -= fir->decimate_factor;

    /* Apply a phase rotation, if appropriate */
    if (!(0 == fir->rot_phase_incr_re && 0 == fir->rot_phase_incr_im)) {
        /* Convert the accumulated sample to Q.15 and apply derotation */
        TSL_BUG_IF_FAILED(_direct_fir_apply_derotation(fir, round_q30_q15(acc_re), round_q30_q15(acc_im), &acc_re, &acc_im));
    }

    /* Return the computed sample, in Q.15 (currently in Q.30 due to the prior multiplications) */
    *psample_real = round_q30_q15(acc_re);
    *psample_imag = round_q30_q15(acc_im);

done:
    return ret;
}
#else /* no FIR implementation defined */
#error No FIR implementation has been defined.
#endif /* _NEON_FIR_IMPLEMENTATION */

aresult_t direct_fir_process(struct direct_fir *fir, int16_t *out_buf, size_t nr_out_samples,
        size_t *nr_out_samples_generated)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG(NULL != fir);
    TSL_ASSERT_ARG(NULL != out_buf);
    TSL_ASSERT_ARG(0 != nr_out_samples);
    TSL_ASSERT_ARG(NULL != nr_out_samples_generated);

    TSL_BUG_ON(NULL == fir->fir_real_coeff);
    TSL_BUG_ON(NULL == fir->fir_imag_coeff);
    TSL_BUG_ON(0 == fir->nr_coeffs);

    *nr_out_samples_generated = 0;

    if (NULL == fir->sb_active && NULL == fir->sb_next) {
        goto done;
    }

    for (size_t i = 0; i < nr_out_samples; i++) {
        if (A_E_DONE == _direct_fir_process_sample(fir, &out_buf[2 * i], &out_buf[2 * i + 1])) {
            *nr_out_samples_generated = i;
            goto done;
        }
    }

    *nr_out_samples_generated = nr_out_samples;

done:
    return ret;
}

aresult_t direct_fir_can_process(struct direct_fir *fir, bool *pcan_process, size_t *pest_count)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG(NULL != fir);
    TSL_ASSERT_ARG(NULL != pcan_process);

    /* The trick for this is to see if there are at least enough samples to run a single pass of the
     * FIR.
     */
    *pcan_process = fir->nr_samples >= fir->nr_coeffs;

    if (NULL != pest_count) {
        *pest_count = fir->nr_samples/fir->nr_coeffs;
    }

    return ret;
}

aresult_t direct_fir_full(struct direct_fir *fir, bool *pfull)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG_DEBUG(NULL != fir);
    TSL_ASSERT_ARG_DEBUG(NULL != pfull);

    *pfull = (NULL != fir->sb_next);

    return ret;
}

