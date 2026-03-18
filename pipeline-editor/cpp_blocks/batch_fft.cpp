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
//             Sym 0 = SIGNAL (preamble already stripped upstream)
//
// Outputs:
//   out[0]: Stacked frequency-domain IQ blocks    (129024 bytes/pkt)
//             504 blocks x 256 bytes each
//             Block 0   = SIGNAL subcarriers
//             Blocks 1+ = DATA  subcarriers
//
//   out[1]: Scatter plot data  (200 MB pipe, variable fill)
// ============================================================

static const bool SCATTER_ENABLED  = true;
static const int  SCATTER_PACKETS  = 1;
static const bool PLOT_SIGNAL      = true;
static const bool PLOT_DATA        = true;
static const bool PLOT_PILOTS      = false;
static const bool PLOT_NULLS       = false;

static const int SYMBOLS_PER_PKT  = 504;
static const int SUBCARRIERS      = 64;
static const int CP_LENGTH        = 16;
static const int SAMPLES_PER_SYM  = SUBCARRIERS + CP_LENGTH;              // 80
static const int BYTES_PER_SAMPLE = 4;
static const int BYTES_PER_SYM    = SAMPLES_PER_SYM * BYTES_PER_SAMPLE;  // 320
static const int BYTES_PER_BLOCK  = SUBCARRIERS * 4;                      // 256
static const int IN_PKT_SIZE      = SYMBOLS_PER_PKT * BYTES_PER_SYM;     // 161280
static const int OUT_PKT_SIZE     = SYMBOLS_PER_PKT * BYTES_PER_BLOCK;   // 129024
static const int SCATTER_PIPE_BYTES = 209715200;

static bool g_isDataSC[64];
static bool g_isPilotSC[64];

static void buildSubcarrierClassification() {
    memset(g_isDataSC,  false, sizeof(g_isDataSC));
    memset(g_isPilotSC, false, sizeof(g_isPilotSC));
    const int dataFreqs[] = {
        -26,-25,-24,-23,-22,
        -20,-19,-18,-17,-16,-15,-14,-13,-12,-11,-10,-9,-8,
        -6,-5,-4,-3,-2,-1,
        1,2,3,4,5,6,
        8,9,10,11,12,13,14,15,16,17,18,19,20,
        22,23,24,25,26
    };
    for (int f : dataFreqs) g_isDataSC[f + 32] = true;
    const int pilotFreqs[] = {-21, -7, 7, 21};
    for (int f : pilotFreqs) g_isPilotSC[f + 32] = true;
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

static void fft64(double* re, double* im) {
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
        double ang = -2.0 * M_PI / len;
        double wRe = cos(ang), wIm = sin(ang);
        for (int i = 0; i < N; i += len) {
            double curRe = 1.0, curIm = 0.0;
            for (int j = 0; j < len / 2; j++) {
                double uRe = re[i+j], uIm = im[i+j];
                double vRe = re[i+j+len/2]*curRe - im[i+j+len/2]*curIm;
                double vIm = re[i+j+len/2]*curIm + im[i+j+len/2]*curRe;
                re[i+j]       = uRe+vRe; im[i+j]       = uIm+vIm;
                re[i+j+len/2] = uRe-vRe; im[i+j+len/2] = uIm-vIm;
                double nRe = curRe*wRe - curIm*wIm;
                double nIm = curRe*wIm + curIm*wRe;
                curRe = nRe; curIm = nIm;
            }
        }
    }
}

static void processSymbol(const int8_t* timeSym, int8_t* freqBlock) {
    // Skip cyclic prefix
    const int8_t* payload = timeSym + CP_LENGTH * BYTES_PER_SAMPLE;
    double re[64], im[64];
    for (int n = 0; n < SUBCARRIERS; n++) unpackIQ(payload + n*4, re[n], im[n]);
    fft64(re, im);
    // fftshift: swap lower/upper halves for natural frequency ordering
    double shiftedRe[64], shiftedIm[64];
    for (int k = 0; k < 32; k++) {
        shiftedRe[k]    = re[k+32]; shiftedIm[k]    = im[k+32];
        shiftedRe[k+32] = re[k];    shiftedIm[k+32] = im[k];
    }
    for (int k = 0; k < SUBCARRIERS; k++)
        packIQ(freqBlock + k*4, shiftedRe[k], shiftedIm[k]);
}

static int appendToScatter(
    const int8_t* freqBlock, int8_t* scatterDst,
    int bytesWritten, int maxBytes, int symIdx
) {
    bool includeBlock = (symIdx == 0) ? PLOT_SIGNAL : PLOT_DATA;
    if (!includeBlock) return 0;
    int added = 0;
    for (int sc = 0; sc < SUBCARRIERS; sc++) {
        bool isData  = g_isDataSC[sc];
        bool isPilot = g_isPilotSC[sc];
        bool isNull  = !isData && !isPilot;
        bool include = false;
        if (isData)  include = true;
        if (isPilot) include = PLOT_PILOTS;
        if (isNull)  include = PLOT_NULLS;
        if (include) {
            if (bytesWritten + added + 4 > maxBytes) break;
            memcpy(scatterDst + bytesWritten + added, freqBlock + sc*4, 4);
            added += 4;
        }
    }
    return added;
}

struct BatchFftData { int frameCount; };

BatchFftData init_batch_fft(const BlockConfig& config) {
    BatchFftData data;
    data.frameCount = 0;
    buildSubcarrierClassification();
    return data;
}

void process_batch_fft(
    const char**    pipeIn,
    const char**    pipeOut,
    BatchFftData&   customData,
    const BlockConfig& config
) {
    PipeIO inTime    (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO outFreq   (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outScatter(pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);

    int8_t* timeBuf = new int8_t[inTime.getBufferSize()];
    int8_t* freqBuf = new int8_t[outFreq.getBufferSize()];

    const int inPkt      = config.inputPacketSizes[0];
    const int outPkt     = config.outputPacketSizes[0];
    const int scatterPkt = config.outputPacketSizes[1];

    const bool isFirstBatch = (customData.frameCount == 0);

    int actualCount = inTime.read(timeBuf);

    memset(freqBuf, 0x80, outFreq.getBufferSize());

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
        const int8_t* pktTime = timeBuf + i * inPkt;
        int8_t*       pktFreq = freqBuf + i * outPkt;

        for (int sym = 0; sym < SYMBOLS_PER_PKT; sym++) {
            processSymbol(pktTime + sym * BYTES_PER_SYM,
                          pktFreq + sym * BYTES_PER_BLOCK);

            if (SCATTER_ENABLED && i < SCATTER_PACKETS) {
                scatterDataBytes += appendToScatter(
                    pktFreq + sym * BYTES_PER_BLOCK,
                    scatterDataStart, scatterDataBytes, scatterMaxBytes, sym);
                if (scatterDataBytes >= scatterMaxBytes) break;
            }
        }

        // Read actual symbol count embedded in last 4 bytes of input
        uint32_t actualSyms = 0;
        for (int j = 0; j < 4; j++)
            actualSyms |= ((uint32_t)(uint8_t)((int32_t)pktTime[inPkt - 4 + j] + 128)) << (j * 8);
        if (actualSyms == 0 || actualSyms > (uint32_t)SYMBOLS_PER_PKT)
            actualSyms = SYMBOLS_PER_PKT;

        if (isFirstBatch && i == 0) {
            printf("[BatchFft] pkt[0] INPUT : %d bits (%u syms x %d bytes)\n",
                   (int)actualSyms * BYTES_PER_SYM * 8, actualSyms, BYTES_PER_SYM);
            printf("[BatchFft] pkt[0] OUTPUT: %d bits (%u freq blocks x 256 bytes)\n",
                   (int)actualSyms * BYTES_PER_BLOCK * 8, actualSyms);
            fflush(stdout);
        }
    }

    outFreq.write(freqBuf, actualCount);

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

    delete[] timeBuf;
    delete[] freqBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: batch_fft <pipeInTime> <pipeOutFreq> <pipeOutScatter>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1]};
    const char* pipeOuts[] = {argv[2], argv[3]};

    BlockConfig config = {
        "BatchFft",
        1,                            // inputs
        2,                            // outputs
        {161280},                     // inputPacketSizes
        {64},                         // inputBatchSizes
        {129024, 209715200},          // outputPacketSizes [freq, scatter 200MB]
        {64, 1},                      // outputBatchSizes
        false,                        // ltr
        true,                         // startWithAll
        "IEEE 802.11a Batch FFT with scatter output"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_batch_fft, init_batch_fft);
    return 0;
}