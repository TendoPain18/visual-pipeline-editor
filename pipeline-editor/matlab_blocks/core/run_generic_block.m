function run_generic_block(pipeIn, pipeOut, block_config, process_fn, init_fn)
% RUN_MANUAL_BLOCK  Full manual I/O control — mirrors C++ run_generic_block.
%
% Unlike run_generic_block (which auto-reads one input, calls process_fn,
% auto-writes one output), this hands your process_fn a cell array of PipeIO
% objects and lets it call read/write in any order it needs.
%
% Usage in your block file:
%
%   function my_block(pipeIn1, pipeIn2, pipeOut1, pipeOut2)
%       block_config = struct(
%           'name',             'MyBlock',
%           'inputs',           2,
%           'outputs',          2,
%           'inputPacketSizes', [1515, 3],
%           'inputBatchSizes',  [44740, 44740],
%           ...
%       );
%       run_generic_block(
%           {pipeIn1, pipeIn2},
%           {pipeOut1, pipeOut2},
%           block_config,
%           @process_fn,
%           @init_fn
%       );
%   end
%
%   function process_fn(pipeIn, pipeOut, customData, config)
%       % pipeIn{1}, pipeIn{2} ... are PipeIO objects
%       % Call read/write in whatever order you need:
%       [sig, n]  = pipeIn{2}.read();     % read signal first
%       pipeOut{2}.write(sig, n);          % forward it
%       [dat, ~]  = pipeIn{1}.read();     % THEN read data
%       pipeOut{1}.write(result, n);       % write result
%   end
%
% Inputs:
%   pipeIn       cell array of pipe name strings  ({} for sources)
%   pipeOut      cell array of pipe name strings  ({} for sinks)
%   block_config struct with fields: name, inputs, outputs,
%                inputPacketSizes, inputBatchSizes,
%                outputPacketSizes, outputBatchSizes,
%                LTR, startWithAll
%   process_fn   function(pipeInObjs, pipeOutObjs, customData, config)
%                returns nothing — calls read/write directly
%   init_fn      (optional) function(config) -> customData

    config   = parseConfig(block_config);
    isSource = isempty(pipeIn)  || config.inputs  == 0;
    isSink   = isempty(pipeOut) || config.outputs == 0;

    %% Banner
    fprintf('\n========================================\n');
    fprintf('%s - MANUAL I/O MODE\n', config.name);
    fprintf('========================================\n');
    if ~isSource
        for i = 1:config.inputs
            bs = config.inputLengthBytes(i) + config.inputPacketSizes(i)*config.inputBatchSizes(i);
            fprintf('  Input  %d: %d bytes x %d pkts = %.2f KB\n', ...
                i, config.inputPacketSizes(i), config.inputBatchSizes(i), bs/1024);
        end
    end
    if ~isSink
        for i = 1:config.outputs
            bs = config.outputLengthBytes(i) + config.outputPacketSizes(i)*config.outputBatchSizes(i);
            fprintf('  Output %d: %d bytes x %d pkts = %.2f KB\n', ...
                i, config.outputPacketSizes(i), config.outputBatchSizes(i), bs/1024);
        end
    end
    fprintf('========================================\n\n');

    %% Socket
    matlabPortEnv = getenv('MATLAB_PORT');
    if ~isempty(matlabPortEnv)
        socketPort = str2double(matlabPortEnv);
    else
        socketPort = config.socketPort;
    end
    socketObj = matlab_socket_client(config.socketHost, socketPort, 10);
    if isempty(socketObj)
        error('[%s] Failed to connect to socket server.', config.name);
    end
    send_socket_message(socketObj, 'BLOCK_INIT', config.blockId, config.name, '');

    %% Build PipeIO objects
    inObjs  = {};
    outObjs = {};
    if ~isSource
        inCell = toCellStr(pipeIn);
        for i = 1:config.inputs
            inObjs{i} = PipeIO(inCell{i}, config.inputPacketSizes(i), config.inputBatchSizes(i));
        end
    end
    if ~isSink
        outCell = toCellStr(pipeOut);
        for i = 1:config.outputs
            outObjs{i} = PipeIO(outCell{i}, config.outputPacketSizes(i), config.outputBatchSizes(i));
        end
    end

    %% Init
    customData = [];
    if nargin >= 5 && ~isempty(init_fn)
        fprintf('Running custom initialization...\n');
        customData = init_fn(config);
    end
    send_socket_message(socketObj, 'BLOCK_READY', config.blockId, config.name, '');

    %% Metrics
    iterCount  = 0;
    totalBytes = 0;
    tStart     = tic;
    lastTime   = 0;
    lastBytes  = 0;

    %% Main loop
    try
        while true
            process_fn(inObjs, outObjs, customData, config);
            iterCount = iterCount + 1;

            if ~isSink && config.outputs > 0
                totalBytes = totalBytes + config.outputLengthBytes(1) + ...
                    config.outputPacketSizes(1) * config.outputBatchSizes(1);
            elseif ~isSource && config.inputs > 0
                totalBytes = totalBytes + config.inputLengthBytes(1) + ...
                    config.inputPacketSizes(1) * config.inputBatchSizes(1);
            end

            now     = toc(tStart);
            elapsed = now - lastTime;
            if elapsed > 0
                gbps = ((totalBytes - lastBytes) * 8 / 1e9) / elapsed;
            else
                gbps = 0;
            end
            lastTime  = now;
            lastBytes = totalBytes;

            m = struct('frames', iterCount, 'gbps', gbps, 'totalGB', totalBytes/1e9);
            send_socket_message(socketObj, 'BLOCK_METRICS', config.blockId, config.name, m);

            if mod(iterCount, 100) == 0
                fprintf('Iterations: %d, Throughput: %.2f Gbps\n', iterCount, gbps);
            end
        end
    catch ME
        if strcmp(ME.identifier, 'MATLAB:MEX:ErrMsgTxt') || ...
           contains(ME.message, 'All files transmitted') || ...
           contains(ME.message, 'Block finished')
            fprintf('\n[%s] Finished. Iterations: %d, Data: %.2f GB\n', ...
                config.name, iterCount, totalBytes/1e9);
            send_socket_message(socketObj, 'BLOCK_STOPPED', config.blockId, config.name, '');
            clear socketObj;
            return;
        else
            send_socket_message(socketObj, 'BLOCK_ERROR', config.blockId, config.name, ME.message);
            clear socketObj;
            rethrow(ME);
        end
    end
end

%% ─── Helpers ────────────────────────────────────────────────────────────────

function config = parseConfig(bc)
    config = bc;
    bid = getenv('BLOCK_ID');
    if isempty(bid); config.blockId = 0; else; config.blockId = str2double(bid); end
    if ~isfield(config,'socketHost'); config.socketHost = 'localhost'; end
    if ~isfield(config,'socketPort'); config.socketPort = 9001;        end

    % Normalise to row vectors
    flds = {'inputPacketSizes','inputBatchSizes','outputPacketSizes','outputBatchSizes'};
    for k = 1:numel(flds)
        f = flds{k};
        if isfield(config, f); config.(f) = config.(f)(:)'; end
    end

    % Pre-compute length bytes
    if isfield(config,'inputBatchSizes')
        config.inputLengthBytes = arrayfun(@lb, config.inputBatchSizes);
    end
    if isfield(config,'outputBatchSizes')
        config.outputLengthBytes = arrayfun(@lb, config.outputBatchSizes);
    end
end

function n = lb(maxCount)
    if     maxCount <= 255;       n = 1;
    elseif maxCount <= 65535;     n = 2;
    elseif maxCount <= 16777215;  n = 3;
    else;                         n = 4;
    end
end

function c = toCellStr(x)
    if iscell(x); c = x; else; c = {x}; end
end
