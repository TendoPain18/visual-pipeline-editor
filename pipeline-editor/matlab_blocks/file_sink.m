function file_sink(pipeIn)
% FILE_SINK  Streaming file sink — writes on-the-fly, no temp files.

    block_config = struct( ...
        'name',             'FileSink', ...
        'inputs',           1, ...
        'outputs',          0, ...
        'inputPacketSizes', 1501, ...
        'inputBatchSizes',  44740, ...
        'LTR',              false, ...
        'startWithAll',     true, ...
        'socketHost',       'localhost', ...
        'socketPort',       9001, ...
        'outputDirectory',  'C:\Users\amrga\Downloads\final\pipeline-editor\Output_Files', ...
        'description',      'Streaming file sink — writes each file live' ...
    );

    run_generic_block({pipeIn}, {}, block_config, @process_fn, @init_fn);
end

%% ─── Init ───────────────────────────────────────────────────────────────────
function state = init_fn(config)
    START_FLAG      = uint8([0xAA, 0x55, 0xAA, 0x55]);
    END_FLAG        = uint8([0x55, 0xAA, 0x55, 0xAA]);
    FILENAME_LENGTH = 256;
    REPETITIONS     = 10;

    outDir = config.outputDirectory;
    fprintf('[DEBUG init] outputDirectory = %s\n', outDir);

    if ~exist(outDir, 'dir')
        fprintf('[DEBUG init] Directory does not exist — creating...\n');
        [ok, msg] = mkdir(outDir);
        fprintf('[DEBUG init] mkdir result: ok=%d msg=%s\n', ok, msg);
    else
        fprintf('[DEBUG init] Directory exists OK\n');
    end

    reportPath = fullfile(outDir, 'error_report.txt');
    fprintf('[DEBUG init] Report path = %s\n', reportPath);

    fid = fopen(reportPath, 'w');
    fprintf('[DEBUG init] fopen report fid = %d\n', fid);
    if fid ~= -1
        fprintf(fid, '========================================\n');
        fprintf(fid, 'FILE TRANSMISSION ERROR REPORT\n');
        fprintf(fid, '========================================\n');
        fprintf(fid, 'Started: %s\nMode: STREAMING (live write)\n\n', datestr(now));
        fprintf(fid, '========================================\n\n');
        fclose(fid);
        fprintf('[DEBUG init] Report file written and closed OK\n');
    else
        fprintf('[DEBUG init] ERROR: Cannot open report file for writing!\n');
    end

    fprintf('[FileSink] Output directory: %s\n', outDir);
    fprintf('[FileSink] Error report:     %s\n', reportPath);

    state = struct( ...
        'streamBuffer',    uint8([]), ...
        'currentFile',     struct('active', false, 'fid', -1), ...
        'filesReceived',   0, ...
        'totalErrorCount', uint64(0), ...
        'outputDirectory', outDir, ...
        'reportPath',      reportPath, ...
        'START_FLAG',      START_FLAG, ...
        'END_FLAG',        END_FLAG, ...
        'FILENAME_LENGTH', FILENAME_LENGTH, ...
        'REPETITIONS',     REPETITIONS, ...
        'lastProgressPct', -1, ...
        'batchCount',      0 ...
    );

    fprintf('[DEBUG init] State struct created OK\n');
end

%% ─── Process ────────────────────────────────────────────────────────────────
function process_fn(pipeIn, ~, initState, ~)
    persistent state;
    if isempty(state)
        fprintf('[DEBUG] First call — initializing persistent state\n');
        state = initState;
        fprintf('[DEBUG] outputDirectory in state = %s\n', state.outputDirectory);
    end

    inp         = pipeIn{1};
    PACKET_SIZE = inp.getPacketSize();   % 1501
    DATA_SIZE   = PACKET_SIZE - 1;       % 1500

    % ── Read a batch ──────────────────────────────────────────────────────────
    [inputBatch, actualCount] = inp.read();

    state.batchCount = state.batchCount + 1;

    fprintf('[DEBUG batch #%d] actualCount=%d PACKET_SIZE=%d DATA_SIZE=%d\n', ...
        state.batchCount, actualCount, PACKET_SIZE, DATA_SIZE);
    fprintf('[DEBUG batch #%d] inputBatch size=%d class=%s\n', ...
        state.batchCount, numel(inputBatch), class(inputBatch));

    % ── EOF signal ────────────────────────────────────────────────────────────
    if actualCount == 0
        fprintf('[DEBUG] actualCount==0 — EOF signal received\n');
        if state.currentFile.active && state.currentFile.fid ~= -1
            fclose(state.currentFile.fid);
            reportFileComplete(state.reportPath, state.currentFile);
            state.filesReceived = state.filesReceived + 1;
            state.currentFile.active = false;
        end
        rfid = fopen(state.reportPath, 'a');
        if rfid ~= -1
            fprintf(rfid, '========================================\nSUMMARY\n');
            fprintf(rfid, 'Finished: %s\nTotal files: %d\nTotal errors: %d\n', ...
                datestr(now), state.filesReceived, state.totalErrorCount);
            fprintf(rfid, '========================================\n');
            fclose(rfid);
        end
        fprintf('\n[FileSink] ALL FILES RECEIVED — %d files, %d errors\n', ...
            state.filesReceived, state.totalErrorCount);
        state = [];
        error('MATLAB:MEX:ErrMsgTxt', 'Block finished');
    end

    % ── Unpack batch ──────────────────────────────────────────────────────────
    validBytes  = actualCount * PACKET_SIZE;
    batchMatrix = reshape(inputBatch(1:validBytes), PACKET_SIZE, actualCount);
    newData     = uint8(int32(batchMatrix(1:DATA_SIZE, :)) + 128);  % DATA_SIZE x actualCount
    errorFlags  = batchMatrix(PACKET_SIZE, :);

    batchErrors           = uint64(sum(errorFlags ~= 0));
    state.totalErrorCount = state.totalErrorCount + batchErrors;

    % Show first few bytes of decoded data for debug
    flatData = newData(:);
    fprintf('[DEBUG batch #%d] decoded %d bytes, first 8 bytes: %s\n', ...
        state.batchCount, numel(flatData), ...
        num2str(double(flatData(1:min(8,end))'), '%02X '));

    % Append to stream buffer
    prevLen = numel(state.streamBuffer);
    state.streamBuffer = [state.streamBuffer; flatData];
    fprintf('[DEBUG batch #%d] streamBuffer: %d -> %d bytes, currentFile.active=%d\n', ...
        state.batchCount, prevLen, numel(state.streamBuffer), state.currentFile.active);

    if state.currentFile.active
        state.currentFile.errorCount = state.currentFile.errorCount + batchErrors;
    end

    % ── Parse stream ──────────────────────────────────────────────────────────
    loopCount = 0;
    while true
        loopCount = loopCount + 1;
        fprintf('[DEBUG parse loop %d] bufLen=%d active=%d\n', ...
            loopCount, numel(state.streamBuffer), state.currentFile.active);

        if ~state.currentFile.active
            % Show first bytes of buffer to see what we have
            showN = min(16, numel(state.streamBuffer));
            if showN > 0
                fprintf('[DEBUG parse] buffer start: %s\n', ...
                    num2str(double(state.streamBuffer(1:showN)'), '%02X '));
            end

            startPos = findPattern(state.streamBuffer, state.START_FLAG);
            fprintf('[DEBUG parse] findPattern(START_FLAG) -> startPos=%s\n', ...
                num2str(startPos));

            if isempty(startPos)
                fprintf('[DEBUG parse] No START flag found — keeping buffer as-is\n');
                % DO NOT trim buffer — keep everything in case START spans batches
                break;
            end

            metaSize = 4 + state.FILENAME_LENGTH*state.REPETITIONS + 8*state.REPETITIONS;
            fprintf('[DEBUG parse] startPos=%d metaSize=%d bufLen=%d needed=%d\n', ...
                startPos, metaSize, numel(state.streamBuffer), startPos+metaSize-1);

            if numel(state.streamBuffer) < startPos + metaSize - 1
                fprintf('[DEBUG parse] Not enough bytes for metadata yet — waiting\n');
                break;
            end

            % Extract filename
            nameStart = startPos + 4;
            names     = cell(state.REPETITIONS, 1);
            for r = 1:state.REPETITIONS
                nb = state.streamBuffer(nameStart+(r-1)*state.FILENAME_LENGTH : ...
                                        nameStart+r*state.FILENAME_LENGTH-1);
                z = find(nb == 0, 1);
                if ~isempty(z); nb = nb(1:z-1); end
                names{r} = char(nb');
            end
            fileName = majorityVote(names);
            fprintf('[DEBUG parse] fileName = "%s"\n', fileName);

            % Extract file size
            szStart = nameStart + state.FILENAME_LENGTH * state.REPETITIONS;
            sizes   = zeros(state.REPETITIONS, 1, 'uint64');
            for r = 1:state.REPETITIONS
                raw      = state.streamBuffer(szStart+(r-1)*8 : szStart+r*8-1);
                sizes(r) = typecast(uint8(raw), 'uint64');
            end
            fileSize = mode(sizes);
            fprintf('[DEBUG parse] fileSize = %d bytes (%.2f KB)\n', fileSize, fileSize/1024);

            % Open final file
            safeName  = regexprep(fileName, '[\\/:*?"<>|]', '_');
            finalPath = fullfile(state.outputDirectory, safeName);
            fprintf('[DEBUG parse] Opening file: %s\n', finalPath);

            fid = fopen(finalPath, 'wb');
            fprintf('[DEBUG parse] fopen fid = %d\n', fid);

            if fid == -1
                fprintf('[DEBUG parse] ERROR: fopen failed! Skipping past START flag.\n');
                state.streamBuffer = state.streamBuffer(startPos+4:end);
                break;
            end

            fprintf('[FileSink] Started: %s (%.2f MB)\n', fileName, fileSize/1e6);

            rfid = fopen(state.reportPath, 'a');
            if rfid ~= -1
                fprintf(rfid, '[%s] STARTED: %s (%.2f MB)\n', datestr(now), fileName, fileSize/1e6);
                fclose(rfid);
            end

            state.currentFile = struct( ...
                'active',       true, ...
                'fid',          fid, ...
                'name',         fileName, ...
                'finalPath',    finalPath, ...
                'expectedSize', fileSize, ...
                'writtenBytes', uint64(0), ...
                'errorCount',   uint64(0) ...
            );
            state.lastProgressPct = -1;

            dataStart = startPos + metaSize;
            state.streamBuffer = state.streamBuffer(dataStart:end);
            fprintf('[DEBUG parse] metadata consumed, buffer now %d bytes\n', numel(state.streamBuffer));

        else
            remaining = state.currentFile.expectedSize - state.currentFile.writtenBytes;
            fprintf('[DEBUG write] remaining=%d bufLen=%d\n', remaining, numel(state.streamBuffer));

            if remaining == 0
                if numel(state.streamBuffer) < 4; break; end
                isEnd = isequal(state.streamBuffer(1:4), state.END_FLAG(:));
                fprintf('[DEBUG write] remaining==0, END flag match=%d\n', isEnd);

                state.streamBuffer = state.streamBuffer(5:end);
                fclose(state.currentFile.fid);
                reportFileComplete(state.reportPath, state.currentFile);

                if state.currentFile.errorCount > 0
                    errRate = 100*double(state.currentFile.errorCount)/double(state.currentFile.expectedSize);
                    fprintf('[FileSink] Completed: %s — %d errors (%.4f%%)\n', ...
                        state.currentFile.name, state.currentFile.errorCount, errRate);
                else
                    fprintf('[FileSink] Completed: %s — Clean\n', state.currentFile.name);
                end

                state.filesReceived = state.filesReceived + 1;
                state.currentFile.active = false;

            else
                toWrite = min(remaining, uint64(numel(state.streamBuffer)));
                if toWrite == 0; break; end

                nWritten = fwrite(state.currentFile.fid, state.streamBuffer(1:toWrite), 'uint8');
                fprintf('[DEBUG write] toWrite=%d nWritten=%d\n', toWrite, nWritten);

                state.currentFile.writtenBytes = state.currentFile.writtenBytes + uint64(nWritten);
                state.streamBuffer = state.streamBuffer(toWrite+1:end);

                pct = floor(100 * double(state.currentFile.writtenBytes) / ...
                                  double(state.currentFile.expectedSize));
                if mod(pct, 10) == 0 && pct ~= state.lastProgressPct
                    fprintf('[FileSink]   %s — %d%% (%.2f / %.2f MB)\n', ...
                        state.currentFile.name, pct, ...
                        double(state.currentFile.writtenBytes)/1e6, ...
                        double(state.currentFile.expectedSize)/1e6);
                    rfid = fopen(state.reportPath, 'a');
                    if rfid ~= -1
                        fprintf(rfid, '  [%d%%] %.2f/%.2f MB\n', pct, ...
                            double(state.currentFile.writtenBytes)/1e6, ...
                            double(state.currentFile.expectedSize)/1e6);
                        fclose(rfid);
                    end
                    state.lastProgressPct = pct;
                end
            end
        end

        if loopCount > 1000
            fprintf('[DEBUG] ERROR: parse loop exceeded 1000 iterations — breaking\n');
            break;
        end
    end
end

%% ─── Helpers ────────────────────────────────────────────────────────────────
function pos = findPattern(data, pattern)
    pos  = [];
    data = data(:);      % force column
    p    = pattern(:);   % force column
    plen = numel(p);
    dlen = numel(data);
    if dlen < plen; return; end
    for i = 1:dlen-plen+1
        if isequal(data(i:i+plen-1), p)
            pos = i; return;
        end
    end
end

function result = majorityVote(strings)
    [u, ~, idx] = unique(strings);
    counts       = histc(idx, 1:numel(u));
    [~, mi]      = max(counts);
    result       = u{mi};
end

function reportFileComplete(path, fi)
    fid = fopen(path, 'a');
    if fid == -1; return; end
    if fi.errorCount > 0
        errRate = 100 * double(fi.errorCount) / double(fi.expectedSize);
        fprintf(fid, '[%s] COMPLETED: %s\n  Size: %d bytes\n  Errors: %d (%.4f%%)\n  Status: CORRUPTED\n\n', ...
            datestr(now), fi.name, fi.expectedSize, fi.errorCount, errRate);
    else
        fprintf(fid, '[%s] COMPLETED: %s\n  Size: %d bytes\n  Errors: 0\n  Status: CLEAN\n\n', ...
            datestr(now), fi.name, fi.expectedSize);
    end
    fclose(fid);
end