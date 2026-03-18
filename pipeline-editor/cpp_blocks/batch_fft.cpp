#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// IEEE 802.11a Batch FFT (Time -> Frequency Domain)
//
// Inputs:
//   in[0]: Stripped time-domain samples           (161280 bytes/pkt)
//             504 symbols x 320 bytes each (80 samples x 4 bytes)
//             First symbol in packet = SIGNAL
//
// Outputs:
//   out[0]: Stacked frequency-domain IQ blocks    (129024 bytes/pkt)
//             504 blocks x 256 bytes each
//             Each block: 64 subcarriers x 4 bytes [I_lo,I_hi,Q_lo,Q_hi]
//             Subcarrier order: index -32..+31 -> array [0..63]
//             Block 0   = SIGNAL (BPSK)
//             Blocks 1+ = DATA
//
// Processing per 80-sample symbol:
//   1. CP Removal: discard first 16 samples (cyclic prefix)
//   2. 64-point FFT on remaining 64 samples (unnormalized)
//   3. fftshift: restore subcarrier order to [-32..+31] -> array [0..63]
//   4. Scale by 32767, round to int16, apply B-128 offset
//
// Normalization convention: IFFT divides by N, FFT does not.
// This preserves the original subcarrier amplitudes through the round-trip.
//
// int8 pipe convention: stored byte B -> pipe int8_t = int8_t(uint8_t(B) - 128)
// ============================================================

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------
static const int SYMBOLS_PER_PKT  = 504;
static const int SUBCARRIERS      = 64;
static const int CP_LENGTH        = 16;
static const int SAMPLES_PER_SYM  = SUBCARRIERS + CP_LENGTH;              // 80
static const int BYTES_PER_SAMPLE = 4;
static const int BYTES_PER_SYM    = SAMPLES_PER_SYM * BYTES_PER_SAMPLE;  // 320
static const int BYTES_PER_BLOCK  = SUBCARRIERS * 4;                      // 256
static const int IN_PKT_SIZE      = SYMBOLS_PER_PKT * BYTES_PER_SYM;     // 161280
static const int OUT_PKT_SIZE     = SYMBOLS_PER_PKT * BYTES_PER_BLOCK;   // 129024
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
// 64-point FFT (Cooley-Tukey DIT, in-place, complex)
// Input/output: re[], im[] of length N=64
// Unnormalized — only IFFT divides by N to preserve round-trip amplitudes.
// -----------------------------------------------------------------------
static void fft64(double* re, double* im) {
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

    // Butterfly stages (negative angle twiddle for FFT)
    for (int len = 2; len <= N; len <<= 1) {
        double ang = -2.0 * M_PI / len;
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
    // FFT is unnormalized (only IFFT divides by N)
}

// -----------------------------------------------------------------------
// Process one time symbol -> one frequency block
//
// Input:  320-byte pipe symbol (80 samples: first 16 = CP, next 64 = payload)
// Output: 256-byte pipe block  (64 subcarriers, index -32..+31 -> [0..63])
// -----------------------------------------------------------------------
static void processSymbol(const int8_t* timeSym, int8_t* freqBlock) {
    // Step 1: CP removal — skip first 16 samples (64 bytes)
    const int8_t* payload = timeSym + CP_LENGTH * BYTES_PER_SAMPLE;

    // Step 2: Unpack 64 time-domain samples
    double re[64], im[64];
    for (int n = 0; n < SUBCARRIERS; n++) {
        unpackIQ(payload + n * 4, re[n], im[n]);
    }

    // Step 3: 64-point FFT
    // Output order: [0, 1, ..., 31, 32, ..., 63]
    // Corresponds to subcarriers [0, 1, ..., 31, -32, ..., -1]
    fft64(re, im);

    // Step 4: fftshift — rearrange to subcarrier order -32..+31 -> array [0..63]
    // output[0..31]  = fft[32..63]  (subcarriers -32..-1)
    // output[32..63] = fft[0..31]   (subcarriers 0..+31)
    double shiftedRe[64], shiftedIm[64];
    for (int k = 0; k < 32; k++) {
        shiftedRe[k]    = re[k + 32];
        shiftedIm[k]    = im[k + 32];
        shiftedRe[k+32] = re[k];
        shiftedIm[k+32] = im[k];
    }

    // Step 5: Pack into output block
    for (int k = 0; k < SUBCARRIERS; k++) {
        packIQ(freqBlock + k * 4, shiftedRe[k], shiftedIm[k]);
    }
}

// -----------------------------------------------------------------------
// Block state
// -----------------------------------------------------------------------
struct BatchFftData {
    int frameCount;
};

BatchFftData init_batch_fft(const BlockConfig& config) {
    BatchFftData data;
    data.frameCount = 0;
    return data;
}

void process_batch_fft(
    const char**    pipeIn,
    const char**    pipeOut,
    BatchFftData&   customData,
    const BlockConfig& config
) {
    PipeIO inTime (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO outFreq(pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);

    int8_t* timeBuf = new int8_t[inTime.getBufferSize()];
    int8_t* freqBuf = new int8_t[outFreq.getBufferSize()];

    const int inPkt  = config.inputPacketSizes[0];
    const int outPkt = config.outputPacketSizes[0];

    int actualCount = inTime.read(timeBuf);

    memset(freqBuf, 0x80, outFreq.getBufferSize());

    for (int i = 0; i < actualCount; i++) {
        const int8_t* pktTime = timeBuf + i * inPkt;
        int8_t*       pktFreq = freqBuf + i * outPkt;

        for (int sym = 0; sym < SYMBOLS_PER_PKT; sym++) {
            processSymbol(pktTime + sym * BYTES_PER_SYM,
                          pktFreq + sym * BYTES_PER_BLOCK);
        }
    }

    outFreq.write(freqBuf, actualCount);
    customData.frameCount += actualCount;

    delete[] timeBuf;
    delete[] freqBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: batch_fft <pipeInTime> <pipeOutFreq>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1]};
    const char* pipeOuts[] = {argv[2]};

    BlockConfig config = {
        "BatchFft",
        1,           // inputs
        1,           // outputs
        {161280},    // inputPacketSizes  [504 symbols * 320 bytes]
        {64},        // inputBatchSizes
        {129024},    // outputPacketSizes [504 freq blocks * 256 bytes]
        {64},        // outputBatchSizes
        false,       // ltr
        true,        // startWithAll
        "IEEE 802.11a Batch FFT: stripped time-domain samples -> frequency-domain IQ blocks"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_batch_fft, init_batch_fft);
    return 0;
}