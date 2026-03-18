function sink(pipeIn)
% SINK - Data sink block with protocol
%
% @BlockConfig
% name: Sink
% inputs: 1
% outputs: 0
% inputSize: 8
% outputSize: 0
% LTR: true
% startWithAll: true
% writeToFile: false
% fileName: output.bin
% description: Data sink block
% @EndBlockConfig

    config = parse_block_config();
    
    % Send BLOCK_INIT
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');
    
    fprintf('========================================\n');
    fprintf('SINK - Starting\n');
    fprintf('========================================\n');
    fprintf('Pipe In:     %s (%.2f MB)\n', pipeIn, config.inputSize/1024/1024);
    fprintf('Write File:  %s\n', config.writeToFile);
    fprintf('========================================\n\n');
    
    try
        if config.writeToFile
            fid = fopen(config.fileName, 'wb');
            if fid == -1, error('Cannot open file'); end
            fprintf('Opened file: %s\n', config.fileName);
        end
        
        % Send BLOCK_READY
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        frameCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastBytes = 0;
        
        while true
            % Read from input pipe (blocks until data available)
            data = pipeline_mex('read', pipeIn, config.inputSize);
            
            if config.writeToFile
                fwrite(fid, data);
            end
            
            frameCount = frameCount + 1;
            totalBytes = totalBytes + length(data);
            
            % Calculate instantaneous throughput
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
            metrics.totalGB = totalBytes / 1e9;
            
            send_protocol_message('BLOCK_METRICS', config.blockId, config.name, metrics);
        end
        
    catch ME
        if config.writeToFile && exist('fid', 'var') && fid ~= -1
            fclose(fid);
        end
        fprintf('\n\nSINK: Stopped - %.2f GB received\n', totalBytes/1e9);
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