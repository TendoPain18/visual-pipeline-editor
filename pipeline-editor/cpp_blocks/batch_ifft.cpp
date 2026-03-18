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
//
// Outputs:
//   out[0]: Stacked time-domain samples          (162560 bytes/pkt)
//             508 symbols x 320 bytes each (80 samples x 4 bytes)
//             Each symbol = 16-sample CP + 64-sample IFFT output
//
//   out[1]: Scatter plot data  (200 MB pipe, variable fill)
// ============================================================

static const bool SCATTER_ENABLED  = true;
static const int  SCATTER_PACKETS  = 1;
static const bool PLOT_STS         = false;
static const bool PLOT_LTS         = false;
static const bool PLOT_SIGNAL      = false;
static const bool PLOT_DATA        = true;

static const int BLOCKS_PER_PKT   = 508;
static const int SUBCARRIERS      = 64;
static const int BYTES_PER_BLOCK  = SUBCARRIERS * 4;                   // 256
static const int IN_PKT_SIZE      = BLOCKS_PER_PKT * BYTES_PER_BLOCK;  // 130048
static const int CP_LENGTH        = 16;
static const int SAMPLES_PER_SYM  = SUBCARRIERS + CP_LENGTH;           // 80
static const int BYTES_PER_SAMPLE = 4;
static const int BYTES_PER_SYM    = SAMPLES_PER_SYM * BYTES_PER_SAMPLE; // 320
static const int OUT_PKT_SIZE     = BLOCKS_PER_PKT * BYTES_PER_SYM;    // 162560
static const int SCATTER_PIPE_BYTES = 209715200;

static const int STS_BLOCK_START  = 0;
static const int STS_BLOCK_END    = 1;
static const int LTS_BLOCK_START  = 2;
static const int LTS_BLOCK_END    = 3;
static const int SIGNAL_BLOCK_IDX = 4;
static const int DATA_BLOCK_START = 5;

static bool shouldPlotBlock(int blkIdx) {
    if (blkIdx >= STS_BLOCK_START && blkIdx <= STS_BLOCK_END)   return PLOT_STS;
    if (blkIdx >= LTS_BLOCK_START && blkIdx <= LTS_BLOCK_END)   return PLOT_LTS;
    if (blkIdx == SIGNAL_BLOCK_IDX)                              return PLOT_SIGNAL;
    if (blkIdx >= DATA_BLOCK_START)                              return PLOT_DATA;
    return false;
}

static void unpackIQ(const int8_t* src, double& I, double& Q) {
    uint8_t iLo = (uint8_t)((int32_t)src[0] + 128);
    uint8_t iHi = (uint8_t)((int32_t)src[1] + 128);
    uint8_t qLo = (uint8_t)((int32_t)src[2] + 128);
    uint8_t qHi = (uint8_t)((int32_t)src[3] + 128);
    I = (double)(int16_t)((uint16_t)iLo | ((uint16_t)iHi << 8)) / 32767.0;
    Q = (double)(int16_t)((uint16_t)qLo | ((uint16_t)qHi << 8)) / 32767.0;
}

static void packIQ(int8_t* dst, double I, double Q) {
    if (I >  1.0) I =  1.0; if (I < -1.0) I = -1.0;
    if (Q >  1.0) Q =  1.0; if (Q < -1.0) Q = -1.0;
    int16_t iVal = (int16_t)round(I * 32767.0);
    int16_t qVal = (int16_t)round(Q * 32767.0);
    uint16_t iu = (uint16_t)iVal, qu = (uint16_t)qVal;
    dst[0] = (int8_t)((int32_t)(uint8_t)(iu & 0xFF)        - 128);
    dst[1] = (int8_t)((int32_t)(uint8_t)((iu >> 8) & 0xFF) - 128);
    dst[2] = (int8_t)((int32_t)(uint8_t)(qu & 0xFF)        - 128);
    dst[3] = (int8_t)((int32_t)(uint8_t)((qu >> 8) & 0xFF) - 128);
}

static void ifft64(double* re, double* im) {
    const int N = 64;
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
    for (int len = 2; len <= N; len <<= 1) {
        double ang = 2.0 * M_PI / len;
        double wRe = cos(ang), wIm = sin(ang);
        for (int i = 0; i < N; i += len) {
            double curRe = 1.0, curIm = 0.0;
            for (int j = 0; j < len / 2; j++) {
                double uRe = re[i+j], uIm = im[i+j];
                double vRe = re[i+j+len/2]*curRe - im[i+j+len/2]*curIm;
                double vIm = re[i+j+len/2]*curIm + im[i+j+len/2]*curRe;
                re[i+j]         = uRe+vRe; im[i+j]         = uIm+vIm;
                re[i+j+len/2]   = uRe-vRe; im[i+j+len/2]   = uIm-vIm;
                double nRe = curRe*wRe - curIm*wIm;
                double nIm = curRe*wIm + curIm*wRe;
                curRe = nRe; curIm = nIm;
            }
        }
    }
    for (int i = 0; i < N; i++) { re[i] /= N; im[i] /= N; }
}

static void processBlock(const int8_t* freqBlock, int8_t* timeSym) {
    double re[64], im[64];
    for (int k = 0; k < 64; k++) unpackIQ(freqBlock + k*4, re[k], im[k]);

    // ifftshift: swap lower/upper halves before IFFT
    double shiftedRe[64], shiftedIm[64];
    for (int k = 0; k < 32; k++) {
        shiftedRe[k]    = re[k+32]; shiftedIm[k]    = im[k+32];
        shiftedRe[k+32] = re[k];    shiftedIm[k+32] = im[k];
    }
    ifft64(shiftedRe, shiftedIm);

    // Prepend cyclic prefix (last CP_LENGTH samples of IFFT output)
    for (int n = 0; n < CP_LENGTH; n++)
        packIQ(timeSym + n*4,
               shiftedRe[SUBCARRIERS - CP_LENGTH + n],
               shiftedIm[SUBCARRIERS - CP_LENGTH + n]);
    // Followed by the full IFFT output
    for (int n = 0; n < SUBCARRIERS; n++)
        packIQ(timeSym + (CP_LENGTH + n)*4, shiftedRe[n], shiftedIm[n]);
}

struct BatchIfftData { int frameCount; };

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
    PipeIO inFreq    (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO outTime   (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outScatter(pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);

    int8_t* freqBuf = new int8_t[inFreq.getBufferSize()];
    int8_t* timeBuf = new int8_t[outTime.getBufferSize()];

    const int inPkt      = config.inputPacketSizes[0];
    const int outPkt     = config.outputPacketSizes[0];
    const int scatterPkt = config.outputPacketSizes[1];

    const bool isFirstBatch = (customData.frameCount == 0);

    int actualCount = inFreq.read(freqBuf);

    memset(timeBuf, 0x80, outTime.getBufferSize());

    // Scatter buffers — only allocated when SCATTER_ENABLED
    int8_t* scatterBuf       = nullptr;
    int8_t* scatterPkt0      = nullptr;
    int8_t* scatterDataStart = nullptr;
    int     scatterDataBytes = 0;
    int     scatterMaxBytes  = 0;

    if (SCATTER_ENABLED) {
        scatterBuf       = new int8_t[outScatter.getBufferSize()];
        memset(scatterBuf, 0x80, outScatter.getBufferSize());
        scatterPkt0      = scatterBuf + calculateLengthBytes(1);
        scatterDataStart = scatterPkt0 + 4;
        scatterMaxBytes  = scatterPkt - 4;
    }

    for (int i = 0; i < actualCount; i++) {
        const int8_t* pktFreq = freqBuf + i * inPkt;
        int8_t*       pktTime = timeBuf + i * outPkt;

        for (int blk = 0; blk < BLOCKS_PER_PKT; blk++) {
            processBlock(pktFreq + blk * BYTES_PER_BLOCK,
                         pktTime + blk * BYTES_PER_SYM);

            if (SCATTER_ENABLED && i < SCATTER_PACKETS && shouldPlotBlock(blk)) {
                const int8_t* timeSym = pktTime + blk * BYTES_PER_SYM;
                int bytesToAdd = BYTES_PER_SYM;
                if (scatterDataBytes + bytesToAdd > scatterMaxBytes)
                    bytesToAdd = scatterMaxBytes - scatterDataBytes;
                if (bytesToAdd > 0) {
                    memcpy(scatterDataStart + scatterDataBytes, timeSym, bytesToAdd);
                    scatterDataBytes += bytesToAdd;
                }
            }
        }

        // Read actual block count embedded by qam_mapper in last 4 bytes
        uint32_t actualBlocks = 0;
        for (int j = 0; j < 4; j++)
            actualBlocks |= ((uint32_t)(uint8_t)((int32_t)pktFreq[inPkt - 4 + j] + 128)) << (j * 8);
        if (actualBlocks == 0 || actualBlocks > (uint32_t)BLOCKS_PER_PKT)
            actualBlocks = BLOCKS_PER_PKT;

        // Pass actual count forward in last 4 bytes of output (zero-padded region)
        for (int j = 0; j < 4; j++) {
            uint8_t b = (uint8_t)((actualBlocks >> (j * 8)) & 0xFF);
            pktTime[outPkt - 4 + j] = (int8_t)((int32_t)b - 128);
        }

        if (isFirstBatch && i == 0) {
            printf("[BatchIfft] pkt[0] INPUT : %d bits (%u freq blocks x 256 bytes)\n",
                   (int)actualBlocks * BYTES_PER_BLOCK * 8, actualBlocks);
            printf("[BatchIfft] pkt[0] OUTPUT: %d bits (%u syms x %d bytes)\n",
                   (int)actualBlocks * BYTES_PER_SYM * 8, actualBlocks, BYTES_PER_SYM);
            fflush(stdout);
        }
    }

    outTime.write(timeBuf, actualCount);

    // Only write to the scatter pipe when SCATTER_ENABLED=true.
    // When false, nothing is written — scatter_plot blocks waiting, which is intended.
    if (SCATTER_ENABLED) {
        uint32_t sz = (uint32_t)scatterDataBytes;
        uint8_t* hdr = (uint8_t*)scatterPkt0;
        hdr[0] = (uint8_t)( sz        & 0xFF);
        hdr[1] = (uint8_t)((sz >>  8) & 0xFF);
        hdr[2] = (uint8_t)((sz >> 16) & 0xFF);
        hdr[3] = (uint8_t)((sz >> 24) & 0xFF);
        scatterBuf[0] = (int8_t)((int32_t)(uint8_t)1 - 128);  // batch count = 1
        outScatter.write(scatterBuf, 1);
        delete[] scatterBuf;
    }

    customData.frameCount += actualCount;

    delete[] freqBuf;
    delete[] timeBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: batch_ifft <pipeInFreq> <pipeOutTime> <pipeOutScatter>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1]};
    const char* pipeOuts[] = {argv[2], argv[3]};

    BlockConfig config = {
        "BatchIfft",
        1,                            // inputs
        2,                            // outputs
        {130048},                     // inputPacketSizes
        {64},                         // inputBatchSizes
        {162560, 209715200},          // outputPacketSizes [time, scatter 200MB]
        {64, 1},                      // outputBatchSizes
        false,                        // ltr
        true,                         // startWithAll
        "IEEE 802.11a Batch IFFT with scatter output"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_batch_ifft, init_batch_ifft);
    return 0;
}