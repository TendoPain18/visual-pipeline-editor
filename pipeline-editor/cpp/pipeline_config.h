#ifndef PIPELINE_CONFIG_H
#define PIPELINE_CONFIG_H

// Auto-generated from GUI
// Generated: 2/6/2026, 1:17:24 PM

#define CONFIG_VERSION "2.0"
#define NUM_BUFFERS 3
#define NUM_BLOCKS 4

static const unsigned long long BASE_FRAME_SIZE_BYTES = 8ULL;

static const unsigned long long BUFFER_SIZES[NUM_BUFFERS] = {
    8ULL,  // GlobalP1 -> 0.00 MB
    8ULL,  // GlobalP2 -> 0.00 MB
    8ULL  // GlobalP3 -> 0.00 MB
};

static const char* PIPE_NAMES[NUM_BUFFERS] = {
    "GlobalP1",
    "GlobalP2",
    "GlobalP3"
};

// Pipeline blocks:
//  1. QAM64Constellation   : Generates 64-QAM constellation (I and Q with noise)
//  2. Source               : Data source block
//  3. Adder                : Adds two input numbers
//  4. Sink                 : Data sink block

#endif // PIPELINE_CONFIG_H
