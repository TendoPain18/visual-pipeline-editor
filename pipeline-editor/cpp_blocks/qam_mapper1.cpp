#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdio>

// ============================================================
// IEEE 802.11a QAM Mapper
//
// Inputs:
//   in[0]: Interleaved DATA from interleaver    (3030 bytes/pkt)  <- FIRST
//   in[1]: rate_encoded_length from interleaver (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: enc_len_lo  (meaningful encoded bytes in in[0])
//             Byte 2: enc_len_hi
//
// Outputs:
//   out[0]: Stacked IQ symbols                  (129024 bytes/pkt)
//             Layout per packet:
//               N_total_sym symbols, each 256 bytes:
//                 64 subcarriers x 4 bytes [I_lo, I_hi, Q_lo, Q_hi] (little-endian int16)
//               Remaining bytes up to 129024 are zero.
//             Symbol 0        = SIGNAL  (BPSK, K=1,   NCBPS=48)
//             Symbols 1..NSYM = DATA    (rate-dependent modulation)
//
// Subcarrier layout (64-point FFT, indices -32..+31 mapped to array [0..63]):
//   Data:   [-26:-22, -20:-8, -6:-1, 1:6, 8:20, 22:26]  = 48 subcarriers
//   Pilots: [-21, -7, 7, 21]                              = 4 subcarriers
//   Nulls:  everything else including DC (index 0)        = 12 subcarriers
//
// Pilot value for symbol n: pilot_seq[n % 127]  (BPSK, +/-1, unscaled)
//
// Normalization: divide by max integer constellation point so packed
// int16 range [-32767, 32767] is fully utilised without clipping.
//
// int8 pipe convention: stored byte B -> pipe int8_t = int8_t(uint8_t(B) - 128)
// ============================================================

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------
static const int MAX_SYMBOLS    = 504;
static const int SUBCARRIERS    = 64;
static const int BYTES_PER_SYM  = SUBCARRIERS * 4;             // 256
static const int OUT_PKT_SIZE   = MAX_SYMBOLS * BYTES_PER_SYM; // 129024
static const int BATCH_SIZE     = 64;

// -----------------------------------------------------------------------
// Rate parameters
// -----------------------------------------------------------------------
struct RateParams {
    int NBPSC;
    int NCBPS;
    int NDBPS;
    double K;
};

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

// -----------------------------------------------------------------------
// Pilot sequence — 127-element PRBS (IEEE 802.11a eq. 25)
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// Subcarrier index map
// -----------------------------------------------------------------------
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

// -----------------------------------------------------------------------
// Gray-coded QAM mapping
// -----------------------------------------------------------------------

static void mapBPSK(const uint8_t* bits, double& I, double& Q) {
    I = (bits[0] == 0) ? -1.0 : 1.0;
    Q = 0.0;
}

static void mapQPSK(const uint8_t* bits, double& I, double& Q) {
    I = (bits[0] == 0) ? -1.0 : 1.0;
    Q = (bits[1] == 0) ? -1.0 : 1.0;
}

static int qam16Axis(uint8_t b0, uint8_t b1) {
    if      (!b0 && !b1) return -3;
    else if (!b0 &&  b1) return -1;
    else if ( b0 &&  b1) return  1;
    else                  return  3;
}
static void map16QAM(const uint8_t* bits, double& I, double& Q) {
    I = (double)qam16Axis(bits[0], bits[1]);
    Q = (double)qam16Axis(bits[2], bits[3]);
}

// 64-QAM: b0=MSB, Gray coded per IEEE 802.11a Table 85
// 000->-7, 001->-5, 011->-3, 010->-1, 110->+1, 111->+3, 101->+5, 100->+7
static int qam64Axis(uint8_t b0, uint8_t b1, uint8_t b2) {
    uint8_t code = (b0 << 2) | (b1 << 1) | b2;
    switch (code) {
        case 0: return -7;
        case 1: return -5;
        case 3: return -3;
        case 2: return -1;
        case 6: return  1;
        case 7: return  3;
        case 5: return  5;
        case 4: return  7;
        default: return 0;
    }
}
static void map64QAM(const uint8_t* bits, double& I, double& Q) {
    I = (double)qam64Axis(bits[0], bits[1], bits[2]);
    Q = (double)qam64Axis(bits[3], bits[4], bits[5]);
}

static void mapSymbol(const uint8_t* bits, int NBPSC, double /*K*/, double& I, double& Q) {
    switch (NBPSC) {
        case 1: mapBPSK (bits, I, Q); I /= 1.0; Q /= 1.0; break;
        case 2: mapQPSK (bits, I, Q); I /= 1.0; Q /= 1.0; break;
        case 4: map16QAM(bits, I, Q); I /= 3.0; Q /= 3.0; break;
        case 6: map64QAM(bits, I, Q); I /= 7.0; Q /= 7.0; break;
        default: I = Q = 0.0; break;
    }
}

// -----------------------------------------------------------------------
// Pack one IQ pair into 4 pipe bytes (little-endian int16, -128 offset)
// -----------------------------------------------------------------------
static void packIQ(int8_t* dst, double I, double Q) {
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
// Write one OFDM symbol from NCBPS bits
// -----------------------------------------------------------------------
static void writeOfdmSymbol(
    int8_t*        symBuf,
    const uint8_t* bits,
    int            NBPSC,
    double         K,
    int            symIdx
) {
    for (int sc = 0; sc < 48; sc++) {
        double I, Q;
        mapSymbol(bits + sc * NBPSC, NBPSC, K, I, Q);
        packIQ(symBuf + g_dataSubcarriers[sc] * 4, I, Q);
    }
    int8_t pilot = g_pilotSeq[symIdx % 127];
    double pilotVal = (double)pilot;
    for (int p = 0; p < 4; p++) {
        packIQ(symBuf + g_pilotSubcarriers[p] * 4, pilotVal, 0.0);
    }
}

// -----------------------------------------------------------------------
// Block state
// -----------------------------------------------------------------------
struct QamMapperData {
    int frameCount;
};

QamMapperData init_qam_mapper(const BlockConfig& config) {
    QamMapperData data;
    data.frameCount = 0;
    buildPilotSeq();
    buildSubcarrierMap();
    return data;
}

void process_qam_mapper(
    const char**    pipeIn,
    const char**    pipeOut,
    QamMapperData&  customData,
    const BlockConfig& config
) {
    PipeIO inData (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO inRate (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);
    PipeIO outIQ  (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);

    int8_t* dataBuf = new int8_t[inData.getBufferSize()];
    int8_t* rateBuf = new int8_t[inRate.getBufferSize()];
    int8_t* iqBuf   = new int8_t[outIQ.getBufferSize()];

    const int inDataPkt = config.inputPacketSizes[0];
    const int inRatePkt = config.inputPacketSizes[1];
    const int outIQPkt  = config.outputPacketSizes[0];

    int actualCount = inData.read(dataBuf);
    inRate.read(rateBuf);

    memset(iqBuf, 0x80, outIQ.getBufferSize());

    for (int i = 0; i < actualCount; i++) {
        const int dataOff = i * inDataPkt;
        const int rateOff = i * inRatePkt;
        const int iqOff   = i * outIQPkt;

        uint8_t rateVal  = (uint8_t)((int32_t)rateBuf[rateOff + 0] + 128);
        uint8_t encLenLo = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t encLenHi = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        int encLen = (int)encLenLo | ((int)encLenHi << 8);

        RateParams signalParams = {1, 48, 24, 1.0};
        RateParams dataParams   = getRate(rateVal);

        int dataBits = (encLen - 6) * 8;
        if (dataBits < 0) {
            fprintf(stderr,
                "[QamMapper] FATAL pkt[%d]: enc_len=%d too small for SIGNAL field\n",
                i, encLen);
            exit(1);
        }
        if (dataBits % dataParams.NCBPS != 0) {
            fprintf(stderr,
                "[QamMapper] FATAL pkt[%d]: DATA bits=%d not divisible by NCBPS=%d "
                "(rate=%d, enc_len=%d).\n",
                i, dataBits, dataParams.NCBPS, rateVal, encLen);
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

        // Symbol 0: SIGNAL (BPSK)
        int8_t* sym0Buf = iqBuf + iqOff;
        memset(sym0Buf, 0x80, BYTES_PER_SYM);
        writeOfdmSymbol(sym0Buf, bits, signalParams.NBPSC, signalParams.K, 0);

        // Symbols 1..N_SYM: DATA
        for (int sym = 0; sym < N_SYM; sym++) {
            int8_t* symBuf = iqBuf + iqOff + (sym + 1) * BYTES_PER_SYM;
            memset(symBuf, 0x80, BYTES_PER_SYM);
            writeOfdmSymbol(symBuf, bits + 48 + sym * dataParams.NCBPS,
                            dataParams.NBPSC, dataParams.K, sym + 1);
        }

        delete[] bits;
    }

    outIQ.write(iqBuf, actualCount);
    customData.frameCount += actualCount;

    delete[] dataBuf;
    delete[] rateBuf;
    delete[] iqBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: qam_mapper <pipeInData> <pipeInRate> <pipeOutIQ>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3]};

    BlockConfig config = {
        "QamMapper",
        2,           // inputs
        1,           // outputs
        {3030, 3},   // inputPacketSizes  [INT_DATA, rate_enc_len]
        {64, 64},    // inputBatchSizes
        {129024},    // outputPacketSizes [IQ stacked: 504 symbols * 256 bytes]
        {64},        // outputBatchSizes
        false,       // ltr
        true,        // startWithAll
        "IEEE 802.11a QAM Mapper: interleaved bits -> OFDM subcarrier IQ symbols"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_qam_mapper, init_qam_mapper);
    return 0;
}