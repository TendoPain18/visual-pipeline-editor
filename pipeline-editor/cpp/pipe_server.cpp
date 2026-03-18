// Auto-generated pipe server
// Generated: 2/6/2026, 1:17:24 PM

#include <windows.h>
#include <stdio.h>
#include <signal.h>
#include "pipeline_config.h"

HANDLE handles[NUM_BUFFERS * 3];
volatile bool running = true;

void cleanup(int sig) {
    printf("\nShutting down...\n");
    running = false;
}

void print_config() {
    printf("========================================\n");
    printf("PIPE SERVER - Configuration\n");
    printf("========================================\n");
    printf("Version:     %s\n", CONFIG_VERSION);
    printf("Num Blocks:  %d\n", NUM_BLOCKS);
    printf("Num Buffers: %d\n", NUM_BUFFERS);
    printf("========================================\n\n");
}

int main() {
    signal(SIGINT, cleanup);
    print_config();

    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    int idx = 0;
    for (int i = 0; i < NUM_BUFFERS; i++) {
        handles[idx] = CreateFileMappingA(
            INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
            (DWORD)(BUFFER_SIZES[i] >> 32),
            (DWORD)(BUFFER_SIZES[i] & 0xFFFFFFFF),
            PIPE_NAMES[i]
        );
        if (!handles[idx]) {
            printf("ERROR: Failed to create %s\n", PIPE_NAMES[i]);
            return 1;
        }
        printf("Created: %s (%.2f MB)\n", PIPE_NAMES[i], BUFFER_SIZES[i] / (1024.0 * 1024.0));
        idx++;

        char rName[128], eName[128];
        sprintf(rName, "%s_Ready", PIPE_NAMES[i]);
        sprintf(eName, "%s_Empty", PIPE_NAMES[i]);
        handles[idx++] = CreateEventA(&sa, FALSE, FALSE, rName);
        handles[idx++] = CreateEventA(&sa, FALSE, TRUE, eName);
    }

    printf("\nHIGHWAY: System UP (%d buffers)\n", NUM_BUFFERS);
    printf("Press Ctrl+C to shutdown\n\n");

    while (running) Sleep(1000);

    printf("\nCleaning up...\n");
    for (int i = 0; i < idx; i++) {
        if (handles[i]) CloseHandle(handles[i]);
    }
    return 0;
}
