#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>
#include <cmath>

// ============================================================
// IEEE 802.11a QAM Demapper (no scatter plot)
//
// Inputs:
//   in[0]: OFDM symbols  (256 bytes/symbol = 64 subcarriers × 4 bytes)  <- FIRST
//             Per subcarrier: I[2 bytes int16 LE] + Q[2 bytes int16 LE], scaled by 32767
//   in[1]: Feedback from ppdu_decapsulate  (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: mac_len_lo
//             Byte 2: mac_len_hi
//
// Outputs:
//   out[0]: SIGNAL bits  (6 bytes/pkt)                                   <- FIRST
//             48 bits demapped from first OFDM symbol (always BPSK)
//             Packed right-MSB (bit 0 = bit 0 of byte 0)
//   out[1]: rate + DATA bits  (3025 bytes/pkt)
//             Byte 0: rate_value
//             Bytes [1..]: demapped DATA bits, packed right-MSB
//
// Protocol (deadlock-free):
//   1. Read SIGNAL OFDM symbol  (in[0])  -- arrives FIRST (mapper sends it first)
//   2. Extract 48 data symbols (skip pilots/nulls), demap BPSK -> 48 bits
//   3. Send SIGNAL bits         (out[0]) -- ppdu_decap reads, sends feedback to us
//   4. Read feedback            (in[1])  -- gate; extract rate, calculate N_SYM
//   5. Read N_SYM DATA OFDM symbols (in[0]), extract data symbols, demap to bits
//   6. Pack bits to bytes, send rate + DATA bits (out[1])
//
// N_SYM calculation (must match qam_mapper):
//   L_PPDU = SERVICE_bits(16) + payload_bits(1500*8) + CRC_bits(32) + TAIL_bits(6)
//   N_SYM  = ceil(L_PPDU / NDBPS)
//
// Rate -> (NBPSC, NCBPS, NDBPS):
//   13 ->  6 Mbps: BPSK,   NBPSC=1, NCBPS=48,  NDBPS=24
//   15 ->  9 Mbps: BPSK,   NBPSC=1, NCBPS=48,  NDBPS=36
//    5 -> 12 Mbps: QPSK,   NBPSC=2, NCBPS=96,  NDBPS=48
//    7 -> 18 Mbps: QPSK,   NBPSC=2, NCBPS=96,  NDBPS=72
//    9 -> 24 Mbps: 16-QAM, NBPSC=4, NCBPS=192, NDBPS=96
//   11 -> 36 Mbps: 16-QAM, NBPSC=4, NCBPS=192, NDBPS=144
//    1 -> 48 Mbps: 64-QAM, NBPSC=6, NCBPS=288, NDBPS=192
//    3 -> 54 Mbps: 64-QAM, NBPSC=6, NCBPS=288, NDBPS=216
// ============================================================

// ---- Rate parameters ----------------------------------------
struct RateModParams {
    int    NBPSC;
    int    NCBPS;
    int    NDBPS;
    const char* modType;
};

static RateModParams getRateModParams(uint8_t rateVal) {
    switch (rateVal) {
        case 13: return {1,  48,  24, "BPSK"};    //  6 Mbps
        case 15: return {1,  48,  36, "BPSK"};    //  9 Mbps
        case  5: return {2,  96,  48, "QPSK"};    // 12 Mbps
        case  7: return {2,  96,  72, "QPSK"};    // 18 Mbps
        case  9: return {4, 192,  96, "16QAM"};   // 24 Mbps
        case 11: return {4, 192, 144, "16QAM"};   // 36 Mbps
        case  1: return {6, 288, 192, "64QAM"};   // 48 Mbps
        case  3: return {6, 288, 216, "64QAM"};   // 54 Mbps
        default: return {2,  96,  48, "QPSK"};    // fallback
    }
}

// ---- Subcarrier layout --------------------------------------
static const int DATA_SUBCARRIERS[48] = {
    -26,-25,-24,-23,-22,
    -20,-19,-18,-17,-16,-15,-14,-13,-12,-11,-10,-9,-8,
    -6,-5,-4,-3,-2,-1,
    1,2,3,4,5,6,
    8,9,10,11,12,13,14,15,16,17,18,19,20,
    22,23,24,25,26
};

// ---- Extract 64 complex subcarriers from packed int8 buffer -----
static void extractSubcarriers(const int8_t* ofdmBuf,
                                double* subcarriersI, double* subcarriersQ) {
    for (int i = 0; i < 64; i++) {
        int base = i * 4;
        uint8_t Ilo = (uint8_t)((int16_t)ofdmBuf[base + 0] + 128);
        uint8_t Ihi = (uint8_t)((int16_t)ofdmBuf[base + 1] + 128);
        uint8_t Qlo = (uint8_t)((int16_t)ofdmBuf[base + 2] + 128);
        uint8_t Qhi = (uint8_t)((int16_t)ofdmBuf[base + 3] + 128);
        int16_t I_val, Q_val;
        uint8_t Ibuf[2] = {Ilo, Ihi};
        uint8_t Qbuf[2] = {Qlo, Qhi};
        memcpy(&I_val, Ibuf, 2);
        memcpy(&Q_val, Qbuf, 2);
        subcarriersI[i] = (double)I_val / 32767.0;
        subcarriersQ[i] = (double)Q_val / 32767.0;
    }
}

// ---- Hard-decision demapping functions ----------------------
static uint8_t demapBpsk(double I) {
    return (I >= 0.0) ? 1 : 0;
}

static void demapQpsk(double I, double Q, uint8_t* bits) {
    bits[0] = (I >= 0.0) ? 1 : 0;
    bits[1] = (Q >= 0.0) ? 1 : 0;
}

static void demap16Qam(double I, double Q, uint8_t* bits) {
    const double k = 1.0 / sqrt(10.0);
    I /= k;
    Q /= k;
    if      (I < -2.0) { bits[0] = 0; bits[1] = 0; }
    else if (I <  0.0) { bits[0] = 0; bits[1] = 1; }
    else if (I <  2.0) { bits[0] = 1; bits[1] = 1; }
    else               { bits[0] = 1; bits[1] = 0; }
    if      (Q < -2.0) { bits[2] = 0; bits[3] = 0; }
    else if (Q <  0.0) { bits[2] = 0; bits[3] = 1; }
    else if (Q <  2.0) { bits[2] = 1; bits[3] = 1; }
    else               { bits[2] = 1; bits[3] = 0; }
}

static void demap64Qam(double I, double Q, uint8_t* bits) {
    const double k = 1.0 / sqrt(42.0);
    I /= k;
    Q /= k;
    if      (I < -6.0) { bits[0]=0; bits[1]=0; bits[2]=0; }
    else if (I < -4.0) { bits[0]=0; bits[1]=0; bits[2]=1; }
    else if (I < -2.0) { bits[0]=0; bits[1]=1; bits[2]=1; }
    else if (I <  0.0) { bits[0]=0; bits[1]=1; bits[2]=0; }
    else if (I <  2.0) { bits[0]=1; bits[1]=1; bits[2]=0; }
    else if (I <  4.0) { bits[0]=1; bits[1]=1; bits[2]=1; }
    else if (I <  6.0) { bits[0]=1; bits[1]=0; bits[2]=1; }
    else               { bits[0]=1; bits[1]=0; bits[2]=0; }
    if      (Q < -6.0) { bits[3]=0; bits[4]=0; bits[5]=0; }
    else if (Q < -4.0) { bits[3]=0; bits[4]=0; bits[5]=1; }
    else if (Q < -2.0) { bits[3]=0; bits[4]=1; bits[5]=1; }
    else if (Q <  0.0) { bits[3]=0; bits[4]=1; bits[5]=0; }
    else if (Q <  2.0) { bits[3]=1; bits[4]=1; bits[5]=0; }
    else if (Q <  4.0) { bits[3]=1; bits[4]=1; bits[5]=1; }
    else if (Q <  6.0) { bits[3]=1; bits[4]=0; bits[5]=1; }
    else               { bits[3]=1; bits[4]=0; bits[5]=0; }
}

// ---- Demap one subcarrier, append bits to array -------------
static void demapSymbol(double I, double Q, int NBPSC,
                        uint8_t* outBits, int& bitIdx, int maxBits) {
    uint8_t symbolBits[6];
    if      (NBPSC == 1) { symbolBits[0] = demapBpsk(I); }
    else if (NBPSC == 2) { demapQpsk(I, Q, symbolBits); }
    else if (NBPSC == 4) { demap16Qam(I, Q, symbolBits); }
    else                 { demap64Qam(I, Q, symbolBits); }
    for (int b = 0; b < NBPSC && bitIdx < maxBits; b++) {
        outBits[bitIdx++] = symbolBits[b];
    }
}

// ---- State --------------------------------------------------
struct QamDemapperNoScatterData {
    int frameCount;
};

QamDemapperNoScatterData init_qam_demapper(const BlockConfig& config) {
    QamDemapperNoScatterData data;
    data.frameCount = 0;

    printf("[QamDemapper] IEEE 802.11a QAM demapper (no scatter plot)\n");
    printf("[QamDemapper] Pipeline wiring: feedback=in[0](argv[1]), OFDM=in[1](argv[2])\n");
    printf("[QamDemapper] Protocol (deadlock-free):\n");
    printf("[QamDemapper]   1. Read SIGNAL OFDM symbol (in[1])  -- mapper sends it first\n");
    printf("[QamDemapper]   2. Demap BPSK -> 48 bits, pack to 6 bytes\n");
    printf("[QamDemapper]   3. Send SIGNAL bits        (out[0]) -- ppdu_decap unblocks\n");
    printf("[QamDemapper]   4. Read feedback            (in[0]) -- gate: rate + N_SYM\n");
    printf("[QamDemapper]   5. Read N_SYM DATA OFDM symbols (in[1]) in one read\n");
    printf("[QamDemapper]   6. Send rate + DATA bits   (out[1])\n");
    printf("[QamDemapper] Ready\n");

    return data;
}

void process_qam_demapper(
    const char** pipeIn,
    const char** pipeOut,
    QamDemapperNoScatterData& customData,
    const BlockConfig& config
) {
    // Pipeline wiring (argv order):
    //   argv[1] = in[0] = feedback from ppdu_decapsulate (3 bytes/pkt)   -- P21
    //   argv[2] = in[1] = OFDM symbols from qam_mapper   (256 bytes/pkt) -- P22
    //   argv[3] = out[0] = SIGNAL bits                   (6 bytes/pkt)   -- P19
    //   argv[4] = out[1] = rate + DATA bits               (3025 bytes/pkt)-- P20
    //
    // Protocol (deadlock-free, feedback-driven):
    //   1. Read one SIGNAL OFDM symbol (in[1]) -- mapper sends SIGNAL first
    //   2. Demap BPSK -> 48 bits, send SIGNAL bits (out[0]) -- unblocks ppdu_decap
    //   3. Read feedback (in[0]) -- gate: get rate, compute N_SYM
    //   4. Read N_SYM DATA OFDM symbols (in[1]) in one batch read
    //   5. Demap, pack, send rate + DATA bits (out[1])
    PipeIO inFeedback (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO inOfdm     (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);
    PipeIO outSignal  (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outData    (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);

    const int inFbPkt    = config.inputPacketSizes[0];   // 3
    const int outDataPkt = config.outputPacketSizes[1];  // 3025

    int8_t  signalOfdmBuf[256];
    int8_t* feedbackBuf = new int8_t[inFeedback.getBufferSize()];
    int8_t* signalBuf   = new int8_t[outSignal.getBufferSize()];
    int8_t* dataBuf     = new int8_t[outData.getBufferSize()];

    double subcarriersI[64], subcarriersQ[64];

    const bool isFirstBatch = (customData.frameCount == 0);

    // ===== STEP 1: Read SIGNAL OFDM symbol (in[0]) -- arrives first =====
    inOfdm.read(signalOfdmBuf);

    // ===== STEP 2: Extract 48 data symbols, demap BPSK -> 48 bits =====
    extractSubcarriers(signalOfdmBuf, subcarriersI, subcarriersQ);

    uint8_t signalBits[48];
    for (int i = 0; i < 48; i++) {
        int arr_idx  = DATA_SUBCARRIERS[i] + 32;
        signalBits[i] = demapBpsk(subcarriersI[arr_idx]);
    }

    // Pack 48 bits to 6 bytes (right-MSB)
    memset(signalBuf, 0, outSignal.getBufferSize());
    for (int byte = 0; byte < 6; byte++) {
        uint8_t b = 0;
        for (int bit = 0; bit < 8; bit++) {
            b |= (signalBits[byte * 8 + bit] << bit);
        }
        signalBuf[byte] = (int8_t)((int16_t)b - 128);
    }

    // ===== STEP 3: Send SIGNAL bits -- ppdu_decap unblocks, sends feedback =====
    outSignal.write(signalBuf, 1);

    // ===== STEP 4: Read feedback -- gate =====
    int actualCount = inFeedback.read(feedbackBuf);

    memset(dataBuf, 0, outData.getBufferSize());

    // ===== STEP 5 + 6: Process each feedback packet =====
    for (int pktIdx = 0; pktIdx < actualCount; pktIdx++) {
        const int fbOff   = pktIdx * inFbPkt;
        const int dataOff = pktIdx * outDataPkt;

        uint8_t rateVal = (uint8_t)((int32_t)feedbackBuf[fbOff + 0] + 128);

        // Calculate N_SYM (must match qam_mapper exactly)
        RateModParams mp   = getRateModParams(rateVal);
        const int L_PPDU   = 16 + (1500 * 8) + 32 + 6;  // 12054 bits
        int N_SYM          = (L_PPDU + mp.NDBPS - 1) / mp.NDBPS;
        int totalDataBits  = N_SYM * mp.NCBPS;

        if (isFirstBatch && pktIdx < 3) {
            printf("\n========================================\n");
            printf("[QamDemapper] Frame %d\n", customData.frameCount + pktIdx + 1);
            printf("========================================\n");
            printf("  RATE value : %u (%s)\n", (unsigned)rateVal, mp.modType);
            printf("  N_SYM      : %d\n", N_SYM);
            printf("  Total bits : %d\n", totalDataBits);
        }

        // Read N_SYM DATA OFDM symbols one at a time (batchSize=1 on OFDM pipe)
        uint8_t* dataBits = new uint8_t[totalDataBits]();
        int bitIdx = 0;

        int8_t singleOfdmBuf[256];
        for (int symIdx = 0; symIdx < N_SYM; symIdx++) {
            inOfdm.read(singleOfdmBuf);
            extractSubcarriers(singleOfdmBuf, subcarriersI, subcarriersQ);

            for (int i = 0; i < 48; i++) {
                int arr_idx = DATA_SUBCARRIERS[i] + 32;
                demapSymbol(subcarriersI[arr_idx], subcarriersQ[arr_idx],
                            mp.NBPSC, dataBits, bitIdx, totalDataBits);
            }

            if (isFirstBatch && pktIdx == 0 && symIdx % 50 == 0) {
                printf("  [QamDemapper] Processed DATA OFDM symbol %d/%d\n", symIdx + 1, N_SYM);
            }
        }

        // Pack bits to bytes (right-MSB)
        int numDataBytes = (totalDataBits + 7) / 8;
        uint8_t* dataBytes = new uint8_t[numDataBytes]();
        for (int byte = 0; byte < numDataBytes; byte++) {
            uint8_t b = 0;
            for (int bit = 0; bit < 8; bit++) {
                int bitPos = byte * 8 + bit;
                if (bitPos < totalDataBits) b |= (dataBits[bitPos] << bit);
            }
            dataBytes[byte] = b;
        }

        // Write rate byte + data bytes to output buffer
        dataBuf[dataOff + 0] = (int8_t)((int32_t)rateVal - 128);
        int copyLen = (numDataBytes < outDataPkt - 1) ? numDataBytes : outDataPkt - 1;
        for (int i = 0; i < copyLen; i++) {
            dataBuf[dataOff + 1 + i] = (int8_t)((int32_t)dataBytes[i] - 128);
        }

        if (isFirstBatch && pktIdx < 3) {
            printf("  Demapped %d DATA symbols, packed to %d bytes\n",
                   N_SYM * 48, numDataBytes);
        }

        delete[] dataBits;
        delete[] dataBytes;
    }

    // ===== STEP 6: Send rate + DATA bits =====
    outData.write(dataBuf, actualCount);

    customData.frameCount += actualCount;

    delete[] feedbackBuf;
    delete[] signalBuf;
    delete[] dataBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: qam_demapper_no_scatter <pipeInOfdm> <pipeInFeedback>"
            " <pipeOutSignal> <pipeOutData>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "QamDemapperNoScatter",
        2,                       // inputs
        2,                       // outputs
        {3, 256},                // inputPacketSizes  [feedback(argv[1]), OFDM_SYMBOL(argv[2])]
        {6000, 1},               // inputBatchSizes   [feedback:6000, OFDM:1 symbol at a time]
        {6, 3025},               // outputPacketSizes [SIGNAL, rate+DATA]
        {6000, 6000},            // outputBatchSizes
        false,                   // ltr (right-to-left; feedback-driven)
        true,                    // startWithAll
        "IEEE 802.11a QAM demapper (no scatter): feedback(in[0]) + OFDM(in[1]) -> bits"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_qam_demapper, init_qam_demapper);
    return 0;
}