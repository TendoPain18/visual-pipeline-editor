function file_source(pipeOut)
% FILE_SOURCE - Progressive file loading using generic block framework

    % ========== USER CONFIGURATION ==========
    block_config = struct( ...
        'name',              'FileSource', ...
        'inputs',            0, ...
        'outputs',           1, ...
        'outputPacketSizes', 1500, ...
        'outputBatchSizes',  44740, ...
        'LTR',               true, ...
        'startWithAll',      false, ...
        'socketHost',        'localhost', ...
        'socketPort',        9001, ...
        'sourceDirectory',   'C:\Users\amrga\Downloads\final\pipeline-editor\Test_Files', ...
        'description',       'Progressive file loading with continuous buffering' ...
    );
    
    % ========== CUSTOM INITIALIZATION ==========
    init_fn = @(config) initialize_file_source(config);
    
    % ========== PROCESSING FUNCTION ==========
    process_fn = @(inputBatch, actualCount, data, config) ...
        process_file_source_persistent(inputBatch, actualCount, data, config);
    
    % ========== RUN GENERIC BLOCK ==========
    run_generic_block([], pipeOut, block_config, process_fn, init_fn);
end

function customData = initialize_file_source(config)
    % FILE PROTOCOL CONSTANTS
    START_FLAG = uint8([0xAA, 0x55, 0xAA, 0x55]);
    END_FLAG = uint8([0x55, 0xAA, 0x55, 0xAA]);
    FILENAME_LENGTH = 256;
    REPETITIONS = 10;
    
    if ~exist(config.sourceDirectory, 'dir')
        mkdir(config.sourceDirectory);
        error('Created source directory. Add files and restart.');
    end
    
    fileList = dir(fullfile(config.sourceDirectory, '*.*'));
    fileList = fileList(~[fileList.isdir]);
    
    if isempty(fileList)
        error('No files found in source directory.');
    end
    
    fprintf('Found %d file(s) to send\n', length(fileList));
    
    customData = struct();
    customData.fileList = fileList;
    customData.currentFileIdx = 1;
    customData.streamBuffer = [];
    customData.sourceDirectory = config.sourceDirectory;
    customData.START_FLAG = START_FLAG;
    customData.END_FLAG = END_FLAG;
    customData.FILENAME_LENGTH = FILENAME_LENGTH;
    customData.REPETITIONS = REPETITIONS;
end

function [outputBatch, actualOutputCount] = process_file_source_persistent(~, ~, initData, config)
    % Use persistent variable to maintain state across calls
    persistent state;
    
    % First call - initialize from initData
    if isempty(state)
        state = initData;
    end
    
    PACKET_SIZE = config.outputPacketSizes(1);
    BATCH_SIZE = config.outputBatchSizes(1);
    totalBatchBytes = BATCH_SIZE * PACKET_SIZE;
    
    % Fill buffer with files until we have enough for a batch
    while state.currentFileIdx <= length(state.fileList) && ...
          length(state.streamBuffer) < totalBatchBytes
        
        fileName = state.fileList(state.currentFileIdx).name;
        filePath = fullfile(state.sourceDirectory, fileName);
        
        % Read current file
        fid = fopen(filePath, 'rb');
        if fid == -1
            fprintf('WARNING: Cannot open: %s (skipping)\n', fileName);
            state.currentFileIdx = state.currentFileIdx + 1;
            continue;
        end
        fileData = fread(fid, '*uint8');
        fclose(fid);
        
        fileSize = length(fileData);
        fprintf('Loading file %d/%d: %s (%.2f KB)\n', ...
                state.currentFileIdx, length(state.fileList), fileName, fileSize/1024);
        
        % Build file packet structure
        fileNameBytes = uint8(zeros(state.FILENAME_LENGTH, 1));
        nameBytes = uint8(fileName);
        nameLen = min(length(nameBytes), state.FILENAME_LENGTH);
        fileNameBytes(1:nameLen) = nameBytes(1:nameLen);
        
        fileSizeBytes = typecast(uint64(fileSize), 'uint8')';
        
        fileStream = [];
        fileStream = [fileStream; state.START_FLAG(:)];
        for rep = 1:state.REPETITIONS
            fileStream = [fileStream; fileNameBytes];
        end
        for rep = 1:state.REPETITIONS
            fileStream = [fileStream; fileSizeBytes];
        end
        fileStream = [fileStream; fileData];
        fileStream = [fileStream; state.END_FLAG(:)];
        
        state.streamBuffer = [state.streamBuffer; fileStream];
        
        fprintf('  Added to buffer: %d bytes (total buffer: %.2f KB)\n', ...
                length(fileStream), length(state.streamBuffer)/1024);
        
        % *** INCREMENT FILE INDEX ***
        state.currentFileIdx = state.currentFileIdx + 1;
        
        if length(state.streamBuffer) >= totalBatchBytes
            break;
        end
    end
    
    % Check if we're done
    if state.currentFileIdx > length(state.fileList) && isempty(state.streamBuffer)
        fprintf('\n========================================\n');
        fprintf('TRANSMISSION COMPLETE\n');
        fprintf('========================================\n');
        fprintf('Total files sent: %d\n', length(state.fileList));
        fprintf('========================================\n');
        error('MATLAB:MEX:ErrMsgTxt', 'All files transmitted');
    end
    
    % Send batch from buffer
    availableBytes = length(state.streamBuffer);
    maxPacketsFromBuffer = ceil(availableBytes / PACKET_SIZE);
    packetsInThisBatch = min(BATCH_SIZE, maxPacketsFromBuffer);
    
    if packetsInThisBatch == 0
        outputBatch = [];
        actualOutputCount = 0;
        return;
    end
    
    % Create batch buffer
    outputBatch = zeros(BATCH_SIZE * PACKET_SIZE, 1, 'int8');
    bytesToSend = min(packetsInThisBatch * PACKET_SIZE, availableBytes);
    
    % Extract data from buffer
    dataToSend = state.streamBuffer(1:bytesToSend);
    state.streamBuffer(1:bytesToSend) = [];  % Remove sent data
    
    % Fill batch
    dataLen = length(dataToSend);
    outputBatch(1:dataLen) = int8(int32(dataToSend) - 128);
    
    actualOutputCount = packetsInThisBatch;
    
    fprintf('Sending batch (%d packets, buffer remaining: %.2f KB)\n', ...
            packetsInThisBatch, length(state.streamBuffer)/1024);
end