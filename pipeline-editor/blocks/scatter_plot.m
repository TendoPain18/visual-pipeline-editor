function scatter_plot(pipeIn)
% SCATTER_PLOT - Real-time on-the-fly scatter plot for QAM constellation
%
% Receives combined I/Q values in batches from QAM demapper
% Format per batch:
%   First 4 bytes: uint32 number of symbols (little-endian)
%   Remaining bytes: Interleaved I,Q pairs as int16 (I[0],Q[0],I[1],Q[1],...)
%
% Displays them as a real-time constellation diagram with rolling window
% Only displays DATA subcarrier symbols (no pilots, no nulls, no SIGNAL)
%
% @BlockConfig
% name: ScatterPlot
% inputs: 1
% outputs: 0
% inputSize: 50000
% outputSize: 0
% LTR: false
% startWithAll: true
% graphType: scatter
% maxPoints: 500
% xLabel: In-phase (I)
% yLabel: Quadrature (Q)
% title: QAM Constellation Diagram
% markerSize: 20
% description: Real-time QAM constellation scatter plot (on-the-fly with rolling window)
% @EndBlockConfig

    config = parse_block_config();
    
    % Send BLOCK_INIT
    send_protocol_message('BLOCK_INIT', config.blockId, config.name, '');
    
    fprintf('========================================\n');
    fprintf('%s - Starting\n', config.name);
    fprintf('========================================\n');
    fprintf('Pipe In:     %s (Combined I/Q values)\n', pipeIn);
    fprintf('Graph Type:  %s\n', config.graphType);
    fprintf('Max Points:  %d (rolling window)\n', config.maxPoints);
    fprintf('Input Format: Batch (length + interleaved I/Q int16 pairs)\n');
    fprintf('========================================\n\n');
    
    try
        % Send BLOCK_READY
        send_protocol_message('BLOCK_READY', config.blockId, config.name, '');
        
        symbolCount = 0;
        batchCount = 0;
        totalBytes = 0;
        startTime = tic;
        lastTime = 0;
        lastSymbols = 0;
        
        % Rolling window buffer for on-the-fly plotting
        maxPoints = config.maxPoints;
        pointBuffer = [];  % Will store [I, Q] pairs
        bufferIndex = 0;
        
        fprintf('Waiting for symbol batches from QAM demapper...\n');
        fprintf('Plotting mode: On-the-fly with rolling window (%d points)\n\n', maxPoints);
        
        while true
            % Read combined I/Q batch (4 bytes length + interleaved I/Q pairs)
            iqInput = pipeline_mex('read', pipeIn, config.inputSize);
            
            % Extract length from first 4 bytes (uint32, little-endian)
            lengthBytes = uint8(int16(iqInput(1:4)) + 128);
            numSymbols = typecast(uint8(lengthBytes), 'uint32');
            
            % Validate length
            if numSymbols == 0 || numSymbols > 100000
                fprintf('WARNING: Invalid symbol count %d, skipping batch\n', numSymbols);
                continue;
            end
            
            batchCount = batchCount + 1;
            symbolCount = symbolCount + double(numSymbols);
            totalBytes = totalBytes + 4 + numSymbols * 4;  % 4 bytes per I/Q pair
            
            % Extract interleaved I/Q values (int16 pairs, scaled by 32767)
            % Format: I[0](2 bytes), Q[0](2 bytes), I[1](2 bytes), Q[1](2 bytes), ...
            for i = 1:numSymbols
                baseIdx = 4 + (i-1)*4;  % 4 bytes per symbol (I+Q)
                
                % Extract I value (bytes 0-1 of this symbol)
                I_bytes = uint8(int16(iqInput(baseIdx+1:baseIdx+2)) + 128);
                I_int16 = typecast(uint8(I_bytes), 'int16');
                I_val = double(I_int16) / 32767.0;
                
                % Extract Q value (bytes 2-3 of this symbol)
                Q_bytes = uint8(int16(iqInput(baseIdx+3:baseIdx+4)) + 128);
                Q_int16 = typecast(uint8(Q_bytes), 'int16');
                Q_val = double(Q_int16) / 32767.0;
                
                % Add to rolling buffer
                if size(pointBuffer, 1) < maxPoints
                    % Buffer not full yet - just append
                    pointBuffer = [pointBuffer; I_val, Q_val];
                else
                    % Buffer full - replace oldest point (circular buffer)
                    bufferIndex = mod(bufferIndex, maxPoints) + 1;
                    pointBuffer(bufferIndex, :) = [I_val, Q_val];
                end
                
                % Send point to graph UI immediately (on-the-fly)
                graphData = struct('x', I_val, 'y', Q_val);
                send_protocol_message('BLOCK_GRAPH', config.blockId, config.name, graphData);
            end
            
            % Send metrics after each batch
            currentTime = toc(startTime);
            elapsed = currentTime - lastTime;
            if elapsed > 0
                instantRate = (symbolCount - lastSymbols) / elapsed;
            else
                instantRate = 0;
            end
            lastTime = currentTime;
            lastSymbols = symbolCount;
            
            metrics = struct();
            metrics.frames = batchCount;
            metrics.gbps = instantRate / 1e6;
            
            send_protocol_message('BLOCK_METRICS', config.blockId, config.name, metrics);
            
            fprintf('Batch %d: Received %d symbols (%.1fk symbols/sec, %d total, %d buffered)\n', ...
                batchCount, numSymbols, instantRate / 1000, symbolCount, size(pointBuffer, 1));
        end
        
    catch ME
        fprintf('\n\n%s: Stopped - %d batches, %d symbols plotted\n', ...
            config.name, batchCount, symbolCount);
        send_protocol_message('BLOCK_ERROR', config.blockId, config.name, ME.message);
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