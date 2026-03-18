#include "core/run_generic_block.h"
#include <cstring>
#include <cstdint>

// ============================================================
// IEEE 802.11a Convolutional Channel Encoder
//
// Input  0: DATA pipe from scrambler         (1515 bytes/pkt, max) <- [0] FIRST for rate counter
//             Layout: SIGNAL(3) + SERVICE(2) + PSDU + TAIL_PAD
//             Only lip bytes are meaningful.
//
// Input  1: RATE/LIP pipe from scrambler     (5 bytes/pkt)
//             Byte 0:   rate_value
//             Bytes 1-4: lipBits (uint32 LE) -- exact PPDU frame bit count
//                        (includes SIGNAL 24b + SERVICE + PSDU + TAIL + PAD)
//
// Output 0: Encoded DATA pipe                (3030 bytes/pkt, max)  <- FIRST
//             Layout: SIGNAL_ENCODED(6 bytes, 48 bits @ R=1/2)
//                   + DATA_ENCODED(variable, @ rate-determined R)
//
// Output 1: rate_encoded_length pipe         (5 bytes/pkt)
//             Byte 0:   rate_value
//             Bytes 1-4: encoded_len_bits (uint32 LE) -- EXACT total encoded bit count
//                        = 48 (SIGNAL, R=1/2) + encDataBits (DATA field at coded rate)
//
// Encoder:
//   Generator polynomials: g0 = 1011011 (binary), g1 = 1111001 (binary)
//   (same as GNU Radio / IEEE 802.11a standard)
//
//   SIGNAL (first 24 bits): always encoded at R=1/2 -> 48 bits = 6 bytes
//   DATA   (bits [24..lipBits-1]): encoded at rate from RATE pipe, exact bit count
//
// Rate -> Coding rate -> Puncture pattern:
//   13 (6  Mbps): R=1/2, no puncture
//   15 (9  Mbps): R=1/2, no puncture
//    5 (12 Mbps): R=2/3, pattern [1 1; 1 0]
//    7 (18 Mbps): R=2/3, pattern [1 1; 1 0]
//    9 (24 Mbps): R=1/2, no puncture
//   11 (36 Mbps): R=3/4, pattern [1 1 0; 1 0 1]
//    1 (48 Mbps): R=2/3, pattern [1 1; 1 0]
//    3 (54 Mbps): R=3/4, pattern [1 1 0; 1 0 1]
//
// Output packet is zero-padded to max size (3030 bytes).
// Downstream (interleaver) uses encoded_len_bits (uint32 LE) to know exact bit count.
// ============================================================

// Generator polynomials (MSB first matching IEEE 802.11a)
// g0 = 1011011 = bits: [1,0,1,1,0,1,1]
// g1 = 1111001 = bits: [1,1,1,1,0,0,1]
// Shift register: new bit goes to position 0, oldest at position 6
static const int G0_TAPS[7] = {1, 0, 1, 1, 0, 1, 1};
static const int G1_TAPS[7] = {1, 1, 1, 1, 0, 0, 1};

// Precomputed encoder output table: for each (state6bit, inputBit) -> {outA, outB, nextState}
struct EncTableEntry {
    uint8_t outBits;   // bit1=outA, bit0=outB
    uint8_t nextState;
};

static EncTableEntry encTable[64][2];

static void build_enc_table() {
    for (int state = 0; state < 64; state++) {
        for (int inp = 0; inp <= 1; inp++) {
            int reg[7];
            reg[0] = inp;
            for (int b = 0; b < 6; b++) reg[b+1] = (state >> b) & 1;
            int outA = 0, outB = 0;
            for (int j = 0; j < 7; j++) {
                outA ^= (reg[j] & G0_TAPS[j]);
                outB ^= (reg[j] & G1_TAPS[j]);
            }
            outA &= 1; outB &= 1;
            int ns = 0;
            for (int b = 0; b < 6; b++) ns |= (reg[b] << b);
            encTable[state][inp].outBits   = (uint8_t)((outA << 1) | outB);
            encTable[state][inp].nextState = (uint8_t)ns;
        }
    }
}

// Coding rate type
enum CodingRate { RATE_1_2, RATE_2_3, RATE_3_4 };

static CodingRate rateValueToCodingRate(uint8_t rateVal) {
    switch (rateVal) {
        case 13: return RATE_1_2;  // 6 Mbps  BPSK   R=1/2
        case 15: return RATE_3_4;  // 9 Mbps  BPSK   R=3/4
        case  5: return RATE_1_2;  // 12 Mbps QPSK   R=1/2
        case  7: return RATE_3_4;  // 18 Mbps QPSK   R=3/4
        case  9: return RATE_1_2;  // 24 Mbps 16-QAM R=1/2
        case 11: return RATE_3_4;  // 36 Mbps 16-QAM R=3/4
        case  1: return RATE_2_3;  // 48 Mbps 64-QAM R=2/3
        case  3: return RATE_3_4;  // 54 Mbps 64-QAM R=3/4
        default: return RATE_1_2;  // fallback
    }
}

// -----------------------------------------------------------------------
// Bit-granularity encoder helpers.
// All functions take an exact numInBits count and encode only those bits,
// reading from packed LSB-first bytes in inBytes.
// Output bits are written packed LSB-first into outBuf (must be pre-zeroed).
// Returns number of output bits written.
// -----------------------------------------------------------------------

// Helper: read single bit from packed byte array (LSB-first)
static inline int read_bit(const uint8_t* buf, int bitIdx) {
    return (buf[bitIdx >> 3] >> (bitIdx & 7)) & 1;
}

// Helper: write single bit into packed byte array (LSB-first)
static inline void write_bit(uint8_t* buf, int bitIdx, int val) {
    if (val) buf[bitIdx >> 3] |= (uint8_t)(1 << (bitIdx & 7));
}

// R=1/2: every input bit -> outA, outB. Output 2 bits per input bit.
static int encode_half_rate_bits(const uint8_t* inBytes, int numInBits,
                                 uint8_t* outBuf, int state6) {
    int outBitIdx = 0;
    for (int i = 0; i < numInBits; i++) {
        int inp = read_bit(inBytes, i);
        const EncTableEntry& e = encTable[state6][inp];
        write_bit(outBuf, outBitIdx++, (e.outBits >> 1) & 1);  // outA
        write_bit(outBuf, outBitIdx++,  e.outBits       & 1);  // outB
        state6 = e.nextState;
    }
    return outBitIdx;
}

// R=2/3 puncture: pattern cols [1,1 / 1,0] -> keep A always, keep B on col0 only
// Output 3 bits per 2 input bits.
static int encode_2_3_bits(const uint8_t* inBytes, int numInBits,
                            uint8_t* outBuf, int state6) {
    int outBitIdx = 0;
    int col = 0;
    for (int i = 0; i < numInBits; i++) {
        int inp = read_bit(inBytes, i);
        const EncTableEntry& e = encTable[state6][inp];
        int outA = (e.outBits >> 1) & 1;
        int outB =  e.outBits       & 1;
        state6 = e.nextState;
        // always keep outA
        write_bit(outBuf, outBitIdx++, outA);
        // keep outB only on col 0
        if (col == 0) {
            write_bit(outBuf, outBitIdx++, outB);
        }
        col ^= 1;
    }
    return outBitIdx;
}

// R=3/4 puncture: pattern cols [1,1,0 / 1,0,1] -> 4 bits out per 3 input bits
// col0: keep A,B; col1: keep A only; col2: keep B only
static int encode_3_4_bits(const uint8_t* inBytes, int numInBits,
                            uint8_t* outBuf, int state6) {
    int outBitIdx = 0;
    int col = 0;
    for (int i = 0; i < numInBits; i++) {
        int inp = read_bit(inBytes, i);
        const EncTableEntry& e = encTable[state6][inp];
        int outA = (e.outBits >> 1) & 1;
        int outB =  e.outBits       & 1;
        state6 = e.nextState;
        if (col != 2) write_bit(outBuf, outBitIdx++, outA);  // rowA[col]=1 for col 0,1
        if (col != 1) write_bit(outBuf, outBitIdx++, outB);  // rowB[col]=1 for col 0,2
        col++; if (col == 3) col = 0;
    }
    return outBitIdx;
}

// -----------------------------------------------------------------------
// Persistent state
// -----------------------------------------------------------------------
struct ChannelEncodeData {
    int frameCount;
    bool tableBuilt;
};

ChannelEncodeData init_channel_encode(const BlockConfig& config) {
    ChannelEncodeData data;
    data.frameCount = 0;
    data.tableBuilt = false;
    build_enc_table();
    data.tableBuilt = true;
    return data;
}

void process_channel_encode(
    const char** pipeIn,
    const char** pipeOut,
    ChannelEncodeData& customData,
    const BlockConfig& config
) {
    PipeIO inData    (pipeIn[0],  config.inputPacketSizes[0],  config.inputBatchSizes[0]);
    PipeIO inRate    (pipeIn[1],  config.inputPacketSizes[1],  config.inputBatchSizes[1]);
    PipeIO outData   (pipeOut[0], config.outputPacketSizes[0], config.outputBatchSizes[0]);
    PipeIO outRate   (pipeOut[1], config.outputPacketSizes[1], config.outputBatchSizes[1]);

    int8_t* rateBuf    = new int8_t[inRate.getBufferSize()];
    int8_t* dataBuf    = new int8_t[inData.getBufferSize()];
    int8_t* rateOutBuf = new int8_t[outRate.getBufferSize()];
    int8_t* dataOutBuf = new int8_t[outData.getBufferSize()];

    const int inDataPkt  = config.inputPacketSizes[0];   // 1515
    const int inRatePkt  = config.inputPacketSizes[1];   // 5
    const int outDataPkt = config.outputPacketSizes[0];  // 3030
    const int outRatePkt = config.outputPacketSizes[1];  // 5

    const bool isFirstBatch = (customData.frameCount == 0);

    // STEP 1: Read DATA batch first (rate measurement)
    int actualCount = inData.read(dataBuf);

    // STEP 2: Read RATE/LIP batch
    inRate.read(rateBuf);

    memset(rateOutBuf, 0, outRate.getBufferSize());
    memset(dataOutBuf, 0, outData.getBufferSize());

    for (int i = 0; i < actualCount; i++) {
        const bool dbg = isFirstBatch && (i == 0);

        const int rateOff    = i * inRatePkt;
        const int dataOff    = i * inDataPkt;
        const int rateOutOff = i * outRatePkt;
        const int dataOutOff = i * outDataPkt;

        // Parse RATE/LIP: 5-byte pipe [rate_val(1) | lipBits_uint32_LE(4)]
        // lipBits = EXACT bit count from upstream (SIGNAL 24b + SERVICE + PSDU + TAIL + PAD)
        uint8_t rateVal = (uint8_t)((int32_t)rateBuf[rateOff + 0] + 128);
        uint8_t rb1     = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t rb2     = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        uint8_t rb3     = (uint8_t)((int32_t)rateBuf[rateOff + 3] + 128);
        uint8_t rb4     = (uint8_t)((int32_t)rateBuf[rateOff + 4] + 128);
        uint32_t lipBitsIn = (uint32_t)rb1
                           | ((uint32_t)rb2 <<  8)
                           | ((uint32_t)rb3 << 16)
                           | ((uint32_t)rb4 << 24);

        // Use EXACT bit counts — no rounding to bytes
        const int totalInBits   = (int)lipBitsIn;
        const int signalInBits  = 24;                              // always first 24 bits
        const int dataInBits    = totalInBits - signalInBits;      // exact DATA bit count

        // How many bytes we need to read from the pipe buffer
        int lipBytes = (totalInBits + 7) / 8;
        if (lipBytes < 5)         lipBytes = 5;
        if (lipBytes > inDataPkt) lipBytes = inDataPkt;

        // Convert input bytes to uint8
        uint8_t pkt[1515];
        for (int j = 0; j < lipBytes; j++)
            pkt[j] = (uint8_t)((int32_t)dataBuf[dataOff + j] + 128);

        // ----- Encode SIGNAL: exactly 24 input bits at R=1/2 -> 48 output bits -----
        uint8_t signalEncBytes[6] = {0};
        encode_half_rate_bits(pkt, signalInBits, signalEncBytes, 0);
        // signalEncBits is always 48

        // ----- Encode DATA: exactly dataInBits input bits at coded rate -----
        int encDataBits = 0;
        uint8_t dataEncBytes[3024] = {0};

        if (dataInBits > 0) {
            // DATA field starts at bit offset 24 in pkt.
            // Pass pkt + 3 (byte-aligned at bit 24) and read dataInBits bits from it.
            memset(dataEncBytes, 0, sizeof(dataEncBytes));
            CodingRate cr = rateValueToCodingRate(rateVal);
            if (cr == RATE_1_2) {
                encDataBits = encode_half_rate_bits(pkt + 3, dataInBits, dataEncBytes, 0);
            } else if (cr == RATE_2_3) {
                encDataBits = encode_2_3_bits(pkt + 3, dataInBits, dataEncBytes, 0);
            } else {
                encDataBits = encode_3_4_bits(pkt + 3, dataInBits, dataEncBytes, 0);
            }
        }

        int encDataBytes = (encDataBits + 7) / 8;

        if (dbg) {
            printf("[ChannelEncode] pkt[0] INPUT : signal=%d  data=%d  total=%d bits\n",
                   signalInBits, dataInBits, totalInBits);
            printf("[ChannelEncode] pkt[0] OUTPUT: signal=%d  data=%d  total=%d bits\n",
                   48, encDataBits, 48 + encDataBits);
            fflush(stdout);
        }

        // ----- Write rate_encoded_length (5 bytes: rate_val + encBits uint32 LE) -----
        // encBits = 48 (SIGNAL encoded) + encDataBits (DATA encoded) — exact, no rounding
        uint32_t encBits = (uint32_t)(48 + encDataBits);
        rateOutBuf[rateOutOff + 0] = (int8_t)((int32_t)rateVal                      - 128);
        rateOutBuf[rateOutOff + 1] = (int8_t)((int32_t)( encBits        & 0xFF)     - 128);
        rateOutBuf[rateOutOff + 2] = (int8_t)((int32_t)((encBits >>  8) & 0xFF)     - 128);
        rateOutBuf[rateOutOff + 3] = (int8_t)((int32_t)((encBits >> 16) & 0xFF)     - 128);
        rateOutBuf[rateOutOff + 4] = (int8_t)((int32_t)((encBits >> 24) & 0xFF)     - 128);

        // ----- Write encoded output packet (zero-padded to 3030 bytes) -----
        for (int j = 0; j < 6; j++)
            dataOutBuf[dataOutOff + j] = (int8_t)((int32_t)signalEncBytes[j] - 128);
        for (int j = 0; j < encDataBytes; j++)
            dataOutBuf[dataOutOff + 6 + j] = (int8_t)((int32_t)dataEncBytes[j] - 128);
        // remaining bytes in the packet are already 0 from memset above
    }

    // STEP 3: Send encoded DATA first, then rate_encoded_length
    outData.write(dataOutBuf, actualCount);
    outRate.write(rateOutBuf, actualCount);

    customData.frameCount += actualCount;

    delete[] rateBuf;
    delete[] dataBuf;
    delete[] rateOutBuf;
    delete[] dataOutBuf;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: channel_encode <pipeInData> <pipeInRate>"
            " <pipeOutData> <pipeOutRate>\n");
        return 1;
    }

    const char* pipeIns[]  = {argv[1], argv[2]};
    const char* pipeOuts[] = {argv[3], argv[4]};

    BlockConfig config = {
        "ChannelEncode",
        2,                      // inputs
        2,                      // outputs
        {1515, 5},              // inputPacketSizes  [DATA_max, RATE/LIP 5B (rate+lipBits uint32 LE)]
        {64, 64},               // inputBatchSizes
        {3030, 5},              // outputPacketSizes [ENC_DATA_max, rate_enc_len_bits (uint32 LE)]
        {64, 64},               // outputBatchSizes
        true,                   // ltr
        true,                   // startWithAll
        "IEEE 802.11a convolutional encoder: SIGNAL@R=1/2, DATA@variable rate, exact bit granularity"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_channel_encode, init_channel_encode);
    return 0;
}