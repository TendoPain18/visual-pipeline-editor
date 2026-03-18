function file_source(pipeOut)
% FILE_SOURCE - Progressive file loading with continuous buffering
% Reads files on-demand and fills buffers as data becomes available

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

    config = parse_block_config(block_config);
    
    % BATCH PROCESSING PARAMETERS
    PACKET_SIZE = config.outputPacketSizes(1);
    BATCH_SIZE = config.outputBatchSizes(1);
    LENGTH_BYTES = calculate_length_bytes(BATCH_SIZE);
    BUFFER_SIZE = LENGTH_BYTES + (PACKET_SIZE * BATCH_SIZE);
    
    % FILE PROTOCOL CONSTANTS
    START_FLAG = uint8([0xAA, 0x55, 0xAA, 0x55]);  % 4 bytes
    END_FLAG = uint8([0x55, 0xAA, 0x55, 0xAA]);    % 4 bytes
    FILENAME_LENGTH = 256;  % Max filename length
    REPETITIONS = 10;       % Repeat name and size 10 times

    fprintf('\n========================================\n');
    fprintf('FILE SOURCE - Progressive Loading\n');
    fprintf('========================================\n');
    fprintf('Packet Size:  %d bytes\n', PACKET_SIZE);
    fprintf('Batch Size:   %d packets\n', BATCH_SIZE);
    fprintf('Mode:         Load files on-demand → Fill batches progressively\n');
    fprintf('Protocol:     START(4B) + Name×10(%dB) + Size×10(%dB) + Data + END(4B)\n', ...
            FILENAME_LENGTH * REPETITIONS, 8 * REPETITIONS);

    % Get instance-specific MATLAB port from environment
    matlabPortStr = getenv('MATLAB_PORT');
    if ~isempty(matlabPortStr)
        matlabPort = str2double(matlabPortStr);
    else
        matlabPort = config.socketPort;
    end

    socketObj = matlab_socket_client(config.socketHost, matlabPort, 10);

    if isempty(socketObj)
        error('Failed to connect to socket server. Make sure Electron is running!');
    end

    send_socket_message(socketObj, 'BLOCK_INIT', config.blockId, config.name, '');

    try
        if ~exist(config.sourceDirectory, 'dir')
            mkdir(config.sourceDirectory);
            fprintf('Created source directory. Add files and restart.\n');
            send_socket_message(socketObj, 'BLOCK_ERROR', config.blockId, config.name, 'No source directory');
            clear socketObj;
            return;
        end

        send_socket_message(socketObj, 'BLOCK_READY', config.blockId, config.name, '');

        batchCount = 0;
        totalBytes = 0;
        startTime  = tic;
        lastTime   = 0;
        lastBytes  = 0;

        fileList = dir(fullfile(config.sourceDirectory, '*.*'));
        fileList = fileList(~[fileList.isdir]);

        if isempty(fileList)
            fprintf('No files found in source directory.\n');
            send_socket_message(socketObj, 'BLOCK_ERROR', config.blockId, config.name, 'No files to send');
            clear socketObj;
            return;
        end

        fprintf('Found %d file(s) to send\n', length(fileList));
        fprintf('========================================\n\n');

        % ===== PROGRESSIVE FILE LOADING AND BUFFERING =====
        streamBuffer = [];  % Running buffer of data to send
        currentFileIdx = 1;
        totalBatchBytes = BATCH_SIZE * PACKET_SIZE;  % Size of one full batch
        
        while currentFileIdx <= length(fileList) || ~isempty(streamBuffer)
            
            % Fill buffer with files until we have enough for a batch OR all files loaded
            while currentFileIdx <= length(fileList) && length(streamBuffer) < totalBatchBytes
                fileName = fileList(currentFileIdx).name;
                filePath = fullfile(config.sourceDirectory, fileName);
                
                % Read current file
                fid = fopen(filePath, 'rb');
                if fid == -1
                    fprintf('WARNING: Cannot open: %s (skipping)\n', fileName);
                    currentFileIdx = currentFileIdx + 1;
                    continue;
                end
                fileData = fread(fid, '*uint8');
                fclose(fid);
                
                fileSize = length(fileData);
                fprintf('Loading file %d/%d: %s (%.2f KB)\n', currentFileIdx, length(fileList), fileName, fileSize/1024);
                
                % ===== BUILD FILE PACKET STRUCTURE =====
                % Prepare filename (pad or truncate to 256 bytes)
                fileNameBytes = uint8(zeros(FILENAME_LENGTH, 1));
                nameBytes = uint8(fileName);
                nameLen = min(length(nameBytes), FILENAME_LENGTH);
                fileNameBytes(1:nameLen) = nameBytes(1:nameLen);
                
                % Prepare file size as uint64 (8 bytes, little-endian)
                fileSizeBytes = typecast(uint64(fileSize), 'uint8')';
                
                % Build metadata for this file
                fileStream = [];
                
                % START FLAG
                fileStream = [fileStream; START_FLAG(:)];
                
                % FILENAME × 10
                for rep = 1:REPETITIONS
                    fileStream = [fileStream; fileNameBytes];
                end
                
                % FILESIZE × 10
                for rep = 1:REPETITIONS
                    fileStream = [fileStream; fileSizeBytes];
                end
                
                % FILE DATA
                fileStream = [fileStream; fileData];
                
                % END FLAG
                fileStream = [fileStream; END_FLAG(:)];
                
                % Append to stream buffer
                streamBuffer = [streamBuffer; fileStream];
                
                fprintf('  Added to buffer: %d bytes (total buffer: %.2f KB)\n', ...
                        length(fileStream), length(streamBuffer)/1024);
                
                currentFileIdx = currentFileIdx + 1;
                
                % Send batch immediately if buffer is full enough
                if length(streamBuffer) >= totalBatchBytes
                    break;  % Exit file loading loop to send batch
                end
            end
            
            % Send batches from buffer
            while ~isempty(streamBuffer)
                
                % Determine how many packets to send in this batch
                availableBytes = length(streamBuffer);
                maxPacketsFromBuffer = ceil(availableBytes / PACKET_SIZE);  % Changed floor to ceil
                packetsInThisBatch = min(BATCH_SIZE, maxPacketsFromBuffer);
                
                if packetsInThisBatch == 0
                    break;  % No more data to send
                end
                
                % Create batch buffer
                batch = zeros(BATCH_SIZE * PACKET_SIZE, 1, 'int8');
                bytesToSend = min(packetsInThisBatch * PACKET_SIZE, availableBytes);
                
                % Extract data from buffer
                dataToSend = streamBuffer(1:min(availableBytes, packetsInThisBatch * PACKET_SIZE));
                streamBuffer(1:length(dataToSend)) = [];  % Remove sent data
                
                % Fill batch
                dataLen = length(dataToSend);
                batch(1:dataLen) = int8(int32(dataToSend) - 128);
                
                % Send batch
                write_batch(pipeOut, batch, packetsInThisBatch, LENGTH_BYTES);
                batchCount = batchCount + 1;
                totalBytes = totalBytes + BUFFER_SIZE;
                
                if mod(batchCount, 10) == 1 || isempty(streamBuffer)
                    fprintf('Sent batch %d (%d packets, buffer remaining: %.2f KB)\n', ...
                            batchCount, packetsInThisBatch, length(streamBuffer)/1024);
                end

                currentTime = toc(startTime);
                elapsed = currentTime - lastTime;
                if elapsed > 0
                    instantGbps = ((totalBytes - lastBytes) * 8 / 1e9) / elapsed;
                else
                    instantGbps = 0;
                end
                lastTime  = currentTime;
                lastBytes = totalBytes;

                metrics        = struct();
                metrics.frames = batchCount;
                metrics.gbps   = instantGbps;
                metrics.totalGB = totalBytes / 1e9;
                send_socket_message(socketObj, 'BLOCK_METRICS', config.blockId, config.name, metrics);
            end
            
            % Exit if all files processed and buffer empty
            if currentFileIdx > length(fileList) && isempty(streamBuffer)
                break;
            end
        end

        fprintf('\n========================================\n');
        fprintf('TRANSMISSION COMPLETE\n');
        fprintf('========================================\n');
        fprintf('Total files sent:  %d\n', length(fileList));
        fprintf('Total batches:     %d\n', batchCount);
        fprintf('========================================\n');

        finalMetrics        = struct();
        finalMetrics.frames = batchCount;
        finalMetrics.gbps   = 0;
        finalMetrics.totalGB = totalBytes / 1e9;
        send_socket_message(socketObj, 'BLOCK_METRICS', config.blockId, config.name, finalMetrics);

        fprintf('\nWaiting 5 seconds before exit...\n');
        pause(5);
        fprintf('Exiting.\n');

        send_socket_message(socketObj, 'BLOCK_STOPPED', config.blockId, config.name, '');
        clear socketObj;

    catch ME
        send_socket_message(socketObj, 'BLOCK_ERROR', config.blockId, config.name, ME.message);
        clear socketObj;
        rethrow(ME);
    end
end

function lengthBytes = calculate_length_bytes(maxCount)
    if maxCount <= 255
        lengthBytes = 1;
    elseif maxCount <= 65535
        lengthBytes = 2;
    elseif maxCount <= 16777215
        lengthBytes = 3;
    else
        lengthBytes = 4;
    end
end

function write_batch(pipeOut, batchData, actualCount, lengthBytes)
    bufferSize = lengthBytes + length(batchData);
    buffer = zeros(bufferSize, 1, 'int8');
    
    % Write length header (little-endian)
    count32 = uint32(actualCount);
    for i = 1:lengthBytes
        byteVal = bitand(bitshift(count32, -(i-1)*8), uint32(0xFF));
        buffer(i) = int8(int32(byteVal) - 128);
    end
    
    % Write batch data
    buffer(lengthBytes + 1 : end) = batchData;
    
    pipeline_mex('write', pipeOut, buffer);
end

function config = parse_block_config(block_config)
    config = block_config;
    blockIdStr = getenv('BLOCK_ID');
    if isempty(blockIdStr)
        config.blockId = 0;
    else
        config.blockId = str2double(blockIdStr);
    end
    
    if ~isfield(config, 'outputPacketSizes')
        error('outputPacketSizes required for batch processing');
    end
    if ~isfield(config, 'outputBatchSizes')
        error('outputBatchSizes required for batch processing');
    end
    
    % Convert to arrays if single values
    if ~iscell(config.outputPacketSizes) && ~ismatrix(config.outputPacketSizes)
        config.outputPacketSizes = [config.outputPacketSizes];
    end
    if ~iscell(config.outputBatchSizes) && ~ismatrix(config.outputBatchSizes)
        config.outputBatchSizes = [config.outputBatchSizes];
    end
end