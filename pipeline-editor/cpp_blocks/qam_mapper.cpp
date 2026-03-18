#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdio>

// ============================================================
// IEEE 802.11a QAM Mapper (Frequency Domain)
//
// Inputs:
//   in[0]: Interleaved DATA from interleaver    (3030 bytes/pkt)
//   in[1]: rate_encoded_length from interleaver (5 bytes/pkt)
//             Byte 0: rate_value
//             Bytes 1-4: encoded_len_bits (uint32 LE) -- EXACT encoded bits
//
// Outputs:
//   out[0]: Stacked frequency-domain IQ blocks  (130048 bytes/pkt)
//             508 blocks x 256 bytes each (2xSTS + 2xLTS + SIGNAL + DATA)
//   out[1]: Scatter plot data  (200 MB pipe, variable fill)
//
// Block layout in output packet:
//   Block 0–1 : STS (Short Training Sequence)
//   Block 2–3 : LTS (Long Training Sequence)
//   Block 4   : SIGNAL OFDM symbol
//   Block 5+  : DATA OFDM symbols (N_SYM total)
//
// Subcarrier layout per OFDM symbol:
//   Data:   [-26:-22, -20:-8, -6:-1, 1:6, 8:20, 22:26] = 48 subcarriers
//   Pilots: [-21, -7, 7, 21]                             =  4 subcarriers
//   Nulls:  DC + guards                                  = 12 subcarriers
// ============================================================

static const bool SCATTER_ENABLED  = true;
static const int  SCATTER_PACKETS  = 1;
static const bool PLOT_STS         = false;
static const bool PLOT_LTS         = false;
static const bool PLOT_SIGNAL      = true;
static const bool PLOT_DATA        = true;
static const bool PLOT_PILOTS      = false;
static const bool PLOT_NULLS       = false;

static const int MAX_DATA_SYMBOLS = 504;
static const int PREAMBLE_BLOCKS  = 4;
static const int MAX_TOTAL_BLOCKS = PREAMBLE_BLOCKS + MAX_DATA_SYMBOLS; // 508
static const int SUBCARRIERS      = 64;
static const int BYTES_PER_BLOCK  = SUBCARRIERS * 4;                    // 256
static const int OUT_PKT_SIZE     = MAX_TOTAL_BLOCKS * BYTES_PER_BLOCK; // 130048
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

struct RateParams { int NBPSC; int NCBPS; int NDBPS; double K; };

static RateParams getRate(uint8_t rateVal) {
    switch (rateVal) {
        case 13: return {1,  48,  24, 1.0};
        case 15: return {1,  48,  36, 1.0};
        case  5: return {2,  96,  48, 1.0/sqrt(2.0)};
        case  7: return {2,  96,  72, 1.0/sqrt(2.0)};
        case  9: return {4, 192,  96, 1.0/sqrt(10.0)};
        case 11: return {4, 192, 144, 1.0/sqrt(10.0)};
        case  1: return {6, 288, 192, 1.0/sqrt(42.0)};
        case  3: return {6, 288, 216, 1.0/sqrt(42.0)};
        default: return {2,  96,  48, 1.0/sqrt(2.0)};
    }
}

static const char* rateNameFromVal(uint8_t rateVal) {
    switch (rateVal) {
        case 13: return "6 Mbps (BPSK 1/2)";
        case 15: return "9 Mbps (BPSK 3/4)";
        case  5: return "12 Mbps (QPSK 1/2)";
        case  7: return "18 Mbps (QPSK 3/4)";
        case  9: return "24 Mbps (16-QAM 1/2)";
        case 11: return "36 Mbps (16-QAM 3/4)";
        case  1: return "48 Mbps (64-QAM 2/3)";
        case  3: return "54 Mbps (64-QAM 3/4)";
        default: return "UNKNOWN";
    }
}

static int8_t g_pilotSeq[127];

static void buildPilotSeq() {
    uint8_t state[7] = {1,1,1,1,1,1,1};
    for (int n = 0; n < 127; n++) {
        uint8_t bit = state[6] ^ state[3];
        for (int s = 6; s > 0; s--) state[s] = state[s-1];
        state[0] = bit;
        g_pilotSeq[n] = (bit == 0) ? 1 : -1;
    }
}

static int g_dataSubcarriers[48];
static int g_pilotSubcarriers[4] = {-21+32, -7+32, 7+32, 21+32};

static void buildSubcarrierMap() {
    int idx = 0;
    for (int f = -26; f <= -22; f++) g_dataSubcarriers[idx++] = f + 32;
    for (int f = -20; f <= -8;  f++) g_dataSubcarriers[idx++] = f + 32;
    for (int f = -6;  f <= -1;  f++) g_dataSubcarriers[idx++] = f + 32;
    for (int f = 1;   f <= 6;   f++) g_dataSubcarriers[idx++] = f + 32;
    for (int f = 8;   f <= 20;  f++) g_dataSubcarriers[idx++] = f + 32;
    for (int f = 22;  f <= 26;  f++) g_dataSubcarriers[idx++] = f + 32;
}

static void packIQ(int8_t* dst, double I, double Q) {
    int16_t iVal = (int16_t)round(I * 32767.0);
    int16_t qVal = (int16_t)round(Q * 32767.0);
    uint16_t iu = (uint16_t)iVal, qu = (uint16_t)qVal;
    dst[0] = (int8_t)((int32_t)(uint8_t)(iu & 0xFF)        - 128);
    dst[1] = (int8_t)((int32_t)(uint8_t)((iu >> 8) & 0xFF) - 128);
    dst[2] = (int8_t)((int32_t)(uint8_t)(qu & 0xFF)        - 128);
    dst[3] = (int8_t)((int32_t)(uint8_t)((qu >> 8) & 0xFF) - 128);
}

static void buildStsBlock(int8_t* blockBuf) {
    memset(blockBuf, 0x80, BYTES_PER_BLOCK);
    const double scale = sqrt(13.0 / 6.0);
    struct { int idx; double I; double Q; } sts[] = {
        {-24,-1,-1},{-20,1,1},{-16,-1,-1},{-12,1,1},
        {-8,-1,-1},{-4,1,1},{4,-1,-1},{8,-1,-1},
        {12,1,1},{16,1,1},{20,1,1},{24,1,1},
    };
    for (int k = 0; k < 12; k++)
        packIQ(blockBuf + (sts[k].idx + 32) * 4, scale * sts[k].I, scale * sts[k].Q);
}

static void buildLtsBlock(int8_t* blockBuf) {
    memset(blockBuf, 0x80, BYTES_PER_BLOCK);
    static const double lts[53] = {
         1, 1,-1,-1, 1, 1,-1, 1,-1, 1, 1, 1, 1, 1, 1,-1,-1, 1, 1,-1, 1,-1, 1, 1, 1, 1,
         0,
         1,-1,-1, 1, 1,-1, 1,-1, 1,-1,-1,-1,-1,-1, 1, 1,-1,-1, 1,-1, 1,-1, 1, 1, 1, 1
    };
    for (int i = 0; i < 53; i++)
        packIQ(blockBuf + ((-26 + i) + 32) * 4, lts[i], 0.0);
}

static void mapBPSK (const uint8_t* bits, double& I, double& Q) { I = bits[0] ? 1.0 : -1.0; Q = 0.0; }
static void mapQPSK (const uint8_t* bits, double& I, double& Q) { I = bits[0] ? 1.0 : -1.0; Q = bits[1] ? 1.0 : -1.0; }
static int qam16Axis(uint8_t b0, uint8_t b1) { return (!b0&&!b1)?-3:(!b0&&b1)?-1:(b0&&b1)?1:3; }
static void map16QAM(const uint8_t* bits, double& I, double& Q) { I=qam16Axis(bits[0],bits[1]); Q=qam16Axis(bits[2],bits[3]); }
static int qam64Axis(uint8_t b0, uint8_t b1, uint8_t b2) {
    switch ((b0<<2)|(b1<<1)|b2) {
        case 0: return -7; case 1: return -5; case 3: return -3; case 2: return -1;
        case 6: return  1; case 7: return  3; case 5: return  5; case 4: return  7;
        default: return 0;
    }
}
static void map64QAM(const uint8_t* bits, double& I, double& Q) {
    I = qam64Axis(bits[0],bits[1],bits[2]);
    Q = qam64Axis(bits[3],bits[4],bits[5]);
}
static void mapSymbol(const uint8_t* bits, int NBPSC, double& I, double& Q) {
    switch (NBPSC) {
        case 1: mapBPSK (bits,I,Q); break;
        case 2: mapQPSK (bits,I,Q); break;
        case 4: map16QAM(bits,I,Q); I/=3.0; Q/=3.0; break;
        case 6: map64QAM(bits,I,Q); I/=7.0; Q/=7.0; break;
        default: I=Q=0.0; break;
    }
}

static void writeOfdmSymbol(int8_t* symBuf, const uint8_t* bits, int NBPSC, int symIdx) {
    for (int sc = 0; sc < 48; sc++) {
        double I, Q;
        mapSymbol(bits + sc * NBPSC, NBPSC, I, Q);
        packIQ(symBuf + g_dataSubcarriers[sc] * 4, I, Q);
    }
    int8_t pilot = g_pilotSeq[symIdx % 127];
    for (int p = 0; p < 4; p++)
        packIQ(symBuf + g_pilotSubcarriers[p] * 4, (double)pilot, 0.0);
}

static int appendToScatter(const int8_t* blockBuf, int8_t* scatterDst,
                            int bytesWritten, int maxBytes, int blockType) {
    bool includeThisBlock;
    switch (blockType) {
        case 0: includeThisBlock = PLOT_STS;    break;
        case 1: includeThisBlock = PLOT_LTS;    break;
        case 2: includeThisBlock = PLOT_SIGNAL; break;
        case 3: includeThisBlock = PLOT_DATA;   break;
        default: return 0;
    }
    if (!includeThisBlock) return 0;
    int added = 0;
    for (int sc = 0; sc < SUBCARRIERS; sc++) {
        bool isData  = g_isDataSC[sc];
        bool isPilot = g_isPilotSC[sc];
        bool isNull  = !isData && !isPilot;
        bool include = false;
        if (blockType == 0 || blockType == 1) { include = true; }
        else { if (isData) include = true; if (isPilot) include = PLOT_PILOTS; if (isNull) include = PLOT_NULLS; }
        if (include) {
            if (bytesWritten + added + 4 > maxBytes) break;
            memcpy(scatterDst + bytesWritten + added, blockBuf + sc*4, 4);
            added += 4;
        }
    }
    return added;
}

struct QamMapperData {
    int frameCount;
    int8_t stsBlock[BYTES_PER_BLOCK];
    int8_t ltsBlock[BYTES_PER_BLOCK];
};

QamMapperData init_qam_mapper(const BlockConfig& config) {
    QamMapperData data;
    data.frameCount = 0;
    buildPilotSeq();
    buildSubcarrierMap();
    buildSubcarrierClassification();
    buildStsBlock(data.stsBlock);
    buildLtsBlock(data.ltsBlock);
    return data;
}

void process_qam_mapper(
    const char**    pipeIn,
    const char**    pipeOut,
    QamMapperData&  customData,
    const BlockConfig& config
) {
    PipeIO inData    (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO inRate    (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);
    PipeIO outIQ     (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outScatter(pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);

    int8_t* dataBuf = new int8_t[inData.getBufferSize()];
    int8_t* rateBuf = new int8_t[inRate.getBufferSize()];
    int8_t* iqBuf   = new int8_t[outIQ.getBufferSize()];

    const int inDataPkt  = config.inputPacketSizes[0];   // 3030
    const int inRatePkt  = config.inputPacketSizes[1];   // 5
    const int outIQPkt   = config.outputPacketSizes[0];  // 130048
    const int scatterPkt = config.outputPacketSizes[1];

    const bool isFirstBatch = (customData.frameCount == 0);

    int actualCount = inData.read(dataBuf);
    inRate.read(rateBuf);

    memset(iqBuf, 0x80, outIQ.getBufferSize());

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
        const bool dbg = isFirstBatch && (i == 0);

        const int dataOff = i * inDataPkt;
        const int rateOff = i * inRatePkt;
        const int iqOff   = i * outIQPkt;

        // Decode rate pipe
        uint8_t rateVal = (uint8_t)((int32_t)rateBuf[rateOff + 0] + 128);
        uint8_t b1      = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t b2      = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        uint8_t b3      = (uint8_t)((int32_t)rateBuf[rateOff + 3] + 128);
        uint8_t b4      = (uint8_t)((int32_t)rateBuf[rateOff + 4] + 128);
        uint32_t encLenBits = (uint32_t)b1 | ((uint32_t)b2 << 8)
                            | ((uint32_t)b3 << 16) | ((uint32_t)b4 << 24);

        int encLen = (int)((encLenBits + 7) / 8);

        RateParams signalParams = {1, 48, 24, 1.0};
        RateParams dataParams   = getRate(rateVal);

        int dataBits = (int)encLenBits - 48;
        if (dataBits < 0) {
            fprintf(stderr, "[QamMapper] FATAL pkt[%d]: encLenBits=%u too small (< 48)\n",
                    i, (unsigned)encLenBits);
            exit(1);
        }
        if (dataBits % dataParams.NCBPS != 0) {
            fprintf(stderr,
                "[QamMapper] FATAL pkt[%d]: DATA bits=%d not divisible by NCBPS=%d\n",
                i, dataBits, dataParams.NCBPS);
            exit(1);
        }
        int N_SYM = dataBits / dataParams.NCBPS;

        int totalBits = encLen * 8;
        uint8_t* bits = new uint8_t[totalBits]();
        for (int b = 0; b < encLen; b++) {
            uint8_t byte = (uint8_t)((int32_t)dataBuf[dataOff + b] + 128);
            for (int bit = 0; bit < 8; bit++)
                bits[b * 8 + bit] = (byte >> bit) & 1;
        }

        int8_t* pktIQ = iqBuf + iqOff;

        // Preamble blocks
        memcpy(pktIQ + 0 * BYTES_PER_BLOCK, customData.stsBlock, BYTES_PER_BLOCK);
        memcpy(pktIQ + 1 * BYTES_PER_BLOCK, customData.stsBlock, BYTES_PER_BLOCK);
        memcpy(pktIQ + 2 * BYTES_PER_BLOCK, customData.ltsBlock, BYTES_PER_BLOCK);
        memcpy(pktIQ + 3 * BYTES_PER_BLOCK, customData.ltsBlock, BYTES_PER_BLOCK);

        // SIGNAL symbol (block 4)
        int8_t* sigBuf = pktIQ + 4 * BYTES_PER_BLOCK;
        memset(sigBuf, 0x80, BYTES_PER_BLOCK);
        writeOfdmSymbol(sigBuf, bits, signalParams.NBPSC, 0);

        // DATA symbols (blocks 5+)
        for (int sym = 0; sym < N_SYM; sym++) {
            int8_t* symBuf = pktIQ + (5 + sym) * BYTES_PER_BLOCK;
            memset(symBuf, 0x80, BYTES_PER_BLOCK);
            writeOfdmSymbol(symBuf, bits + 48 + sym * dataParams.NCBPS,
                            dataParams.NBPSC, sym + 1);
        }

        // Embed actual block count in last 4 bytes of packet (zero-padded region)
        int totalBlocks = 5 + N_SYM;  // 4 preamble + 1 SIGNAL + N_SYM DATA
        for (int j = 0; j < 4; j++) {
            uint8_t b = (uint8_t)(((uint32_t)totalBlocks >> (j * 8)) & 0xFF);
            pktIQ[outIQPkt - 4 + j] = (int8_t)((int32_t)b - 128);
        }

        if (dbg) {
            printf("[QamMapper] pkt[0] INPUT : signal=48  data=%d  total=%u bits\n",
                   dataBits, (unsigned)encLenBits);
            printf("[QamMapper] pkt[0] OUTPUT: %d OFDM blocks x 256 bytes = %d bits "
                   "(4xPreamble + 1xSIGNAL + %dxDATA)\n",
                   totalBlocks, totalBlocks * 256 * 8, N_SYM);
            fflush(stdout);
        }

        delete[] bits;

        if (SCATTER_ENABLED && i < SCATTER_PACKETS) {
            for (int blk = 0; blk < 2; blk++)
                scatterDataBytes += appendToScatter(pktIQ + blk * BYTES_PER_BLOCK, scatterDataStart, scatterDataBytes, scatterMaxBytes, 0);
            for (int blk = 2; blk < 4; blk++)
                scatterDataBytes += appendToScatter(pktIQ + blk * BYTES_PER_BLOCK, scatterDataStart, scatterDataBytes, scatterMaxBytes, 1);
            scatterDataBytes += appendToScatter(pktIQ + 4 * BYTES_PER_BLOCK, scatterDataStart, scatterDataBytes, scatterMaxBytes, 2);
            for (int sym = 0; sym < N_SYM; sym++) {
                scatterDataBytes += appendToScatter(pktIQ + (5+sym)*BYTES_PER_BLOCK, scatterDataStart, scatterDataBytes, scatterMaxBytes, 3);
                if (scatterDataBytes >= scatterMaxBytes) break;
            }
        }
    }

    outIQ.write(iqBuf, actualCount);

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

    delete[] dataBuf;
    delete[] rateBuf;
    delete[] iqBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: qam_mapper <pipeInData> <pipeInRate> <pipeOutIQ> <pipeOutScatter>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "QamMapper",
        2,                            // inputs
        2,                            // outputs
        {3030, 5},                    // inputPacketSizes
        {64, 64},                     // inputBatchSizes
        {130048, 209715200},          // outputPacketSizes
        {64, 1},                      // outputBatchSizes
        false,                        // ltr
        true,                         // startWithAll
        "IEEE 802.11a QAM Mapper: takes (3030, 5) from interleaver, outputs (130048, scatter)"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_qam_mapper, init_qam_mapper);
    return 0;
}