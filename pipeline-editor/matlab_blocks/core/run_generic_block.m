function run_generic_block(pipeIn, pipeOut, block_config, process_fn, init_fn)
% RUN_GENERIC_BLOCK - Generic block executor framework
% Handles all boilerplate: sockets, batch I/O, metrics, error handling

    % Parse configuration
    config = parse_block_config(block_config);
    
    % Determine block type
    isSource = isempty(pipeIn) || config.inputs == 0;
    isSink = isempty(pipeOut) || config.outputs == 0;
    
    % Calculate batch parameters
    if ~isSource
        INPUT_PACKET_SIZE = config.inputPacketSizes(1);
        INPUT_BATCH_SIZE = config.inputBatchSizes(1);
        INPUT_LENGTH_BYTES = calculate_length_bytes(INPUT_BATCH_SIZE);
        INPUT_BUFFER_SIZE = INPUT_LENGTH_BYTES + (INPUT_PACKET_SIZE * INPUT_BATCH_SIZE);
    end
    
    if ~isSink
        OUTPUT_PACKET_SIZE = config.outputPacketSizes(1);
        OUTPUT_BATCH_SIZE = config.outputBatchSizes(1);
        OUTPUT_LENGTH_BYTES = calculate_length_bytes(OUTPUT_BATCH_SIZE);
        OUTPUT_BUFFER_SIZE = OUTPUT_LENGTH_BYTES + (OUTPUT_PACKET_SIZE * OUTPUT_BATCH_SIZE);
    end
    
    % Print block info
    fprintf('\n========================================\n');
    fprintf('%s - Generic Block Framework\n', config.name);
    fprintf('========================================\n');
    if ~isSource
        fprintf('INPUT:  %d bytes × %d packets (header: %dB, total: %.2fKB)\n', ...
                INPUT_PACKET_SIZE, INPUT_BATCH_SIZE, INPUT_LENGTH_BYTES, INPUT_BUFFER_SIZE/1024);
    end
    if ~isSink
        fprintf('OUTPUT: %d bytes × %d packets (header: %dB, total: %.2fKB)\n', ...
                OUTPUT_PACKET_SIZE, OUTPUT_BATCH_SIZE, OUTPUT_LENGTH_BYTES, OUTPUT_BUFFER_SIZE/1024);
    end
    fprintf('========================================\n');
    
    % Get instance-specific MATLAB port from environment
    matlabPortStr = getenv('MATLAB_PORT');
    if ~isempty(matlabPortStr)
        matlabPort = str2double(matlabPortStr);
    else
        matlabPort = config.socketPort;
    end
    
    % Connect to socket
    socketObj = matlab_socket_client(config.socketHost, matlabPort, 10);
    if isempty(socketObj)
        error('Failed to connect to socket server. Make sure Electron is running!');
    end
    
    send_socket_message(socketObj, 'BLOCK_INIT', config.blockId, config.name, '');
    
    try
        % Optional custom initialization
        customData = [];
        if nargin >= 5 && ~isempty(init_fn)
            fprintf('Running custom initialization...\n');
            customData = init_fn(config);
        end
        
        send_socket_message(socketObj, 'BLOCK_READY', config.blockId, config.name, '');
        
        % Metrics tracking
        batchCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        % Main processing loop
        while true
            try
                % Read input batch (if not a source)
                inputBatch = [];
                actualInputCount = 0;
                
                if ~isSource
                    inputBuffer = pipeline_mex('read', pipeIn, INPUT_BUFFER_SIZE);
                    
                    % Parse length header
                    for i = 1:INPUT_LENGTH_BYTES
                        actualInputCount = actualInputCount + double(uint8(int32(inputBuffer(i)) + 128)) * (256^(i-1));
                    end
                    
                    % Extract input batch data
                    inputBatch = inputBuffer(INPUT_LENGTH_BYTES + 1 : end);
                end
                
                % Call user's processing function
                [outputBatch, actualOutputCount] = process_fn(inputBatch, actualInputCount, customData, config);
                
                % Write output batch (if not a sink)
                if ~isSink && ~isempty(outputBatch)
                    write_batch(pipeOut, outputBatch, actualOutputCount, OUTPUT_LENGTH_BYTES);
                end
                
                batchCount = batchCount + 1;
                
                % Calculate throughput
                if ~isSink
                    totalBytes = totalBytes + OUTPUT_BUFFER_SIZE;
                elseif ~isSource
                    totalBytes = totalBytes + INPUT_BUFFER_SIZE;
                end
                
                currentTime = toc(startTime);
                elapsed = currentTime - lastTime;
                if elapsed > 0
                    instantGbps = ((totalBytes - lastBytes) * 8 / 1e9) / elapsed;
                else
                    instantGbps = 0;
                end
                lastTime = currentTime;
                lastBytes = totalBytes;
                
                % Send metrics
                metrics = struct();
                metrics.frames = batchCount;
                metrics.gbps = instantGbps;
                metrics.totalGB = totalBytes / 1e9;
                send_socket_message(socketObj, 'BLOCK_METRICS', config.blockId, config.name, metrics);
                
                % Progress update (reduced frequency)
                if mod(batchCount, 100) == 0
                    fprintf('Batches: %d, Throughput: %.2f Gbps\n', batchCount, instantGbps);
                end
                
            catch ME
                if strcmp(ME.identifier, 'MATLAB:MEX:ErrMsgTxt') || contains(ME.message, 'All files transmitted')
                    % Pipeline closed or source finished - graceful shutdown
                    fprintf('\n========================================\n');
                    fprintf('BLOCK FINISHED\n');
                    fprintf('========================================\n');
                    fprintf('Total batches: %d\n', batchCount);
                    fprintf('Total data: %.2f GB\n', totalBytes / 1e9);
                    fprintf('========================================\n');
                    
                    % For sources that finish, send EOF signal to downstream
                    if isSource && ~isSink
                        fprintf('Sending EOF signal (0-length batch)...\n');
                        emptyBatch = zeros(OUTPUT_BATCH_SIZE * OUTPUT_PACKET_SIZE, 1, 'int8');
                        write_batch(pipeOut, emptyBatch, 0, OUTPUT_LENGTH_BYTES);
                    end
                    
                    send_socket_message(socketObj, 'BLOCK_STOPPED', config.blockId, config.name, '');
                    clear socketObj;
                    return;
                else
                    rethrow(ME);
                end
            end
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
    
    % Set defaults
    if ~isfield(config, 'socketHost')
        config.socketHost = 'localhost';
    end
    if ~isfield(config, 'socketPort')
        config.socketPort = 9001;
    end
    
    % Validate required fields
    if ~isfield(config, 'name')
        error('block_config must have a name field');
    end
    if ~isfield(config, 'inputs')
        error('block_config must have an inputs field');
    end
    if ~isfield(config, 'outputs')
        error('block_config must have an outputs field');
    end
    
    % Convert to arrays
    if config.inputs > 0
        if ~isfield(config, 'inputPacketSizes') || ~isfield(config, 'inputBatchSizes')
            error('Blocks with inputs must specify inputPacketSizes and inputBatchSizes');
        end
        if ~iscell(config.inputPacketSizes) && ~ismatrix(config.inputPacketSizes)
            config.inputPacketSizes = [config.inputPacketSizes];
        end
        if ~iscell(config.inputBatchSizes) && ~ismatrix(config.inputBatchSizes)
            config.inputBatchSizes = [config.inputBatchSizes];
        end
    end
    
    if config.outputs > 0
        if ~isfield(config, 'outputPacketSizes') || ~isfield(config, 'outputBatchSizes')
            error('Blocks with outputs must specify outputPacketSizes and outputBatchSizes');
        end
        if ~iscell(config.outputPacketSizes) && ~ismatrix(config.outputPacketSizes)
            config.outputPacketSizes = [config.outputPacketSizes];
        end
        if ~iscell(config.outputBatchSizes) && ~ismatrix(config.outputBatchSizes)
            config.outputBatchSizes = [config.outputBatchSizes];
        end
    end
end