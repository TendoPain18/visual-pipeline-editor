export const generateCppHeader = (sortedBlocks, connections) => {
  const numBuffers = connections.length;
  const pipes = connections.map((_, i) => `GlobalP${i + 1}`);
  
  let content = `#ifndef PIPELINE_CONFIG_H\n#define PIPELINE_CONFIG_H\n\n`;
  content += `// Auto-generated from GUI\n`;
  content += `// Generated: ${new Date().toLocaleString()}\n\n`;
  
  content += `#define CONFIG_VERSION "2.0"\n`;
  content += `#define NUM_BUFFERS ${numBuffers}\n`;
  content += `#define NUM_BLOCKS ${sortedBlocks.length}\n\n`;
  
  // Get base size - handle both single values and arrays
  let baseSize = 67108864; // Default 64MB
  if (sortedBlocks.length > 0) {
    const firstBlock = sortedBlocks[0];
    if (Array.isArray(firstBlock.outputSize)) {
      // If array, use first element
      baseSize = firstBlock.outputSize[0] || baseSize;
    } else {
      baseSize = firstBlock.outputSize || baseSize;
    }
  }
  
  content += `static const unsigned long long BASE_FRAME_SIZE_BYTES = ${baseSize}ULL;\n\n`;
  
  content += `static const unsigned long long BUFFER_SIZES[NUM_BUFFERS] = {\n`;
  connections.forEach((conn, i) => {
    const fromBlock = sortedBlocks.find(b => b.id === conn.fromBlock);
    
    // Get size for specific output port
    let size = baseSize;
    if (fromBlock) {
      if (Array.isArray(fromBlock.outputSize)) {
        // Use the size for the specific output port
        size = fromBlock.outputSize[conn.fromPort] || fromBlock.outputSize[0] || baseSize;
      } else {
        size = fromBlock.outputSize || baseSize;
      }
    }
    
    content += `    ${size}ULL${i < numBuffers - 1 ? ',' : ''}  // ${pipes[i]} -> ${(size / 1024 / 1024).toFixed(2)} MB\n`;
  });
  content += `};\n\n`;
  
  content += `static const char* PIPE_NAMES[NUM_BUFFERS] = {\n`;
  pipes.forEach((pipe, i) => {
    content += `    "${pipe}"${i < pipes.length - 1 ? ',' : ''}\n`;
  });
  content += `};\n\n`;
  
  content += `// Pipeline blocks:\n`;
  sortedBlocks.forEach((block, i) => {
    content += `//  ${i + 1}. ${block.name.padEnd(20)} : ${block.description}\n`;
  });
  content += `\n#endif // PIPELINE_CONFIG_H\n`;
  
  return content;
};

export const generateCppServer = (sortedBlocks, connections) => {
  const numBuffers = connections.length;
  
  let content = `// Auto-generated pipe server\n`;
  content += `// Generated: ${new Date().toLocaleString()}\n\n`;
  content += `#include <windows.h>\n`;
  content += `#include <stdio.h>\n`;
  content += `#include <signal.h>\n`;
  content += `#include "pipeline_config.h"\n\n`;
  
  content += `HANDLE handles[NUM_BUFFERS * 3];\n`;
  content += `volatile bool running = true;\n\n`;
  
  content += `void cleanup(int sig) {\n`;
  content += `    printf("\\nShutting down...\\n");\n`;
  content += `    running = false;\n`;
  content += `}\n\n`;
  
  content += `void print_config() {\n`;
  content += `    printf("========================================\\n");\n`;
  content += `    printf("PIPE SERVER - Configuration\\n");\n`;
  content += `    printf("========================================\\n");\n`;
  content += `    printf("Version:     %s\\n", CONFIG_VERSION);\n`;
  content += `    printf("Num Blocks:  %d\\n", NUM_BLOCKS);\n`;
  content += `    printf("Num Buffers: %d\\n", NUM_BUFFERS);\n`;
  content += `    printf("========================================\\n\\n");\n`;
  content += `}\n\n`;
  
  content += `int main() {\n`;
  content += `    signal(SIGINT, cleanup);\n`;
  content += `    print_config();\n\n`;
  
  content += `    SECURITY_ATTRIBUTES sa;\n`;
  content += `    SECURITY_DESCRIPTOR sd;\n`;
  content += `    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);\n`;
  content += `    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);\n`;
  content += `    sa.nLength = sizeof(SECURITY_ATTRIBUTES);\n`;
  content += `    sa.lpSecurityDescriptor = &sd;\n`;
  content += `    sa.bInheritHandle = FALSE;\n\n`;
  
  content += `    int idx = 0;\n`;
  content += `    for (int i = 0; i < NUM_BUFFERS; i++) {\n`;
  content += `        handles[idx] = CreateFileMappingA(\n`;
  content += `            INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,\n`;
  content += `            (DWORD)(BUFFER_SIZES[i] >> 32),\n`;
  content += `            (DWORD)(BUFFER_SIZES[i] & 0xFFFFFFFF),\n`;
  content += `            PIPE_NAMES[i]\n`;
  content += `        );\n`;
  content += `        if (!handles[idx]) {\n`;
  content += `            printf("ERROR: Failed to create %s\\n", PIPE_NAMES[i]);\n`;
  content += `            return 1;\n`;
  content += `        }\n`;
  content += `        printf("Created: %s (%.2f MB)\\n", PIPE_NAMES[i], BUFFER_SIZES[i] / (1024.0 * 1024.0));\n`;
  content += `        idx++;\n\n`;
  content += `        char rName[128], eName[128];\n`;
  content += `        sprintf(rName, "%s_Ready", PIPE_NAMES[i]);\n`;
  content += `        sprintf(eName, "%s_Empty", PIPE_NAMES[i]);\n`;
  content += `        handles[idx++] = CreateEventA(&sa, FALSE, FALSE, rName);\n`;
  content += `        handles[idx++] = CreateEventA(&sa, FALSE, TRUE, eName);\n`;
  content += `    }\n\n`;
  
  content += `    printf("\\nHIGHWAY: System UP (%d buffers)\\n", NUM_BUFFERS);\n`;
  content += `    printf("Press Ctrl+C to shutdown\\n\\n");\n\n`;
  
  content += `    while (running) Sleep(1000);\n\n`;
  
  content += `    printf("\\nCleaning up...\\n");\n`;
  content += `    for (int i = 0; i < idx; i++) {\n`;
  content += `        if (handles[i]) CloseHandle(handles[i]);\n`;
  content += `    }\n`;
  content += `    return 0;\n`;
  content += `}\n`;
  
  return content;
};
