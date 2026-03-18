function crc_encode(pipeIn, pipeOut)
% CRC_ENCODE - ITU-T CRC-32 encoder (continuous hardware-style operation)
%
% @BlockConfig
% name: CrcEncode
% inputs: 1
% outputs: 1
% inputSize: 1500
% outputSize: 1504
% LTR: true
% startWithAll: true
% polynomial: 0x04C11DB7
% description: CRC-32 encoder (ITU-T V.42) - continuous operation
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');
    
    fprintf('========================================\n');
    fprintf('CRC ENCODE - Continuous Mode\n');
    fprintf('========================================\n');
    fprintf('Pipe In:     %s (%d bytes)\n', pipeIn, config.inputSize);
    fprintf('Pipe Out:    %s (%d bytes)\n', pipeOut, config.outputSize);
    fprintf('Polynomial:  0x%08X\n', config.polynomial);
    fprintf('========================================\n\n');
    
    try
        % Build CRC-32 lookup table
        fprintf('Building CRC-32 lookup table...\n');
        crcTable = build_crc32_table();
        fprintf('CRC table ready\n');
        
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        % CONTINUOUS OPERATION - Never exits
        while true
            % Read input (1500 bytes as int8)
            inputData = pipeline_mex('read', pipeIn, config.inputSize);
            
            % Calculate CRC on the int8 data by treating as uint8
            dataAsUint8 = typecast(inputData, 'uint8');
            crc32 = calculate_crc32(dataAsUint8, crcTable);
            
            % Create output (1504 bytes = 1500 data + 4 CRC)
            outputData = zeros(config.outputSize, 1, 'int8');
            outputData(1:config.inputSize) = inputData;
            
            % Append CRC as 4 bytes (little-endian)
            crcBytes = typecast(uint32(crc32), 'uint8');
            outputData(1501:1504) = typecast(crcBytes, 'int8');
            
            pipeline_mex('write', pipeOut, outputData);
            
            frameCount = frameCount + 1;
            totalBytes = totalBytes + length(outputData);
            
            currentTime = toc(startTime);
            elapsed = currentTime - lastTime;
            if elapsed > 0
                instantGbps = ((totalBytes - lastBytes) * 8 / 1e9) / elapsed;
            else
                instantGbps = 0;
            end
            lastTime = currentTime;
            lastBytes = totalBytes;
            
            metrics = struct();
            metrics.frames = frameCount;
            metrics.gbps = instantGbps;
            send_protocol_message('BLOCK_METRICS', config.blockId, config.name, metrics);
        end
        
    catch ME
        send_protocol_message('BLOCK_ERROR', config.blockId, config.name, ME.message);
        rethrow(ME);
    end
end

function crcTable = build_crc32_table()
    poly = uint32(0xEDB88320);
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

function config = parse_block_config()
    filePath = mfilename('fullpath');
    fid = fopen([filePath '.m'], 'r');
    if fid == -1, error('Cannot open configuration file'); end
    content = fread(fid, '*char')';
    fclose(fid);
    startMarker = '@BlockConfig';
    endMarker = '@EndBlockConfig';
    startIdx = strfind(content, startMarker);
    endIdx = strfind(content, endMarker);
    if isempty(startIdx) || isempty(endIdx)
        error('No @BlockConfig section found');
    end
    configStart = startIdx(1) + length(startMarker);
    configEnd = endIdx(1) - 1;
    configText = content(configStart:configEnd);
    config = struct();
    lines = strsplit(configText, newline);
    for i = 1:length(lines)
        line = strtrim(lines{i});
        if isempty(line), continue; end
        if line(1) == '%', line = strtrim(line(2:end)); end
        if isempty(line), continue; end
        colonIdx = strfind(line, ':');
        if isempty(colonIdx), continue; end
        key = strtrim(line(1:colonIdx(1)-1));
        value = strtrim(line(colonIdx(1)+1:end));
        commentIdx = strfind(value, '%');
        if ~isempty(commentIdx)
            value = strtrim(value(1:commentIdx(1)-1));
        end
        if isempty(key), continue; end
        try
            numValue = eval(value);
            config.(key) = numValue;
        catch
            config.(key) = value;
        end
    end
    blockIdStr = getenv('BLOCK_ID');
    if isempty(blockIdStr)
        config.blockId = 0;
    else
        config.blockId = str2double(blockIdStr);
    end
    requiredFields = {'name', 'inputs', 'outputs', 'inputSize', 'outputSize'};
    for i = 1:length(requiredFields)
        if ~isfield(config, requiredFields{i})
            error('Missing required field: %s', requiredFields{i});
        end
    end
end
