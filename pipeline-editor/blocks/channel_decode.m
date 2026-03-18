function channel_decode(pipeIn, pipeOut)
% CHANNEL_DECODE - Channel decoding block with protocol
%
% @BlockConfig
% name: ChannelDecode
% inputs: 1
% outputs: 1
% inputSize: 128*1024*1024
% outputSize: 64*1024*1024
% LTR: true
% startWithAll: true
% codingType: convolutional
% constraintLength: 7
% codeGenerator: [171 133]
% tracebackDepth: 35
% decisionType: hard
% operationMode: cont
% chunkSize: 2*1024*1024
% description: Channel decoder (rate 1/2)
% @EndBlockConfig

    config = parse_block_config();
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');
    
    fprintf('========================================\n');
    fprintf('DECODER - Starting\n');
    fprintf('========================================\n');
    fprintf('Pipe In:     %s (%.2f MB)\n', pipeIn, config.inputSize/1024/1024);
    fprintf('Pipe Out:    %s (%.2f MB)\n', pipeOut, config.outputSize/1024/1024);
    fprintf('Type:        %s\n', config.codingType);
    fprintf('Constraint:  %d\n', config.constraintLength);
    fprintf('Generators:  [%d %d] (octal)\n', config.codeGenerator(1), config.codeGenerator(2));
    fprintf('Traceback:   %d\n', config.tracebackDepth);
    fprintf('Decision:    %s\n', config.decisionType);
    fprintf('Mode:        %s\n', config.operationMode);
    fprintf('Chunk Size:  %.2f MB\n', config.chunkSize/1024/1024);
    fprintf('========================================\n\n');
    
    try
        fprintf('Initializing decoder...\n');
        trellis = poly2trellis(config.constraintLength, config.codeGenerator);
        fprintf('Decoder initialized\n');
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        while true
            coded = pipeline_mex('read', pipeIn, config.inputSize);
            binaryData = de2bi(double(typecast(coded, 'uint8')), 8, 'left-msb');
            binaryData = reshape(binaryData.', [], 1);
            
            numChunks = ceil(length(binaryData) / config.chunkSize);
            decoded = [];
            
            for c = 1:numChunks
                startIdx = (c-1) * config.chunkSize + 1;
                endIdx   = min(c * config.chunkSize, length(binaryData));
                chunk    = binaryData(startIdx:endIdx);
                decodedChunk = vitdec(double(chunk(:)), trellis, ...
                                    config.tracebackDepth, config.operationMode, ...
                                    config.decisionType);
                decoded = [decoded; decodedChunk];
            end
            
            padLen = mod(8 - mod(length(decoded), 8), 8);
            if padLen > 0
                decoded = [decoded; zeros(padLen, 1)];
            end
            
            decodedBytes = bi2de(reshape(decoded, 8, []).', 'left-msb');
            outputData = int8(typecast(uint8(decodedBytes), 'int8'));
            
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
