#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// IEEE 802.11a Batch IFFT (Frequency -> Time Domain)
//
// Inputs:
//   in[0]: Stacked frequency-domain IQ blocks   (130048 bytes/pkt)
//             508 blocks x 256 bytes each
//             Each block: 64 subcarriers x 4 bytes [I_lo,I_hi,Q_lo,Q_hi]
//             Subcarrier order: index -32..+31 mapped to array [0..63]
//
// Outputs:
//   out[0]: Stacked time-domain samples          (162560 bytes/pkt)
//             508 symbols x 320 bytes each (80 samples x 4 bytes)
//             Each symbol: 16-sample CP + 64-sample IFFT output
//             Format: int16 I,Q little-endian with B-128 protocol offset
//
// Processing per block:
//   1. ifftshift: rearrange so DC (index 32) moves to position 0
//      -> subcarrier order becomes [0..31, -32..-1]
//   2. 64-point IFFT, normalized by 1/N
//   3. Cyclic prefix: prepend last 16 samples -> 80 samples total
//   4. Scale by 32767, round to int16, apply B-128 offset
//
// Normalization convention: IFFT divides by N, FFT does not.
//
// int8 pipe convention: stored byte B -> pipe int8_t = int8_t(uint8_t(B) - 128)
// ============================================================

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------
static const int BLOCKS_PER_PKT   = 508;
static const int SUBCARRIERS      = 64;
static const int BYTES_PER_BLOCK  = SUBCARRIERS * 4;              // 256
static const int IN_PKT_SIZE      = BLOCKS_PER_PKT * BYTES_PER_BLOCK; // 130048
static const int CP_LENGTH        = 16;
static const int SAMPLES_PER_SYM  = SUBCARRIERS + CP_LENGTH;      // 80
static const int BYTES_PER_SAMPLE = 4;
static const int BYTES_PER_SYM    = SAMPLES_PER_SYM * BYTES_PER_SAMPLE; // 320
static const int OUT_PKT_SIZE     = BLOCKS_PER_PKT * BYTES_PER_SYM;    // 162560
static const int BATCH_SIZE       = 64;

// -----------------------------------------------------------------------
// Unpack one IQ pair from 4 pipe bytes -> double
// -----------------------------------------------------------------------
static void unpackIQ(const int8_t* src, double& I, double& Q) {
    uint8_t iLo = (uint8_t)((int32_t)src[0] + 128);
    uint8_t iHi = (uint8_t)((int32_t)src[1] + 128);
    uint8_t qLo = (uint8_t)((int32_t)src[2] + 128);
    uint8_t qHi = (uint8_t)((int32_t)src[3] + 128);
    I = (double)(int16_t)((uint16_t)iLo | ((uint16_t)iHi << 8)) / 32767.0;
    Q = (double)(int16_t)((uint16_t)qLo | ((uint16_t)qHi << 8)) / 32767.0;
}

// -----------------------------------------------------------------------
// Pack one IQ pair into 4 pipe bytes
// -----------------------------------------------------------------------
static void packIQ(int8_t* dst, double I, double Q) {
    if (I >  1.0) I =  1.0;
    if (I < -1.0) I = -1.0;
    if (Q >  1.0) Q =  1.0;
    if (Q < -1.0) Q = -1.0;
    int16_t iVal = (int16_t)round(I * 32767.0);
    int16_t qVal = (int16_t)round(Q * 32767.0);
    uint16_t iu = (uint16_t)iVal;
    uint16_t qu = (uint16_t)qVal;
    dst[0] = (int8_t)((int32_t)(uint8_t)(iu & 0xFF)        - 128);
    dst[1] = (int8_t)((int32_t)(uint8_t)((iu >> 8) & 0xFF) - 128);
    dst[2] = (int8_t)((int32_t)(uint8_t)(qu & 0xFF)        - 128);
    dst[3] = (int8_t)((int32_t)(uint8_t)((qu >> 8) & 0xFF) - 128);
}

// -----------------------------------------------------------------------
// 64-point IFFT (Cooley-Tukey DIT, in-place, complex)
// Input/output: re[], im[] of length N=64
// Normalized by 1/N so that IFFT(FFT(x)) = x
// -----------------------------------------------------------------------
static void ifft64(double* re, double* im) {
    const int N = 64;

    // Bit-reversal permutation
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double tmp;
            tmp = re[i]; re[i] = re[j]; re[j] = tmp;
            tmp = im[i]; im[i] = im[j]; im[j] = tmp;
        }
    }

    // Butterfly stages (positive angle twiddle for IFFT)
    for (int len = 2; len <= N; len <<= 1) {
        double ang = 2.0 * M_PI / len;
        double wRe = cos(ang);
        double wIm = sin(ang);
        for (int i = 0; i < N; i += len) {
            double curRe = 1.0, curIm = 0.0;
            for (int j = 0; j < len / 2; j++) {
                double uRe = re[i + j];
                double uIm = im[i + j];
                double vRe = re[i + j + len/2] * curRe - im[i + j + len/2] * curIm;
                double vIm = re[i + j + len/2] * curIm + im[i + j + len/2] * curRe;
                re[i + j]         = uRe + vRe;
                im[i + j]         = uIm + vIm;
                re[i + j + len/2] = uRe - vRe;
                im[i + j + len/2] = uIm - vIm;
                double newCurRe = curRe * wRe - curIm * wIm;
                double newCurIm = curRe * wIm + curIm * wRe;
                curRe = newCurRe;
                curIm = newCurIm;
            }
        }
    }

    // Normalize by 1/N
    for (int i = 0; i < N; i++) {
        re[i] /= N;
        im[i] /= N;
    }
}

// -----------------------------------------------------------------------
// Process one frequency block -> one time symbol with CP
//
// Input:  256-byte pipe block (64 subcarriers, index -32..+31 -> [0..63])
// Output: 320-byte pipe symbol (80 samples: 16-sample CP + 64-sample IFFT)
// -----------------------------------------------------------------------
static void processBlock(const int8_t* freqBlock, int8_t* timeSym) {
    // Step 1: Unpack frequency domain
    // Input array order: [0]=subcarrier -32, ..., [63]=subcarrier +31
    double re[64], im[64];
    for (int k = 0; k < 64; k++) {
        unpackIQ(freqBlock + k * 4, re[k], im[k]);
    }

    // Step 2: ifftshift — DC (currently at index 32) moves to index 0
    // Resulting order: [array[32..63], array[0..31]]
    double shiftedRe[64], shiftedIm[64];
    for (int k = 0; k < 32; k++) {
        shiftedRe[k]    = re[k + 32];
        shiftedIm[k]    = im[k + 32];
        shiftedRe[k+32] = re[k];
        shiftedIm[k+32] = im[k];
    }

    // Step 3: 64-point IFFT
    ifft64(shiftedRe, shiftedIm);

    // Step 4: Cyclic prefix — copy last 16 samples to front
    // timeSym[0..15]  = CP  (samples 48..63 of IFFT output)
    // timeSym[16..79] = IFFT output samples 0..63
    for (int n = 0; n < CP_LENGTH; n++) {
        packIQ(timeSym + n * 4,
               shiftedRe[SUBCARRIERS - CP_LENGTH + n],
               shiftedIm[SUBCARRIERS - CP_LENGTH + n]);
    }
    for (int n = 0; n < SUBCARRIERS; n++) {
        packIQ(timeSym + (CP_LENGTH + n) * 4, shiftedRe[n], shiftedIm[n]);
    }
}

// -----------------------------------------------------------------------
// Block state
// -----------------------------------------------------------------------
struct BatchIfftData {
    int frameCount;
};

BatchIfftData init_batch_ifft(const BlockConfig& config) {
    BatchIfftData data;
    data.frameCount = 0;
    return data;
}

void process_batch_ifft(
    const char**    pipeIn,
    const char**    pipeOut,
    BatchIfftData&  customData,
    const BlockConfig& config
) {
    PipeIO inFreq (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO outTime(pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);

    int8_t* freqBuf = new int8_t[inFreq.getBufferSize()];
    int8_t* timeBuf = new int8_t[outTime.getBufferSize()];

    const int inPkt  = config.inputPacketSizes[0];
    const int outPkt = config.outputPacketSizes[0];

    int actualCount = inFreq.read(freqBuf);

    memset(timeBuf, 0x80, outTime.getBufferSize());

    for (int i = 0; i < actualCount; i++) {
        const int8_t* pktFreq = freqBuf + i * inPkt;
        int8_t*       pktTime = timeBuf + i * outPkt;

        for (int blk = 0; blk < BLOCKS_PER_PKT; blk++) {
            processBlock(pktFreq + blk * BYTES_PER_BLOCK,
                         pktTime + blk * BYTES_PER_SYM);
        }
    }

    outTime.write(timeBuf, actualCount);
    customData.frameCount += actualCount;

    delete[] freqBuf;
    delete[] timeBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: batch_ifft <pipeInFreq> <pipeOutTime>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1]};
    const char* pipeOuts[] = {argv[2]};

    BlockConfig config = {
        "BatchIfft",
        1,           // inputs
        1,           // outputs
        {130048},    // inputPacketSizes  [508 freq blocks * 256 bytes]
        {64},        // inputBatchSizes
        {162560},    // outputPacketSizes [508 time symbols * 320 bytes]
        {64},        // outputBatchSizes
        false,       // ltr
        true,        // startWithAll
        "IEEE 802.11a Batch IFFT: frequency-domain IQ -> time-domain samples with CP"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_batch_ifft, init_batch_ifft);
    return 0;
}