function file_source(pipeOut)
% FILE_SOURCE  Progressive file loader — manual I/O version.

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

    run_generic_block({}, {pipeOut}, block_config, @process_fn, @init_fn);
end

%% ─── Init ───────────────────────────────────────────────────────────────────
function state = init_fn(config)
    START_FLAG      = uint8([0xAA, 0x55, 0xAA, 0x55]);
    END_FLAG        = uint8([0x55, 0xAA, 0x55, 0xAA]);
    FILENAME_LENGTH = 256;
    REPETITIONS     = 10;

    srcDir = config.sourceDirectory;
    if ~exist(srcDir, 'dir')
        mkdir(srcDir);
        error('Created source directory. Add files and restart.');
    end

    listing = dir(fullfile(srcDir, '*.*'));
    listing = listing(~[listing.isdir]);
    if isempty(listing)
        error('No files found in source directory: %s', srcDir);
    end
    fprintf('Found %d file(s) to send\n', numel(listing));

    state = struct( ...
        'fileList',       listing, ...
        'currentFileIdx', 1, ...
        'streamBuffer',   uint8([]), ...
        'sourceDirectory',srcDir, ...
        'START_FLAG',     START_FLAG, ...
        'END_FLAG',       END_FLAG, ...
        'FILENAME_LENGTH',FILENAME_LENGTH, ...
        'REPETITIONS',    REPETITIONS ...
    );
end

%% ─── Process ────────────────────────────────────────────────────────────────
function process_fn(~, pipeOut, initState, config)
    % Use persistent so state survives across calls
    persistent state;
    if isempty(state)
        state = initState;
    end

    out = pipeOut{1};

    PACKET_SIZE    = out.getPacketSize();   % 1500
    BATCH_SIZE     = out.getBatchSize();    % 44740
    totalBatchBytes = BATCH_SIZE * PACKET_SIZE;

    % ── Fill stream buffer from files until we have a full batch ──────────────
    while state.currentFileIdx <= numel(state.fileList) && ...
          numel(state.streamBuffer) < totalBatchBytes

        fileName = state.fileList(state.currentFileIdx).name;
        filePath = fullfile(state.sourceDirectory, fileName);

        fid = fopen(filePath, 'rb');
        if fid == -1
            fprintf('WARNING: Cannot open %s — skipping\n', fileName);
            state.currentFileIdx = state.currentFileIdx + 1;
            continue;
        end
        fileData = fread(fid, '*uint8');
        fclose(fid);

        fileSize = numel(fileData);
        fprintf('Loading %d/%d: %s (%.2f KB)\n', ...
            state.currentFileIdx, numel(state.fileList), fileName, fileSize/1024);

        % Build framed packet
        nameBytes = uint8(zeros(state.FILENAME_LENGTH, 1));
        nb = min(numel(uint8(fileName)), state.FILENAME_LENGTH);
        nameBytes(1:nb) = uint8(fileName(1:nb));

        sizeBytes = typecast(uint64(fileSize), 'uint8');

        stream = state.START_FLAG(:);
        for r = 1:state.REPETITIONS; stream = [stream; nameBytes]; end
        for r = 1:state.REPETITIONS; stream = [stream; sizeBytes(:)]; end
        stream = [stream; fileData(:)];
        stream = [stream; state.END_FLAG(:)];

        state.streamBuffer = [state.streamBuffer; stream];
        fprintf('  Buffer now: %.2f KB\n', numel(state.streamBuffer)/1024);
        state.currentFileIdx = state.currentFileIdx + 1;
    end

    % ── Done? ─────────────────────────────────────────────────────────────────
    if state.currentFileIdx > numel(state.fileList) && isempty(state.streamBuffer)
        fprintf('\n========================================\nTRANSMISSION COMPLETE\n');
        fprintf('Total files sent: %d\n========================================\n', ...
            numel(state.fileList));
        state = [];  % reset persistent for next run
        error('MATLAB:MEX:ErrMsgTxt', 'All files transmitted');
    end

    % ── Carve out a batch ─────────────────────────────────────────────────────
    available = numel(state.streamBuffer);
    if available == 0; return; end

    maxPkts  = ceil(available / PACKET_SIZE);
    numPkts  = min(BATCH_SIZE, maxPkts);
    bytesSend = min(numPkts * PACKET_SIZE, available);

    outBuf = zeros(BATCH_SIZE * PACKET_SIZE, 1, 'int8');
    outBuf(1:bytesSend) = int8(int32(state.streamBuffer(1:bytesSend)) - 128);

    state.streamBuffer(1:bytesSend) = [];

    fprintf('Sending %d pkts (buffer remaining: %.2f KB)\n', ...
        numPkts, numel(state.streamBuffer)/1024);

    % ── Write to pipe ─────────────────────────────────────────────────────────
    out.write(outBuf, numPkts);
end