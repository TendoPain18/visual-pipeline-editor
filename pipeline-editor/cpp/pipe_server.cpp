// Parameterized Pipe Server - No recompilation needed!
// Usage: pipe_server.exe <num_buffers> <buffer1_name> <buffer1_size> <buffer2_name> <buffer2_size> ...
// Generated: One-time compilation

#include <windows.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BUFFERS 100
#define MAX_NAME_LEN 128

struct PipeInfo {
    char name[MAX_NAME_LEN];
    unsigned long long size;
    HANDLE hMapFile;
    HANDLE hReady;
    HANDLE hEmpty;
};

PipeInfo pipes[MAX_BUFFERS];
int numBuffers = 0;
volatile bool running = true;

void cleanup(int sig) {
    printf("\nShutting down...\n");
    running = false;
}

void print_usage() {
    printf("Usage: pipe_server.exe <num_buffers> <name1> <size1> <name2> <size2> ...\n");
    printf("Example: pipe_server.exe 2 GlobalP1 67108864 GlobalP2 67108864\n");
    exit(1);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, cleanup);
    
    // Parse arguments
    if (argc < 2) {
        print_usage();
    }
    
    numBuffers = atoi(argv[1]);
    
    if (numBuffers <= 0 || numBuffers > MAX_BUFFERS) {
        printf("Error: Invalid number of buffers (must be 1-%d)\n", MAX_BUFFERS);
        return 1;
    }
    
    if (argc != 2 + numBuffers * 2) {
        printf("Error: Expected %d arguments (name and size for each buffer)\n", numBuffers * 2);
        print_usage();
    }
    
    // Parse pipe configurations
    for (int i = 0; i < numBuffers; i++) {
        int argIdx = 2 + i * 2;
        strncpy(pipes[i].name, argv[argIdx], MAX_NAME_LEN - 1);
        pipes[i].name[MAX_NAME_LEN - 1] = '\0';
        pipes[i].size = strtoull(argv[argIdx + 1], NULL, 10);
        
        if (pipes[i].size == 0) {
            printf("Error: Invalid size for buffer %d\n", i + 1);
            return 1;
        }
    }
    
    printf("========================================\n");
    printf("PIPE SERVER - Parameterized Version\n");
    printf("========================================\n");
    printf("Buffers: %d\n", numBuffers);
    printf("========================================\n\n");
    
    // Create security attributes
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    
    // Create all pipes
    for (int i = 0; i < numBuffers; i++) {
        // Create shared memory
        pipes[i].hMapFile = CreateFileMappingA(
            INVALID_HANDLE_VALUE, 
            &sa, 
            PAGE_READWRITE,
            (DWORD)(pipes[i].size >> 32),
            (DWORD)(pipes[i].size & 0xFFFFFFFF),
            pipes[i].name
        );
        
        if (!pipes[i].hMapFile) {
            printf("ERROR: Failed to create %s (Error: %lu)\n", pipes[i].name, GetLastError());
            
            // Cleanup already created pipes
            for (int j = 0; j < i; j++) {
                if (pipes[j].hMapFile) CloseHandle(pipes[j].hMapFile);
                if (pipes[j].hReady) CloseHandle(pipes[j].hReady);
                if (pipes[j].hEmpty) CloseHandle(pipes[j].hEmpty);
            }
            return 1;
        }
        
        printf("Created: %s (%.2f MB)\n", pipes[i].name, pipes[i].size / (1024.0 * 1024.0));
        
        // Create events
        char readyName[MAX_NAME_LEN + 10];
        char emptyName[MAX_NAME_LEN + 10];
        sprintf(readyName, "%s_Ready", pipes[i].name);
        sprintf(emptyName, "%s_Empty", pipes[i].name);
        
        pipes[i].hReady = CreateEventA(&sa, FALSE, FALSE, readyName);
        pipes[i].hEmpty = CreateEventA(&sa, FALSE, TRUE, emptyName);
        
        if (!pipes[i].hReady || !pipes[i].hEmpty) {
            printf("ERROR: Failed to create events for %s\n", pipes[i].name);
            
            // Cleanup
            for (int j = 0; j <= i; j++) {
                if (pipes[j].hMapFile) CloseHandle(pipes[j].hMapFile);
                if (pipes[j].hReady) CloseHandle(pipes[j].hReady);
                if (pipes[j].hEmpty) CloseHandle(pipes[j].hEmpty);
            }
            return 1;
        }
    }
    
    printf("\nHIGHWAY: System UP (%d buffers)\n", numBuffers);
    printf("Press Ctrl+C to shutdown\n\n");
    
    while (running) {
        Sleep(1000);
    }
    
    printf("\nCleaning up...\n");
    for (int i = 0; i < numBuffers; i++) {
        if (pipes[i].hMapFile) CloseHandle(pipes[i].hMapFile);
        if (pipes[i].hReady) CloseHandle(pipes[i].hReady);
        if (pipes[i].hEmpty) CloseHandle(pipes[i].hEmpty);
    }
    
    printf("Shutdown complete\n");
    return 0;
}
