function crc_encode(pipeIn, pipeOut)
% CRC_ENCODE - ITU-T CRC-32 encoder with BATCH PROCESSING

block_config = struct( ...
    'name',              'CrcEncode', ...
    'inputs',            1, ...
    'outputs',           1, ...
    'inputPacketSizes',  1500, ...
    'inputBatchSizes',   44740, ...
    'outputPacketSizes', 1504, ...
    'outputBatchSizes',  44740, ...
    'LTR',               true, ...
    'startWithAll',      true, ...
    'socketHost',        'localhost', ...
    'socketPort',        9001, ...
    'polynomial',        79764919, ...
    'description',       'CRC-32 encoder (ITU-T V.42) - batch processing' ...
);

    config = parse_block_config(block_config);

    % BATCH PROCESSING PARAMETERS
    INPUT_PACKET_SIZE = config.inputPacketSizes(1);
    INPUT_BATCH_SIZE = config.inputBatchSizes(1);
    INPUT_LENGTH_BYTES = calculate_length_bytes(INPUT_BATCH_SIZE);
    INPUT_BUFFER_SIZE = INPUT_LENGTH_BYTES + (INPUT_PACKET_SIZE * INPUT_BATCH_SIZE);
    
    OUTPUT_PACKET_SIZE = config.outputPacketSizes(1);
    OUTPUT_BATCH_SIZE = config.outputBatchSizes(1);
    OUTPUT_LENGTH_BYTES = calculate_length_bytes(OUTPUT_BATCH_SIZE);
    OUTPUT_BUFFER_SIZE = OUTPUT_LENGTH_BYTES + (OUTPUT_PACKET_SIZE * OUTPUT_BATCH_SIZE);

    fprintf('\n========================================\n');
    fprintf('CRC ENCODER - Batch Processing Mode\n');
    fprintf('========================================\n');
    fprintf('INPUT:  %d bytes × %d packets (header: %dB, total: %.2fKB)\n', ...
            INPUT_PACKET_SIZE, INPUT_BATCH_SIZE, INPUT_LENGTH_BYTES, INPUT_BUFFER_SIZE/1024);
    fprintf('OUTPUT: %d bytes × %d packets (header: %dB, total: %.2fKB)\n', ...
            OUTPUT_PACKET_SIZE, OUTPUT_BATCH_SIZE, OUTPUT_LENGTH_BYTES, OUTPUT_BUFFER_SIZE/1024);
    fprintf('========================================\n');

    % Get instance-specific MATLAB port from environment
    matlabPortStr = getenv('MATLAB_PORT');
    if ~isempty(matlabPortStr)
        matlabPort = str2double(matlabPortStr);
    else
        matlabPort = config.socketPort;
    end

    % SOCKET CONNECTION (REQUIRED)
    socketObj = matlab_socket_client(config.socketHost, matlabPort, 10);

    if isempty(socketObj)
        error('Failed to connect to socket server. Make sure Electron is running!');
    end

    send_socket_message(socketObj, 'BLOCK_INIT', config.blockId, config.name, '');

    try
        % Build CRC-32 lookup table
        crcTable = build_crc32_table();

        send_socket_message(socketObj, 'BLOCK_READY', config.blockId, config.name, '');

        batchCount = 0;
        totalBytes = 0;
        startTime  = tic;
        lastTime   = 0;
        lastBytes  = 0;

        % CONTINUOUS OPERATION - Never exits
        while true
            % Read input batch
            inputBuffer = pipeline_mex('read', pipeIn, INPUT_BUFFER_SIZE);
            
            % Parse length header
            actualCount = 0;
            for i = 1:INPUT_LENGTH_BYTES
                actualCount = actualCount + double(uint8(int32(inputBuffer(i)) + 128)) * (256^(i-1));
            end
            
            % Extract input batch data
            inputBatch = inputBuffer(INPUT_LENGTH_BYTES + 1 : end);
            
            % Process batch: add CRC to each packet
            outputBatch = zeros(OUTPUT_BATCH_SIZE * OUTPUT_PACKET_SIZE, 1, 'int8');
            
            for pktIdx = 1:actualCount
                % Extract input packet
                inputOffset = (pktIdx - 1) * INPUT_PACKET_SIZE;
                inputPacket = inputBatch(inputOffset + 1 : inputOffset + INPUT_PACKET_SIZE);
                
                % Calculate CRC on the int8 data by treating as uint8
                dataAsUint8 = typecast(inputPacket, 'uint8');
                crc32 = calculate_crc32(dataAsUint8, crcTable);
                
                % Create output packet (data + CRC)
                outputOffset = (pktIdx - 1) * OUTPUT_PACKET_SIZE;
                outputBatch(outputOffset + 1 : outputOffset + INPUT_PACKET_SIZE) = inputPacket;
                
                % Append CRC as 4 bytes (little-endian)
                crcBytes = typecast(uint32(crc32), 'uint8');
                crcInt8 = typecast(crcBytes, 'int8');
                outputBatch(outputOffset + INPUT_PACKET_SIZE + 1 : outputOffset + OUTPUT_PACKET_SIZE) = crcInt8;
            end
            
            % Write output batch with length header
            write_batch(pipeOut, outputBatch, actualCount, OUTPUT_LENGTH_BYTES);

            batchCount = batchCount + 1;
            totalBytes = totalBytes + OUTPUT_BUFFER_SIZE;

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
            send_socket_message(socketObj, 'BLOCK_METRICS', config.blockId, config.name, metrics);
        end

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
    % FIXED: Convert to uint32 to avoid double/integer mixing
    count32 = uint32(actualCount);
    for i = 1:lengthBytes
        byteVal = bitand(bitshift(count32, -(i-1)*8), uint32(0xFF));
        buffer(i) = int8(int32(byteVal) - 128);
    end
    
    % Write batch data
    buffer(lengthBytes + 1 : end) = batchData;
    
    pipeline_mex('write', pipeOut, buffer);
end

function crcTable = build_crc32_table()
    poly     = uint32(0xEDB88320);
    crcTable = zeros(256, 1, 'uint32');
    for i = 0:255
        crc = uint32(i);
        for j = 0:7
            if bitand(crc, uint32(1))
                crc = bitxor(bitshift(crc, -1), poly);
            else
                crc = bitshift(crc, -1);
            end
        end
        crcTable(i + 1) = crc;
    end
end

function crc = calculate_crc32(data, crcTable)
    crc = uint32(0xFFFFFFFF);
    for i = 1:length(data)
        tableIdx = bitxor(bitand(crc, uint32(0xFF)), uint32(data(i))) + 1;
        crc = bitxor(bitshift(crc, -8), crcTable(tableIdx));
    end
    crc = bitxor(crc, uint32(0xFFFFFFFF));
end

function config = parse_block_config(block_config)
    config = block_config;
    blockIdStr = getenv('BLOCK_ID');
    if isempty(blockIdStr)
        config.blockId = 0;
    else
        config.blockId = str2double(blockIdStr);
    end
    
    if ~isfield(config, 'inputPacketSizes') || ~isfield(config, 'inputBatchSizes')
        error('inputPacketSizes and inputBatchSizes required');
    end
    if ~isfield(config, 'outputPacketSizes') || ~isfield(config, 'outputBatchSizes')
        error('outputPacketSizes and outputBatchSizes required');
    end
    
    % Convert to arrays
    if ~iscell(config.inputPacketSizes) && ~ismatrix(config.inputPacketSizes)
        config.inputPacketSizes = [config.inputPacketSizes];
    end
    if ~iscell(config.inputBatchSizes) && ~ismatrix(config.inputBatchSizes)
        config.inputBatchSizes = [config.inputBatchSizes];
    end
    if ~iscell(config.outputPacketSizes) && ~ismatrix(config.outputPacketSizes)
        config.outputPacketSizes = [config.outputPacketSizes];
    end
    if ~iscell(config.outputBatchSizes) && ~ismatrix(config.outputBatchSizes)
        config.outputBatchSizes = [config.outputBatchSizes];
    end
end