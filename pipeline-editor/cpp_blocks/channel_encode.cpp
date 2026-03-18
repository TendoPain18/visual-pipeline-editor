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
// Input  1: RATE/LIP pipe from scrambler     (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: lip_low   (length_in_pipe low byte)
//             Byte 2: lip_high  (length_in_pipe high byte)
//
// Output 0: Encoded DATA pipe                (3030 bytes/pkt, max)  <- FIRST
//             Layout: SIGNAL_ENCODED(6 bytes, 48 bits @ R=1/2)
//                   + DATA_ENCODED(variable, @ rate-determined R)
//
// Output 1: rate_encoded_length pipe         (3 bytes/pkt)
//             Byte 0: rate_value
//             Byte 1: encoded_length_low
//             Byte 2: encoded_length_high
//
// Encoder:
//   Generator polynomials: g0 = 1011011 (binary), g1 = 1111001 (binary)
//   (same as GNU Radio / IEEE 802.11a standard)
//
//   SIGNAL (first 3 bytes = 24 bits): always encoded at R=1/2 -> 48 bits = 6 bytes
//   DATA   (bytes [3..lip-1])        : encoded at rate from RATE pipe
//
// Rate -> Coding rate -> Puncture pattern:
//   13 (6  Mbps): R=1/2, no puncture
//   15 (9  Mbps): R=1/2, no puncture
//    5 (12 Mbps): R=2/3, pattern [1 1; 1 0]
//    7 (18 Mbps): R=2/3, pattern [1 1; 1 0]
//    9 (24 Mbps): R=3/4, pattern [1 1 0; 1 0 1]
//   11 (36 Mbps): R=3/4, pattern [1 1 0; 1 0 1]
//    1 (48 Mbps): R=2/3, pattern [1 1; 1 0]
//    3 (54 Mbps): R=3/4, pattern [1 1 0; 1 0 1]
//
// Output packet is zero-padded to max size (3030 bytes).
// Downstream uses rate_encoded_length to know actual byte count.
// ============================================================

// Debug: print detailed info only for the first N packets in the first batch
static const int DEBUG_PKT_LIMIT = 3;

// Generator polynomials (MSB first matching IEEE 802.11a)
// g0 = 1011011 = bits: [1,0,1,1,0,1,1]
// g1 = 1111001 = bits: [1,1,1,1,0,0,1]
// Shift register: new bit goes to position 0, oldest at position 6
static const int G0_TAPS[7] = {1, 0, 1, 1, 0, 1, 1};
static const int G1_TAPS[7] = {1, 1, 1, 1, 0, 0, 1};

// Precomputed encoder output table: for each (state6bit, inputBit) -> {outA, outB, nextState}
// state = lower 6 bits of shift register (positions [1..6], i.e. the memory)
// After encoding bit `inp`: new shift reg = [inp, state[0..4]], state becomes [inp, state[0..4]]
// nextState = lower 6 bits of new shift reg = (inp << 5) | (state >> 1) [if state is MSB-first]
// We store: encTable[state][inp] = (outA << 1) | outB packed as uint8, nextState as uint8
struct EncTableEntry {
    uint8_t outBits;   // bit1=outA, bit0=outB
    uint8_t nextState;
};

static EncTableEntry encTable[64][2];

static void build_enc_table() {
    for (int state = 0; state < 64; state++) {
        for (int inp = 0; inp <= 1; inp++) {
            // shift reg: reg[0]=inp, reg[1..6]=state bits 0..5
            int reg[7];
            reg[0] = inp;
            for (int b = 0; b < 6; b++) reg[b+1] = (state >> b) & 1;
            int outA = 0, outB = 0;
            for (int j = 0; j < 7; j++) {
                outA ^= (reg[j] & G0_TAPS[j]);
                outB ^= (reg[j] & G1_TAPS[j]);
            }
            outA &= 1; outB &= 1;
            // nextState: new shift reg positions [1..6] = old [0..5] = old reg[0..5]
            // = (inp << 5) | (state & 0x1F) won't work directly; it's reg[0..5] packed
            // reg[0]=inp, reg[1]=state[0], ..., reg[5]=state[4]
            // nextState bit b = reg[b] for b in [0..5]
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
        case 13: return RATE_1_2;  // 6 Mbps
        case 15: return RATE_3_4;  // 9 Mbps
        case  5: return RATE_1_2;  // 12 Mbps
        case  7: return RATE_3_4;  // 18 Mbps
        case  9: return RATE_1_2;  // 24 Mbps
        case 11: return RATE_3_4;  // 36 Mbps
        case  1: return RATE_2_3;  // 48 Mbps
        case  3: return RATE_3_4;  // 54 Mbps
        default: return RATE_2_3;  // fallback
    }
}

// -----------------------------------------------------------------------
// Byte-level encoder: works directly on packed bytes, no int bit arrays.
// Writes output bits packed LSB-first into outBuf (must be pre-zeroed).
// Returns number of output bits written.
// -----------------------------------------------------------------------

// R=1/2: every input bit -> outA, outB. Output 2 bits per input bit.
static int encode_half_rate_bytes(const uint8_t* inBytes, int numInBytes,
                                  uint8_t* outBuf, int state6) {
    int outBitIdx = 0;
    for (int i = 0; i < numInBytes; i++) {
        uint8_t b = inBytes[i];
        for (int bit = 0; bit < 8; bit++) {
            int inp = (b >> bit) & 1;
            const EncTableEntry& e = encTable[state6][inp];
            int outA = (e.outBits >> 1) & 1;
            int outB =  e.outBits       & 1;
            state6 = e.nextState;
            // write outA at outBitIdx, outB at outBitIdx+1
            outBuf[outBitIdx >> 3] |= (uint8_t)(outA << (outBitIdx & 7)); outBitIdx++;
            outBuf[outBitIdx >> 3] |= (uint8_t)(outB << (outBitIdx & 7)); outBitIdx++;
        }
    }
    return outBitIdx;
}

// R=2/3 puncture: pattern cols [1,1 / 1,0] -> keep A always, keep B on col0 only
// Output bits per 2 input bits: 3
static int encode_2_3_bytes(const uint8_t* inBytes, int numInBytes,
                             uint8_t* outBuf, int state6) {
    int outBitIdx = 0;
    int col = 0; // puncture column index (0 or 1)
    for (int i = 0; i < numInBytes; i++) {
        uint8_t b = inBytes[i];
        for (int bit = 0; bit < 8; bit++) {
            int inp = (b >> bit) & 1;
            const EncTableEntry& e = encTable[state6][inp];
            int outA = (e.outBits >> 1) & 1;
            int outB =  e.outBits       & 1;
            state6 = e.nextState;
            // R=2/3 pat23: rowA=[1,1] rowB=[1,0]
            // always keep outA
            outBuf[outBitIdx >> 3] |= (uint8_t)(outA << (outBitIdx & 7)); outBitIdx++;
            // keep outB only on col 0
            if (col == 0) {
                outBuf[outBitIdx >> 3] |= (uint8_t)(outB << (outBitIdx & 7)); outBitIdx++;
            }
            col ^= 1; // toggle 0/1
        }
    }
    return outBitIdx;
}

// R=3/4 puncture: pattern cols [1,1,0 / 1,0,1] -> 3 cols, 4 bits out per 3 in
// col0: keep A,B; col1: keep A only; col2: keep B only
static int encode_3_4_bytes(const uint8_t* inBytes, int numInBytes,
                             uint8_t* outBuf, int state6) {
    int outBitIdx = 0;
    int col = 0; // 0,1,2 cycling
    for (int i = 0; i < numInBytes; i++) {
        uint8_t b = inBytes[i];
        for (int bit = 0; bit < 8; bit++) {
            int inp = (b >> bit) & 1;
            const EncTableEntry& e = encTable[state6][inp];
            int outA = (e.outBits >> 1) & 1;
            int outB =  e.outBits       & 1;
            state6 = e.nextState;
            // R=3/4 pat34: rowA=[1,1,0] rowB=[1,0,1]
            if (col != 2) { // rowA[col]=1 for col 0,1
                outBuf[outBitIdx >> 3] |= (uint8_t)(outA << (outBitIdx & 7)); outBitIdx++;
            }
            if (col != 1) { // rowB[col]=1 for col 0,2
                outBuf[outBitIdx >> 3] |= (uint8_t)(outB << (outBitIdx & 7)); outBitIdx++;
            }
            col++; if (col == 3) col = 0;
        }
    }
    return outBitIdx;
}

// -----------------------------------------------------------------------
// Persistent state
// -----------------------------------------------------------------------
struct ChannelEncodeData {
    int frameCount;
    bool tableBuilt;
    // Pre-allocated working buffer (max encoded output: 1515*8*2 bits = 24240 bits = 3030 bytes)
    uint8_t encOutBuf[3030];
};

ChannelEncodeData init_channel_encode(const BlockConfig& config) {
    ChannelEncodeData data;
    data.frameCount = 0;
    data.tableBuilt = false;
    build_enc_table();
    data.tableBuilt = true;

    printf("[ChannelEncode] IEEE 802.11a Convolutional Encoder\n");
    printf("[ChannelEncode] g0=1011011, g1=1111001 (right-MSB), byte-level encoding\n");
    printf("[ChannelEncode] SIGNAL: always R=1/2 (24 bits -> 48 bits = 6 bytes)\n");
    printf("[ChannelEncode] DATA:   rate-determined (R=1/2, 2/3, or 3/4)\n");
    printf("[ChannelEncode] Output: SIGNAL_ENC(6B) + DATA_ENC(variable), zero-padded to 3030B\n");
    printf("[ChannelEncode] Debug: first %d packets of first batch logged\n", DEBUG_PKT_LIMIT);
    printf("[ChannelEncode] Ready\n");

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
    const int inRatePkt  = config.inputPacketSizes[1];   // 3
    const int outDataPkt = config.outputPacketSizes[0];  // 3030
    const int outRatePkt = config.outputPacketSizes[1];  // 3

    const bool isFirstBatch = (customData.frameCount == 0);

    // STEP 1: Read DATA batch first (rate measurement)
    int actualCount = inData.read(dataBuf);

    // STEP 2: Read RATE/LIP batch
    inRate.read(rateBuf);

    memset(rateOutBuf, 0, outRate.getBufferSize());
    memset(dataOutBuf, 0, outData.getBufferSize());

    for (int i = 0; i < actualCount; i++) {
        const bool dbg = isFirstBatch && (i < DEBUG_PKT_LIMIT);

        const int rateOff    = i * inRatePkt;
        const int dataOff    = i * inDataPkt;
        const int rateOutOff = i * outRatePkt;
        const int dataOutOff = i * outDataPkt;

        // Parse RATE/LIP
        uint8_t rateVal = (uint8_t)((int32_t)rateBuf[rateOff + 0] + 128);
        uint8_t lipLo   = (uint8_t)((int32_t)rateBuf[rateOff + 1] + 128);
        uint8_t lipHi   = (uint8_t)((int32_t)rateBuf[rateOff + 2] + 128);
        int lip = (int)lipLo | ((int)lipHi << 8);
        if (lip < 5)         lip = 5;
        if (lip > inDataPkt) lip = inDataPkt;

        if (dbg) {
            printf("[ChannelEncode] DBG pkt[%d] rateVal=%u lip=%d\n", i, (unsigned)rateVal, lip);
        }

        // Convert input bytes to uint8
        uint8_t pkt[1515];
        for (int j = 0; j < lip; j++)
            pkt[j] = (uint8_t)((int32_t)dataBuf[dataOff + j] + 128);

        if (dbg) {
            printf("[ChannelEncode] DBG pkt[%d] SIGNAL raw bytes: %02X %02X %02X\n",
                   i, pkt[0], pkt[1], pkt[2]);
        }

        // ----- Encode SIGNAL (first 3 bytes = 24 bits) at R=1/2 -> 48 bits = 6 bytes -----
        uint8_t signalEncBytes[6] = {0};
        encode_half_rate_bytes(pkt, 3, signalEncBytes, 0 /*shiftReg starts at 0*/);

        if (dbg) {
            printf("[ChannelEncode] DBG pkt[%d] SIGNAL_ENC: %02X %02X %02X %02X %02X %02X\n",
                   i, signalEncBytes[0], signalEncBytes[1], signalEncBytes[2],
                      signalEncBytes[3], signalEncBytes[4], signalEncBytes[5]);
        }

        // ----- Encode DATA (bytes [3..lip-1]) at rate-determined coding rate -----
        int dataFieldBytes = lip - 3;
        if (dataFieldBytes < 0) dataFieldBytes = 0;

        int encDataBits = 0;
        uint8_t dataEncBytes[3024] = {0};

        if (dataFieldBytes > 0) {
            // Each DATA field is encoded with its own fresh shift register (state=0)
            // matching the encoder's per-field independence
            memset(dataEncBytes, 0, sizeof(dataEncBytes));
            CodingRate cr = rateValueToCodingRate(rateVal);
            if (cr == RATE_1_2) {
                encDataBits = encode_half_rate_bytes(pkt + 3, dataFieldBytes, dataEncBytes, 0);
            } else if (cr == RATE_2_3) {
                encDataBits = encode_2_3_bytes(pkt + 3, dataFieldBytes, dataEncBytes, 0);
            } else {
                encDataBits = encode_3_4_bytes(pkt + 3, dataFieldBytes, dataEncBytes, 0);
            }

            if (dbg) {
                int encDataBytes_dbg = (encDataBits + 7) / 8;
                printf("[ChannelEncode] DBG pkt[%d] rate=%s dataFieldBytes=%d "
                       "encDataBits=%d encDataBytes=%d\n",
                       i,
                       (cr == RATE_1_2 ? "1/2" : cr == RATE_2_3 ? "2/3" : "3/4"),
                       dataFieldBytes, encDataBits, encDataBytes_dbg);
                printf("[ChannelEncode] DBG pkt[%d] DATA_ENC first bytes: "
                       "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                       i,
                       dataEncBytes[0], dataEncBytes[1], dataEncBytes[2], dataEncBytes[3],
                       dataEncBytes[4], dataEncBytes[5], dataEncBytes[6], dataEncBytes[7]);
            }
        }

        int encDataBytes = (encDataBits + 7) / 8;
        int totalEncBytes = 6 + encDataBytes;

        if (dbg) {
            printf("[ChannelEncode] DBG pkt[%d] totalEncBytes=%d\n", i, totalEncBytes);
        }

        // ----- Write rate_encoded_length -----
        rateOutBuf[rateOutOff + 0] = (int8_t)((int32_t)rateVal                       - 128);
        rateOutBuf[rateOutOff + 1] = (int8_t)((int32_t)(totalEncBytes & 0xFF)        - 128);
        rateOutBuf[rateOutOff + 2] = (int8_t)((int32_t)((totalEncBytes >> 8) & 0xFF) - 128);

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
        {1515, 3},              // inputPacketSizes  [DATA_max, RATE/LIP]
        {6000, 6000},         // inputBatchSizes
        {3030, 3},              // outputPacketSizes [ENC_DATA_max, rate_enc_len]
        {6000, 6000},         // outputBatchSizes
        true,                   // ltr
        true,                   // startWithAll
        "IEEE 802.11a convolutional encoder: SIGNAL@R=1/2, DATA@variable rate"
    };

    run_manual_block(pipeIns, pipeOuts, config, process_channel_encode, init_channel_encode);
    return 0;
}