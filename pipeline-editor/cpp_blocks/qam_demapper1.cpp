#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdio>

// ============================================================
// IEEE 802.11a QAM Demapper
//
// Inputs:
//   in[0]: Stacked IQ symbols from qam_mapper   (129024 bytes/pkt)  <- FIRST
//   in[1]: Feedback from ppdu_decapsulate        (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: mac_len_lo
//             Byte 2: mac_len_hi
//
// Outputs:
//   out[0]: rate + lip + DATA_INTERLEAVED        (3027 bytes/pkt)    <- FIRST
//             Byte 0: rate_value
//             Byte 1: lip_lo
//             Byte 2: lip_hi
//             Bytes [3..3+lip-1]: interleaved DATA bits repacked to bytes
//   out[1]: SIGNAL_INTERLEAVED                   (6 bytes/pkt)
//
// Protocol (deadlock-free):
//   1. Read IQ data (in[0])
//   2. Demap SIGNAL symbol -> send SIGNAL_INTERLEAVED (out[1])
//   3. Wait for feedback (in[1]) -> extract rate + mac_length
//   4. Recompute N_SYM from mac_length + rate
//   5. Demap DATA symbols -> send rate+lip+DATA_INTERLEAVED (out[0])
//
// int8 pipe convention: stored byte B -> pipe int8_t = int8_t(uint8_t(B) - 128)
// ============================================================

// -----------------------------------------------------------------------
// Constants (must match qam_mapper.cpp)
// -----------------------------------------------------------------------
static const int MAX_SYMBOLS    = 504;
static const int SUBCARRIERS    = 64;
static const int BYTES_PER_SYM  = SUBCARRIERS * 4;             // 256
static const int IN_IQ_PKT_SIZE = MAX_SYMBOLS * BYTES_PER_SYM; // 129024
static const int BATCH_SIZE     = 64;

// -----------------------------------------------------------------------
// Rate parameters
// -----------------------------------------------------------------------
struct RateParams {
    int NBPSC;
    int NCBPS;
    int NDBPS;
};

static RateParams getRate(uint8_t rateVal) {
    switch (rateVal) {
        case 13: return {1,  48,  24};
        case 15: return {1,  48,  36};
        case  5: return {2,  96,  48};
        case  7: return {2,  96,  72};
        case  9: return {4, 192,  96};
        case 11: return {4, 192, 144};
        case  1: return {6, 288, 192};
        case  3: return {6, 288, 216};
        default: return {2,  96,  48};
    }
}

// -----------------------------------------------------------------------
// Subcarrier map (must match mapper)
// -----------------------------------------------------------------------
static int g_dataSubcarriers[48];

static void buildSubcarrierMap() {
    int idx = 0;
    for (int f = -26; f <= -22; f++) g_dataSubcarriers[idx++] = f + 32;
    for (int f = -20; f <= -8;  f++) g_dataSubcarriers[idx++] = f + 32;
    for (int f = -6;  f <= -1;  f++) g_dataSubcarriers[idx++] = f + 32;
    for (int f = 1;   f <= 6;   f++) g_dataSubcarriers[idx++] = f + 32;
    for (int f = 8;   f <= 20;  f++) g_dataSubcarriers[idx++] = f + 32;
    for (int f = 22;  f <= 26;  f++) g_dataSubcarriers[idx++] = f + 32;
}

// -----------------------------------------------------------------------
// Unpack one IQ pair from 4 pipe bytes
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
// Hard-decision demappers
//
// 64-QAM Gray code axis (b0=MSB): inverse of qam64Axis() in mapper.
//   000->-7, 001->-5, 011->-3, 010->-1, 110->+1, 111->+3, 101->+5, 100->+7
// -----------------------------------------------------------------------

static void demapBPSK(double I, double /*Q*/, uint8_t* bits) {
    bits[0] = (I >= 0.0) ? 1 : 0;
}

static void demapQPSK(double I, double Q, uint8_t* bits) {
    bits[0] = (I >= 0.0) ? 1 : 0;
    bits[1] = (Q >= 0.0) ? 1 : 0;
}

static void demapQAM16Axis(double val, uint8_t& b0, uint8_t& b1) {
    double x = val * 3.0;
    if      (x < -2.0) { b0 = 0; b1 = 0; }
    else if (x <  0.0) { b0 = 0; b1 = 1; }
    else if (x <  2.0) { b0 = 1; b1 = 1; }
    else               { b0 = 1; b1 = 0; }
}
static void demap16QAM(double I, double Q, uint8_t* bits) {
    demapQAM16Axis(I, bits[0], bits[1]);
    demapQAM16Axis(Q, bits[2], bits[3]);
}

static void demapQAM64Axis(double val, uint8_t& b0, uint8_t& b1, uint8_t& b2) {
    double x = val * 7.0;
    if      (x < -6.0) { b0=0; b1=0; b2=0; }
    else if (x < -4.0) { b0=0; b1=0; b2=1; }
    else if (x < -2.0) { b0=0; b1=1; b2=1; }
    else if (x <  0.0) { b0=0; b1=1; b2=0; }
    else if (x <  2.0) { b0=1; b1=1; b2=0; }
    else if (x <  4.0) { b0=1; b1=1; b2=1; }
    else if (x <  6.0) { b0=1; b1=0; b2=1; }
    else               { b0=1; b1=0; b2=0; }
}
static void demap64QAM(double I, double Q, uint8_t* bits) {
    demapQAM64Axis(I, bits[0], bits[1], bits[2]);
    demapQAM64Axis(Q, bits[3], bits[4], bits[5]);
}

static void demapSymbol(double I, double Q, int NBPSC, uint8_t* bits) {
    switch (NBPSC) {
        case 1: demapBPSK (I, Q, bits); break;
        case 2: demapQPSK (I, Q, bits); break;
        case 4: demap16QAM(I, Q, bits); break;
        case 6: demap64QAM(I, Q, bits); break;
        default: memset(bits, 0, NBPSC); break;
    }
}

// -----------------------------------------------------------------------
// Extract NCBPS bits from one OFDM symbol
// -----------------------------------------------------------------------
static void readOfdmSymbol(const int8_t* symBuf, uint8_t* bits, int NBPSC) {
    for (int sc = 0; sc < 48; sc++) {
        double I, Q;
        unpackIQ(symBuf + g_dataSubcarriers[sc] * 4, I, Q);
        demapSymbol(I, Q, NBPSC, bits + sc * NBPSC);
    }
}

// -----------------------------------------------------------------------
// Dynamic N_SYM computation from mac_length + rate
// -----------------------------------------------------------------------
static void computeEncLen(int macLength, const RateParams& rp,
                           int& N_SYM, int& encLen, int& lip) {
    int dataBits = 16 + macLength * 8 + 6;
    N_SYM  = (dataBits + rp.NDBPS - 1) / rp.NDBPS;
    encLen = 6 + (N_SYM * rp.NCBPS) / 8;
    lip    = encLen - 6;
}

// -----------------------------------------------------------------------
// Block state
// -----------------------------------------------------------------------
struct QamDemapperData {
    int frameCount;
};

QamDemapperData init_qam_demapper(const BlockConfig& config) {
    QamDemapperData data;
    data.frameCount = 0;
    buildSubcarrierMap();
    return data;
}

void process_qam_demapper(
    const char**      pipeIn,
    const char**      pipeOut,
    QamDemapperData&  customData,
    const BlockConfig& config
) {
    PipeIO inIQ      (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO inFeedback(pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);
    PipeIO outData   (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outSignal (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);

    int8_t* iqBuf       = new int8_t[inIQ.getBufferSize()];
    int8_t* feedbackBuf = new int8_t[inFeedback.getBufferSize()];
    int8_t* signalBuf   = new int8_t[outSignal.getBufferSize()];
    int8_t* dataOutBuf  = new int8_t[outData.getBufferSize()];

    const int inIQPkt    = config.inputPacketSizes[0];
    const int inFbPkt    = config.inputPacketSizes[1];
    const int outDataPkt = config.outputPacketSizes[0];
    const int outSigPkt  = config.outputPacketSizes[1];

    // ===== STEP 1: Read IQ =====
    int actualCount = inIQ.read(iqBuf);

    memset(signalBuf,  0x80, outSignal.getBufferSize());
    memset(dataOutBuf, 0x80, outData.getBufferSize());

    // ===== STEP 2: Demap SIGNAL symbol for every packet =====
    for (int i = 0; i < actualCount; i++) {
        const int iqOff  = i * inIQPkt;
        const int sigOff = i * outSigPkt;

        const int8_t* sym0 = iqBuf + iqOff;

        uint8_t signalBits[48];
        readOfdmSymbol(sym0, signalBits, 1);  // BPSK

        for (int b = 0; b < 6; b++) {
            uint8_t byte = 0;
            for (int bit = 0; bit < 8; bit++)
                byte |= (signalBits[b * 8 + bit] & 1) << bit;
            signalBuf[sigOff + b] = (int8_t)((int32_t)byte - 128);
        }
    }

    // ===== STEP 3: Send SIGNAL_INTERLEAVED =====
    outSignal.write(signalBuf, actualCount);

    // ===== STEP 4: Wait for feedback =====
    inFeedback.read(feedbackBuf);

    // ===== STEP 5+6+7: Demap DATA symbols =====
    for (int i = 0; i < actualCount; i++) {
        const int iqOff   = i * inIQPkt;
        const int fbOff   = i * inFbPkt;
        const int dataOff = i * outDataPkt;

        uint8_t rateVal  = (uint8_t)((int32_t)feedbackBuf[fbOff + 0] + 128);
        uint8_t macLenLo = (uint8_t)((int32_t)feedbackBuf[fbOff + 1] + 128);
        uint8_t macLenHi = (uint8_t)((int32_t)feedbackBuf[fbOff + 2] + 128);
        int macLength = (int)macLenLo | ((int)macLenHi << 8);

        RateParams rp = getRate(rateVal);
        int N_SYM, encLen, lip;
        computeEncLen(macLength, rp, N_SYM, encLen, lip);

        if (lip < 0)    lip = 0;
        if (lip > 3024) lip = 3024;

        int totalDataBits = N_SYM * rp.NCBPS;
        uint8_t* dataBits = new uint8_t[totalDataBits]();

        for (int sym = 0; sym < N_SYM; sym++)
            readOfdmSymbol(iqBuf + iqOff + (sym + 1) * BYTES_PER_SYM,
                           dataBits + sym * rp.NCBPS, rp.NBPSC);

        int dataBytes = totalDataBits / 8;
        if (dataBytes > lip) dataBytes = lip;

        for (int b = 0; b < dataBytes; b++) {
            uint8_t byte = 0;
            for (int bit = 0; bit < 8; bit++)
                byte |= (dataBits[b * 8 + bit] & 1) << bit;
            dataOutBuf[dataOff + 3 + b] = (int8_t)((int32_t)byte - 128);
        }

        dataOutBuf[dataOff + 0] = (int8_t)((int32_t)rateVal              - 128);
        dataOutBuf[dataOff + 1] = (int8_t)((int32_t)(lip & 0xFF)         - 128);
        dataOutBuf[dataOff + 2] = (int8_t)((int32_t)((lip >> 8) & 0xFF)  - 128);

        delete[] dataBits;
    }

    outData.write(dataOutBuf, actualCount);
    customData.frameCount += actualCount;

    delete[] iqBuf;
    delete[] feedbackBuf;
    delete[] signalBuf;
    delete[] dataOutBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: qam_demapper <pipeInIQ> <pipeInFeedback>"
            " <pipeOutData> <pipeOutSignal>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "QamDemapper",
        2,              // inputs
        2,              // outputs
        {129024, 3},    // inputPacketSizes  [IQ stacked: 504*256, feedback]
        {64, 64},       // inputBatchSizes
        {3027, 6},      // outputPacketSizes [rate+lip+DATA_INT, SIGNAL_INT]
        {64, 64},       // outputBatchSizes
        false,          // ltr
        true,           // startWithAll
        "IEEE 802.11a QAM Demapper: IQ symbols -> interleaved bits; SIGNAL first for feedback"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_qam_demapper, init_qam_demapper);
    return 0;
}