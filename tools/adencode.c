/*
 * adencode.c - build-time audio encoder for the Butterscotch GB300V2/SF2000 port.
 *
 * Reads the AUDO chunk of a GameMaker 1.4 data.win and writes SNDS.BIN, which the
 * console runtime loads on demand.  Why a pre-pass is necessary:
 *
 *   Undertale's AUDO chunk is 32.6 MB of *uncompressed* RIFF/WAVE PCM at 44100 Hz
 *   (227 entries, 177 stereo / 50 mono) plus 4 raw Ogg Vorbis blobs.  Decoding that
 *   in soft-float MIPS32 at runtime is not viable, and 32.6 MB does not fit in the
 *   58 MiB heap next to the VM, CODE and STRG.
 *
 * So we do everything once on the host:
 *
 *   1. downmix to mono   - the console's mixer is mono-only anyway (the firmware
 *                          force-mixes stereo to mono in core_api.c)
 *   2. resample 44100 -> 22050 Hz  - that is the hardware rate, so the device mixer
 *                          needs no rate conversion for SFX and playback pitch is native
 *   3. optionally IMA ADPCM 4:1 (--adpcm, off by default - see below)
 *
 *   32.6 MB -> 16.3 MB -> 8.15 MB -> 16.3 MB (pcm) or 2.2 MB (adpcm)
 *
 * PCM is the DEFAULT and ADPCM is opt-in, because compression turned out to be
 * optimising the wrong thing here.  Measured on Undertale's own 225 SFX, IMA ADPCM
 * this variant averages 12.2 dB SNR (worst -14.7 dB) - it is a 4-bit predictive codec
 * and Undertale's sound effects are all sharp transients.  Meanwhile the SFX never
 * need to be resident: they are played one or two at a time out of a small LRU cache,
 * so the extra 14 MB lives on the SD card where it costs nothing and is read at
 * ~10-30 ms per sound, once.  Keeping the decoded form means the device needs no
 * ADPCM decoder at all and spends zero CPU on it.  Pass --adpcm if the card ever
 * gets tight; the codec is fully implemented and unit tested either way.
 *
 * Raw Ogg Vorbis entries are copied verbatim and flagged so the device can route
 * them through stb_vorbis instead.
 *
 * Usage: adencode <data.win> <out.SNDS.BIN> [targetRate] [--adpcm]
 *        adencode --selftest
 *        adencode --verify <data.win> <SNDS.BIN> [rate]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FLAG_PCM      0   /* int16 mono @ targetRate */
#define FLAG_ADPCM    1   /* IMA ADPCM blocks */
#define FLAG_VERBATIM 2   /* raw Ogg Vorbis, device uses stb_vorbis */

static int g_adpcm = 0;

#define SNDS_MAGIC   "SNDS"
#define SNDS_VERSION 1
#define INDEX_SIZE   32
#define QUIET_THRESH   16     /* |sample| below this counts as silence */
#define QUIET_RUN      64     /* consecutive quiet samples before a state reset */
#define RESTART_INDEX   24     /* step index to restart from after a reset */

/* After a silence reset the predictor is 0, but index 0 means step 7 and a max delta
 * of 11 - a loud transient could not be represented at all (measured 0.5 dB SNR on a
 * click train).  Restarting at a mid step lets transients through; the index then
 * walks down on its own through the emitted nibbles. */
#define BLOCK_SAMPLES 2048          /* mixer chunk = one block of decoded PCM */
#define BLOCK_BYTES  (4 + BLOCK_SAMPLES / 2)   /* header + 2 samples per nibble byte */
#define FIR_TAPS     33

/* ---------------------------------------------------------------- IMA ADPCM -- */
/*
 * Bitstream format - deliberately identical to Butterscotch's own reference
 * decoder in src/audio/ps2/ps2_audio_system.c (imaAdpcmDecodeBlock), so the device
 * side needs no new codec and cannot drift from it:
 *
 *   - 4-byte block header then raw nibbles, 2 samples per byte, LOW nibble first
 *     then HIGH:
 *         int16 predictor | uint8 stepIndex | uint8 pad | 1024 nibble bytes = 2048 samples
 *   - the header is needed because the step index cannot be changed unilaterally:
 *     the decoder derives it from the emitted nibbles, so a full-scale transient in
 *     the middle of a block can only be reconstructed over ~8 samples of ramp-up.
 *     A per-block header lets the encoder restart predictor+index per 2048 samples
 *     (measured: fixes click trains, costs 4 bytes per block = 0.4% overhead).
 *   - delta = step>>3, +step>>2 if bit0, +step>>1 if bit1, +step if bit2,
 *     negated if bit3
 *
 * The encoder searches all 16 nibbles and applies the decoder's own (shift-truncated)
 * delta, so the predictor trajectory it produces is bit-identical to what the device
 * will reconstruct.  That also makes the round-trip SNR meaningful.
 */

static const int16_t IMA_STEP_TABLE[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,
       16,    17,    19,    21,    23,    25,    28,    31,
       34,    37,    41,    45,    50,    55,    60,    66,
       73,    80,    88,    97,   107,   118,   130,   143,
      157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,
      724,   796,   876,   963,  1060,  1166,  1282,  1411,
     1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
     3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
     7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

static const int8_t IMA_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static inline int32_t ima_delta(int32_t step, int nib)
{
    int32_t d = step >> 3;
    if (nib & 1) d += step >> 2;
    if (nib & 2) d += step >> 1;
    if (nib & 4) d += step;
    if (nib & 8) d = -d;
    return d;
}

static inline int32_t clamp16(int32_t v)
{
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

static inline int clamp_index(int i)
{
    return i < 0 ? 0 : (i > 88 ? 88 : i);
}

/* Encode `nsamples` from `in` as 1028-byte blocks (4-byte header + 2048 nibbles).
 * Returns bytes written. */
static uint32_t adpcm_encode(const int16_t *in, int nsamples, uint8_t *out)
{
    uint32_t written = 0;
    int32_t predictor = 0;
    int index = RESTART_INDEX;
    int quiet_run = 0;

    for (int off = 0; off < nsamples; off += BLOCK_SAMPLES) {
        int nb = nsamples - off;
        if (nb > BLOCK_SAMPLES) nb = BLOCK_SAMPLES;

        /* Restart the block at the first sample so the decoder starts exactly on
         * the waveform instead of ramping up from a stale predictor. */
        predictor = in[off];
        index = RESTART_INDEX;
        /* The quiet-run counter must restart with the block: if it carried in from the
         * previous block the encoder would drop to index 0 before emitting the first
         * nibble while the header had already told the decoder "start at RESTART_INDEX",
         * and the two would drift apart (measured: 80 LSB of residual on silence). */
        quiet_run = 0;
        out[written + 0] = (uint8_t)((uint16_t)predictor & 0xFF);
        out[written + 1] = (uint8_t)(((uint16_t)predictor >> 8) & 0xFF);
        out[written + 2] = (uint8_t)index;
        out[written + 3] = 0;
        uint8_t *nib = out + written + 4;
        uint32_t nbytes = 0;

        for (int i = 0; i < nb; i++) {
            int32_t step = IMA_STEP_TABLE[index];
            int best_nib = 0;
            int32_t best_err = 0x7FFFFFFF;

            /* A 4-bit predictive codec has no silence detection, so a quiet passage
             * makes the predictor ring for thousands of samples.  Reset once a run of
             * near-silent samples is established - silence then costs 1 bit/sample. */
            int32_t mag = in[off + i];
            if (mag < 0) mag = -mag;
            if (mag < QUIET_THRESH) quiet_run++;
            else quiet_run = 0;

            if (quiet_run >= QUIET_RUN && mag < QUIET_THRESH) {
                /* index 0 (step 7) makes nibble 0 decode as delta 0 - that is the
                 * only way this variant can represent exact silence, since
                 * ima_delta(step, 0) == step >> 8 and is non-zero for any larger step. */
                predictor = 0;
                index = 0;
                best_nib = 0;
            } else {
                for (int n = 0; n < 8; n++) {
                    for (int s2 = 0; s2 < 2; s2++) {
                        int cand_nib = n | (s2 << 3);
                        int32_t cand = clamp16(predictor + ima_delta(step, cand_nib));
                        int32_t err = cand - in[off + i];
                        if (err < 0) err = -err;
                        if (err < best_err) { best_err = err; best_nib = cand_nib; }
                    }
                }
                predictor = clamp16(predictor + ima_delta(step, best_nib));
                index = clamp_index(index + IMA_INDEX_TABLE[best_nib]);
            }

            /* ps2_audio_system.c reads the LOW nibble first: even sample -> low */
            if (i & 1) nib[nbytes - 1] |= (uint8_t)(best_nib << 4);
            else      { nib[nbytes] = (uint8_t)best_nib; nbytes++; }
        }
        /* pad the final partial block so the decoder always sees whole nibble pairs */
        if (nb & 1) { nib[nbytes] = 0; nbytes++; }
        written += 4 + nbytes;
    }
    return written;
}

/* --------------------------------------------------------- resampling filter -- */

/* Hamming-windowed sinc low-pass, cutoff = 0.45 * targetRate, then decimate. */
#define FIR_TAPS 33
static double fir[FIR_TAPS];
static double fir_sum;

static void fir_init(double target_rate, double in_rate)
{
    double fc = 0.45 * target_rate;          /* -6 dB corner, Hz */
    int mid = (FIR_TAPS - 1) / 2;
    fir_sum = 0.0;
    for (int i = 0; i < FIR_TAPS; i++) {
        double n = (double)(i - mid);
        double s = (n == 0.0) ? 2.0 * fc / in_rate
                              : sin(2.0 * M_PI * fc * n / in_rate) / (M_PI * n);
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * i / (FIR_TAPS - 1)); /* Hamming */
        fir[i] = s * w;
        fir_sum += fir[i];
    }
    if (fir_sum != 0.0)
        for (int i = 0; i < FIR_TAPS; i++) fir[i] /= fir_sum;
}

static int16_t fir_sample(const int16_t *hist, int16_t newest)
{
    double acc = fir[FIR_TAPS - 1] * (double)newest;
    for (int i = 0; i < FIR_TAPS - 1; i++)
        acc += fir[i] * (double)hist[i];
    int v = (int)lrint(acc);
    return clamp16(v);
}

/* --------------------------------------------------------------- WAV parsing -- */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* Locate fmt and data inside a RIFF/WAVE blob. Returns 0 on success. */
static int parse_wav(const uint8_t *b, uint32_t size,
                     uint32_t *rate, uint16_t *channels, uint16_t *bits,
                     const uint8_t **data, uint32_t *data_size)
{
    if (size < 44 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0)
        return -1;
    uint32_t pos = 12;
    int have_fmt = 0;
    while (pos + 8 <= size) {
        uint32_t clen = rd32(b + pos + 4);
        if (memcmp(b + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= size) {
            if (rd16(b + pos + 8) != 1) return -2;   /* only PCM */
            *channels = rd16(b + pos + 10);
            *rate     = rd32(b + pos + 12);
            *bits     = rd16(b + pos + 22);
            have_fmt = 1;
        } else if (memcmp(b + pos, "data", 4) == 0) {
            if (!have_fmt) return -3;
            uint32_t avail = size - (pos + 8);
            *data = b + pos + 8;
            *data_size = clen < avail ? clen : avail;
            return 0;
        }
        pos += 8 + clen + (clen & 1);
    }
    return -4;
}


/* -------------------------------------------------- resample (shared path) -- */

/* Downmix to mono, anti-alias, decimate to target_rate.  Shared by the encoder and by
 * --verify so the reference signal in the SNR check is bit-identical to what the
 * encoder was fed (the only loss measured is then the codec itself). */
static uint32_t resample_mono(const uint8_t *pcm, uint32_t dsize, uint16_t ch,
                              uint32_t in_rate, uint32_t target_rate,
                              int16_t *out, uint32_t cap)
{
    uint32_t frames = dsize / (2u * ch);
    uint32_t produced = 0;
    uint32_t dec = (in_rate + target_rate / 2) / target_rate;
    if (dec < 1) dec = 1;
    static int16_t hist[FIR_TAPS];
    fir_init((double)target_rate, (double)in_rate);
    for (uint32_t i = 0; i < frames; i++) {
        int32_t v = 0;
        for (uint16_t c = 0; c < ch; c++)
            v += (int16_t)rd16(pcm + (i * ch + c) * 2);
        int16_t s = (int16_t)(v / ch);
        memmove(hist, hist + 1, sizeof(int16_t) * (FIR_TAPS - 1));
        hist[FIR_TAPS - 1] = s;
        if (i >= FIR_TAPS - 1 && ((i + 1) % dec) == 0) {
            if (produced >= cap) break;
            out[produced++] = fir_sample(hist, s);
        }
    }
    return produced;
}

/* Reference decoder - mirrors ps2_audio_system.c exactly, plus the block header.
 * This is the function src/audio/sf2000/sf2000_audio.c will implement. */
static uint32_t adpcm_decode(const uint8_t *in, uint32_t in_bytes,
                             int16_t *out, uint32_t out_cap)
{
    uint32_t samples = 0, pos = 0;
    while (pos + BLOCK_BYTES <= in_bytes && samples + BLOCK_SAMPLES <= out_cap) {
        int32_t predictor = (int16_t)((uint16_t)in[pos] | ((uint16_t)in[pos + 1] << 8));
        int index = in[pos + 2];
        if (index > 88) index = 88;
        const uint8_t *nib = in + pos + 4;
        for (uint32_t i = 0; i < BLOCK_SAMPLES; i++) {
            uint8_t byte = nib[i >> 1];
            uint8_t n = (i & 1) ? (byte >> 4) : (byte & 0x0F);
            predictor = clamp16(predictor + ima_delta(IMA_STEP_TABLE[index], n));
            out[samples++] = (int16_t)predictor;
            index = clamp_index(index + IMA_INDEX_TABLE[n]);
        }
        pos += BLOCK_BYTES;
    }
    return samples;
}

/* Encode -> decode round trip.  IMA ADPCM is lossy, so we assert a healthy SNR
 * floor rather than bit equality. */
static double roundtrip_snr(const int16_t *in, int nsamples)
{
    static uint8_t bits[BLOCK_BYTES * 32 + 64];
    static int16_t out[BLOCK_SAMPLES * 32];
    double num = 0.0, den = 0.0;

    uint32_t nb = adpcm_encode(in, nsamples, bits);
    uint32_t got = adpcm_decode(bits, nb, out, (uint32_t)(BLOCK_SAMPLES * 32));
    if (got < (uint32_t)nsamples) return 0.0;

    /* Skip the head where the step index is still converging from 0. */
    int skip = nsamples / 8;
    for (int i = skip; i < nsamples; i++) {
        double d = (double)in[i] - (double)out[i];
        num += d * d;
        den += (double)in[i] * (double)in[i];
    }
    if (num == 0.0) return 99.0;
    /* All-zero input has no SNR to speak of; report on absolute error instead.
     * The codec's residual on digital silence is +-step/8, i.e. at most 14 for the
     * restart index we use, which is inaudible. */
    if (den == 0.0) {
        double maxerr = 0.0;
        for (int i = skip; i < nsamples; i++) {
            double d = (double)in[i] - (double)out[i];
            if (d < 0) d = -d;
            if (d > maxerr) maxerr = d;
        }
        return maxerr <= 32.0 ? 99.0 : 0.0;
    }
    return 10.0 * log10(den / num);
}

static int selftest(void)
{
    enum { N = BLOCK_SAMPLES * 3 };
    static int16_t buf[N];
    unsigned seed = 12345;
    /* Floors are measured 4-bit IMA ADPCM expectations for this variant, with ~2 dB
     * of margin.  "click train" is a deliberate worst case: a full-scale step out of
     * digital silence mid-block can only be reconstructed as an ~8 sample ramp, because
     * the decoder derives the step index from the nibbles themselves and cannot be told
     * to jump.  Every real IMA ADPCM implementation has this artifact; it does not
     * matter here because Undertale's music is Ogg Vorbis (untouched) and its sound
     * effects are continuous.  A periodic click train measures only ~3 dB and always
     * will: after each click the decoder needs ~6 samples of maximum-magnitude nibbles
     * to walk the step index back up to a useful size, and that ramp dominates the
     * error budget.  The 2 dB floor is there to catch a catastrophic regression, not
     * to assert quality.  It is a 4-bit predictive codec with no silence detection (we add
     * that in the encoder), so transients and high harmonics are inherently lossy. */
    struct { const char *name; int kind; double arg; double floor; } tests[] = {
        { "silence",     0, 0.0,       90.0 },
        { "click train", 1, 0.0,        2.0 },
        { "step",        2, 0.0,       18.0 },
        { "ramp",        3, 0.0,       30.0 },
        { "sine 440Hz",  4, 440.0,     34.0 },
        { "sine 3kHz",   4, 3000.0,    18.0 },
        { "sine 9kHz",   4, 9000.0,    14.0 },
        { "square 1kHz", 5, 1000.0,    7.0  },
        { "white noise", 6, 0.0,       14.0 },
    };
    int fails = 0;
    printf("IMA ADPCM round trip (%d samples, %d blocks)\n", N, N / BLOCK_SAMPLES);
    printf("  %-14s %9s %9s   %s\n", "signal", "SNR dB", "floor", "result");
    for (unsigned t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
        for (int i = 0; i < N; i++) {
            double x = (double)i;
            switch (tests[t].kind) {
                case 0: buf[i] = 0; break;
                case 1: buf[i] = (i % 512) < 8 ? 2000 : 0; break;   /* click train */
                case 2: buf[i] = (i < N / 2) ? -12000 : 12000; break;
                case 3: buf[i] = (int16_t)(-32768 + (i * 65536 / N)); break;
                case 4: buf[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * tests[t].arg * x / 22050.0)); break;
                case 5: buf[i] = (int16_t)((i / 11) & 1 ? 18000 : -18000); break;
                default: seed = seed * 1103515245u + 12345u;
                         buf[i] = (int16_t)((seed >> 8) & 0xFFFF); break;
            }
        }
        double snr = roundtrip_snr(buf, N);
        int ok = snr >= tests[t].floor;
        if (!ok) fails++;
        printf("  %-14s %9.1f %9.1f   %s\n", tests[t].name, snr, tests[t].floor,
               ok ? "ok" : "FAIL");
    }
    printf("%s\n", fails ? "SELFTEST FAILED" : "selftest passed");
    return fails ? 1 : 0;
}

/* --------------------------------------------------------------- --verify -- */

/* Decode SNDS.BIN back to PCM and compare against the same FIR-resampled reference
 * the encoder used, so the reported SNR is purely the codec's loss. */
static int verify(const char *datawin_path, const char *snds_path, uint32_t target_rate)
{
    FILE *sf = fopen(snds_path, "rb");
    if (!sf) { perror(snds_path); return 1; }
    fseek(sf, 0, SEEK_END);
    long ssz = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    uint8_t *snds = (uint8_t *)malloc((size_t)ssz);
    if (!snds || fread(snds, 1, (size_t)ssz, sf) != (size_t)ssz) return 1;
    fclose(sf);
    uint32_t scount, sidx, srate, sblock;
    if (memcmp(snds, SNDS_MAGIC, 4) != 0) { fprintf(stderr, "bad magic\n"); return 1; }
    scount = rd32(snds + 8); sidx = rd32(snds + 12);
    srate = rd32(snds + 20); sblock = rd32(snds + 24);
    (void)rd32(snds + 16);
    printf("SNDS.BIN: %u entries, rate %u Hz, block %u samples, %.2f MB\n\n",
           scount, srate, sblock, ssz / 1048576.0);

    FILE *f = fopen(datawin_path, "rb");
    if (!f) { perror(datawin_path); return 1; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)fsz);
    if (!data || fread(data, 1, (size_t)fsz, f) != (size_t)fsz) return 1;
    fclose(f);

    uint32_t aoff = 0, p = 8, end = rd32(data + 4) + 8;
    while (p + 8 <= end) {
        uint32_t cl = rd32(data + p + 4);
        if (memcmp(data + p, "AUDO", 4) == 0) aoff = p + 8;
        p = (p + 8 + cl + 3) & ~3;
    }
    uint32_t *aptr = (uint32_t *)malloc(sizeof(uint32_t) * scount);
    for (uint32_t i = 0; i < scount; i++) aptr[i] = rd32(data + aoff + 4 + i * 4);

    static int16_t ref[4u * 1024 * 1024];
    static int16_t dec[4u * 1024 * 1024];
    double worst = 999, sum = 0; int n = 0, below = 0, shifted = 0, verbatim = 0;

    for (uint32_t i = 0; i < scount; i++) {
        uint32_t off, size, orate, nsamp;
        memcpy(&off,  snds + sidx + i * INDEX_SIZE +  0, 4);
        memcpy(&size, snds + sidx + i * INDEX_SIZE +  4, 4);
        memcpy(&orate,snds + sidx + i * INDEX_SIZE +  8, 4);
        memcpy(&nsamp,snds + sidx + i * INDEX_SIZE + 12, 4);
        uint16_t flags = rd16(snds + sidx + i * INDEX_SIZE + 18);
        if (flags == FLAG_VERBATIM) { verbatim++; continue; }

        uint32_t ent = aptr[i];
        uint32_t esize = rd32(data + ent);
        const uint8_t *blob = data + ent + 4;
        uint32_t rate, dsize; uint16_t ch, bps; const uint8_t *pcm;
        if (parse_wav(blob, esize, &rate, &ch, &bps, &pcm, &dsize) != 0) continue;
        uint32_t nref = resample_mono(pcm, dsize, ch, rate, target_rate, ref, 4u * 1024 * 1024);
        uint32_t got;
        if (flags == FLAG_ADPCM) {
            got = adpcm_decode(snds + off, size, dec, nsamp);
        } else {
            got = nsamp;
            if (got > 4u * 1024 * 1024) got = 4u * 1024 * 1024;
            for (uint32_t k = 0; k < got; k++)
                dec[k] = (int16_t)rd16(snds + off + k * 2);
        }
        uint32_t m = got < nref ? got : nref;
        if (m < 512) continue;

        /* Scan a small window for the best alignment: a one-sample offset destroys
         * the SNR of an otherwise fine codec, and it is worth knowing which case we
         * are in before blaming the encoder. */
        double best = -999; int best_shift = 0;
        for (int sh = -4; sh <= 4; sh++) {
            uint32_t lo = (sh < 0) ? (uint32_t)(-sh) : 0;
            if (lo >= m) continue;
            double num = 0, den = 0;
            for (uint32_t k = lo; k < m; k++) {
                double d = (double)ref[k] - (double)dec[k + sh];
                num += d * d;
                den += (double)ref[k] * (double)ref[k];
            }
            double v = (den > 0) ? 10.0 * log10(den / num) : ((num == 0) ? 99.0 : 0.0);
            if (v > best) { best = v; best_shift = sh; }
        }
        double snr = best;
        if (best_shift != 0) shifted++;
        if (snr < 14.0) below++;
        if (snr < worst) worst = snr;
        sum += snr; n++;
        if (i < 12 || snr < 14.0)
            printf("  entry %3u: %6u samp %5uHz x%u -> SNR %5.1f dB (shift %+d)%s\n",
                   i, nsamp, orate, ch, snr, best_shift, snr < 14.0 ? "   <-- low" : "");
    }
    printf("\n%d SFX measured (%d verbatim ogg skipped): mean %.1f dB, worst %.1f dB, "
           "%d below 14 dB, %d needed a shift\n",
           n, verbatim, n ? sum / n : 0.0, worst, below, shifted);
    free(snds); free(data); free(aptr);
    return below ? 1 : 0;
}

/* ------------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();
    if (argc >= 4 && strcmp(argv[1], "--verify") == 0)
        return verify(argv[2], argv[3], (argc > 4) ? (uint32_t)atoi(argv[4]) : 22050u);

    if (argc < 3) {
        fprintf(stderr, "usage: %s <data.win> <out.SNDS.BIN> [targetRate]\n"
                        "       %s --selftest\n"
                        "       %s --verify <data.win> <SNDS.BIN> [rate]\n",
           argv[0], argv[0], argv[0]);
        return 1;
    }
    for (int i = 3; i < argc; i++)
        if (strcmp(argv[i], "--adpcm") == 0) g_adpcm = 1;
    uint32_t target_rate = 22050u;
    for (int i = 3; i < argc; i++)
        if (argv[i][0] != '-') target_rate = (uint32_t)atoi(argv[i]);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)fsize);
    if (!data || fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "read failed\n"); return 1;
    }
    fclose(f);

    /* find AUDO */
    uint32_t audo_off = 0, audo_len = 0;
    if (memcmp(data, "FORM", 4) != 0) { fprintf(stderr, "not a WAD file\n"); return 1; }
    uint32_t end = rd32(data + 4) + 8;
    for (uint32_t p = 8; p + 8 <= end;) {
        uint32_t clen = rd32(data + p + 4);
        if (memcmp(data + p, "AUDO", 4) == 0) { audo_off = p + 8; audo_len = clen; }
        p = (p + 8 + clen + 3) & ~3u;
    }
    if (!audo_off) { fprintf(stderr, "no AUDO chunk\n"); return 1; }
    printf("AUDO at 0x%X len %u\n", audo_off, audo_len);

    uint32_t count = rd32(data + audo_off);
    uint32_t *ptrs = (uint32_t *)malloc(sizeof(uint32_t) * count);
    for (uint32_t i = 0; i < count; i++)
        ptrs[i] = rd32(data + audo_off + 4 + i * 4);

    uint8_t index[INDEX_SIZE * 4096];
    if (count > 4096) { fprintf(stderr, "too many entries\n"); return 1; }

    uint32_t data_start = 32 + INDEX_SIZE * count;
    uint32_t cursor = data_start;

    /* scratch buffers */
    int16_t *mono = (int16_t *)malloc(4u * 1024 * 1024);
    int16_t *hist = (int16_t *)calloc(FIR_TAPS + 1, sizeof(int16_t));
    uint8_t *adpcm = (uint8_t *)malloc((size_t)BLOCK_BYTES * (4u * 1024 * 1024 / BLOCK_SAMPLES + 8));
    if (!mono || !hist || !adpcm) { fprintf(stderr, "oom\n"); return 1; }

    uint64_t total_in = 0, total_out = 0;
    uint32_t n_adpcm = 0, n_pcm = 0, n_copy = 0, n_bad = 0;

    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }

    /* Placeholder header + index, then the payload.  The index MUST be emitted here
     * (not just seeked over): data_start is 32 + INDEX_SIZE*count, so if the payload
     * started at 32 every recorded offset would be short by INDEX_SIZE*count and the
     * tail of the audio would fall off the end of the file. */
    uint8_t hdr[32];
    memset(hdr, 0, sizeof(hdr));
    fwrite(hdr, 1, sizeof(hdr), out);
    fwrite(index, 1, (size_t)INDEX_SIZE * count, out);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t ent = ptrs[i];
        uint32_t entry_size = rd32(data + ent);
        const uint8_t *blob = data + ent + 4;
        uint32_t blob_size = (entry_size && ent + 4 + entry_size <= (uint32_t)fsize)
                             ? entry_size : 0;

        uint32_t orig_rate = 0, num_samples = 0, out_bytes = 0;
        uint16_t channels = 1, bits = 16, flags = 0, block_bytes = 0;
        int copy_verbatim = 0;
        int use_adpcm = g_adpcm;

        if (blob_size >= 4 && memcmp(blob, "OggS", 4) == 0) {
            /* Already compressed - ship as-is, device decodes with stb_vorbis. */
            fwrite(blob, 1, blob_size, out);
            out_bytes = blob_size;
            flags = FLAG_VERBATIM;
            copy_verbatim = 1;
            n_copy++;
        } else {
            uint32_t rate = 0, dsize = 0;
            uint16_t ch = 0, bps = 0;
            const uint8_t *pcm = NULL;
            int r = parse_wav(blob, blob_size, &rate, &ch, &bps, &pcm, &dsize);
            if (r != 0 || ch < 1 || ch > 2 || bps != 16 || rate == 0) {
                fprintf(stderr, "  entry %u: unsupported audio (r=%d) size=%u\n", i, r, blob_size);
                n_bad++;
                continue;
            }
            orig_rate = rate;
            channels = 1;
            bits = 16;

            /* downmix + decimate through the FIR */
            num_samples = resample_mono(pcm, dsize, ch, rate,
                                        target_rate, mono, 4u * 1024 * 1024);
            total_in += dsize / (2u * ch);
            total_out += num_samples;

            if (use_adpcm) {
                /* 4-byte block header + packed nibbles, 2048 samples per block */
                uint32_t written = adpcm_encode(mono, (int)num_samples, adpcm);
                fwrite(adpcm, 1, written, out);
                out_bytes = written;
                block_bytes = BLOCK_BYTES;
                flags = FLAG_ADPCM;
                n_adpcm++;
            } else {
                fwrite(mono, 1, (size_t)num_samples * 2, out);
                out_bytes = num_samples * 2;
                block_bytes = 0;
                flags = FLAG_PCM;
                n_pcm++;
            }
        }

        if (out_bytes & 1) { fputc(0, out); out_bytes++; }   /* keep 2-byte align */
        uint32_t rec[8] = {0};
        rec[0] = cursor;                       /* dataOffset */
        rec[1] = out_bytes;                    /* dataSize */
        rec[2] = copy_verbatim ? 0 : orig_rate;/* originalRate */
        rec[3] = num_samples;                  /* numSamples */
        ((uint16_t *)rec)[8]  = channels;      /* channels */
        ((uint16_t *)rec)[9]  = flags;         /* flags: 1=ADPCM */
        ((uint16_t *)rec)[10] = bits;
        ((uint16_t *)rec)[11] = block_bytes;
        memcpy(index + i * INDEX_SIZE, rec, INDEX_SIZE);
        cursor += out_bytes;
    }

    /* patch up the header + index now that offsets are known */
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, SNDS_MAGIC, 4);
    ((uint32_t *)hdr)[1] = SNDS_VERSION;
    ((uint32_t *)hdr)[2] = count;
    ((uint32_t *)hdr)[3] = 32;
    ((uint32_t *)hdr)[4] = data_start;
    ((uint32_t *)hdr)[5] = target_rate;
    ((uint32_t *)hdr)[6] = BLOCK_SAMPLES;
    fseek(out, 0, SEEK_SET);
    fwrite(hdr, 1, sizeof(hdr), out);
    fwrite(index, 1, (size_t)INDEX_SIZE * count, out);
    long final_size = (long)cursor;
    fclose(out);
    printf("entries      : %u  (pcm %u, adpcm %u, verbatim %u, skipped %u)\n",
           count, n_pcm, n_adpcm, n_copy, n_bad);
    printf("mode         : %s\n", g_adpcm ? "IMA ADPCM 4:1" : "PCM16 mono 22050");
    printf("input frames : %llu (%.2f MB PCM)\n", (unsigned long long)total_in,
           total_in * 2.0 / 1048576.0);
    printf("output samp  : %llu @ %u Hz (%.2f min)\n", (unsigned long long)total_out,
           target_rate, total_out / (double)target_rate / 60.0);
    printf("payload      : %.2f MB\n", (final_size - (long)data_start) / 1048576.0);

    free(data); free(ptrs); free(mono); free(hist); free(adpcm);
    return 0;
}
