function adder(pipeIn1, pipeIn2, pipeOut)
% ADDER - Adds two input numbers with protocol
%
% @BlockConfig
% name: Adder
% inputs: 2
% outputs: 1
% inputSize: [8, 8]
% outputSize: 8
% LTR: true
% startWithAll: true
% description: Adds two input numbers
% @EndBlockConfig

    config = parse_block_config();
    
    % Send BLOCK_INIT
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');
    
    fprintf('========================================\n');
    fprintf('%s - Starting\n', config.name);
    fprintf('========================================\n');
    fprintf('Pipe In 1:   %s\n', pipeIn1);
    fprintf('Pipe In 2:   %s\n', pipeIn2);
    fprintf('Pipe Out:    %s\n', pipeOut);
    fprintf('========================================\n\n');
    
    try
        % Send BLOCK_READY
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        while true
            % Read from both input pipes (blocks until data available)
            input1 = pipeline_mex('read', pipeIn1, 8);
            input2 = pipeline_mex('read', pipeIn2, 8);
            
            % Extract values
            val1 = double(input1(1));
            val2 = double(input2(1));
            
            % Add values
            result = val1 + val2;
            
            % Create output
            outputData = int8(zeros(8, 1));
            outputData(1) = int8(result);
            
            % Write to output pipe (blocks until pipe is ready)
            pipeline_mex('write', pipeOut, outputData);
            
            frameCount = frameCount + 1;
            totalBytes = totalBytes + length(outputData);
            
            % Calculate instantaneous throughput (per frame)
            currentTime = toc(startTime);
            elapsed = currentTime - lastTime;
            if elapsed > 0
                instantGbps = ((totalBytes - lastBytes) * 8 / 1e9) / elapsed;
            else
                instantGbps = 0;
            end
            lastTime = currentTime;
            lastBytes = totalBytes;
            
            % Send metrics every frame
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
