function channel_encode_middleman(pipeInRate, pipeInData, pipeInFeedback, pipeOutSignal, pipeOutData)
% CHANNEL_ENCODE_MIDDLEMAN — batch processing, deadlock-free protocol
%
% Pipe sizes:
%   Input 1  (pipeInRate):     3 bytes/pkt    × 44740 batch  — rate+len from channel_encode (DISCARDED)
%   Input 2  (pipeInData):     3030 bytes/pkt × 44740 batch  — encoded SIGNAL+DATA from channel_encode
%   Input 3  (pipeInFeedback): 3 bytes/pkt    × 44740 batch  — rate+length from ppdu_decapsulate
%   Output 1 (pipeOutSignal):  6 bytes/pkt    × 44740 batch  — SIGNAL field → channel_decode input 1
%   Output 2 (pipeOutData):    3025 bytes/pkt × 44740 batch  — rate(1) + DATA → channel_decode input 2
%
% Per-frame deadlock-free protocol (same as C++ version):
%   1. Read & discard rate_encoded_length  (pipeIn{1})
%   2. Read full encoded packet            (pipeIn{2})
%   3. Send SIGNAL batch (bytes 1-6)       (pipeOut{1}) ← unblocks channel_decode SIGNAL read
%   4. Wait for rate_length feedback batch (pipeIn{3})  ← gate from ppdu_decapsulate
%   5. Send rate + DATA batch              (pipeOut{2}) ← unblocks channel_decode DATA read

    block_config = struct( ...
        'name',              'ChannelEncodeMiddleman', ...
        'inputs',            3, ...
        'outputs',           2, ...
        'inputPacketSizes',  [3,     3030,  3], ...
        'inputBatchSizes',   [44740, 44740, 44740], ...
        'outputPacketSizes', [6,     3025], ...
        'outputBatchSizes',  [44740, 44740], ...
        'LTR',               false, ...
        'startWithAll',      true, ...
        'socketHost',        'localhost', ...
        'socketPort',        9001 ...
    );

    run_generic_block( ...
        {pipeInRate, pipeInData, pipeInFeedback}, ...
        {pipeOutSignal, pipeOutData}, ...
        block_config, @process_fn, []);
end

%% ─── Process ─────────────────────────────────────────────────────────────────
function process_fn(pipeIn, pipeOut, ~, ~)
    IN_DATA_PKT  = pipeIn{2}.getPacketSize();   % 3030
    OUT_SIG_PKT  = pipeOut{1}.getPacketSize();  % 6
    OUT_DATA_PKT = pipeOut{2}.getPacketSize();  % 3025
    BATCH        = pipeIn{2}.getBatchSize();    % 44740

    % STEP 1: Read and discard rate_encoded_length
    [~, actualCount] = pipeIn{1}.read();

    % STEP 2: Read full encoded packet batch
    [dataRaw, ~] = pipeIn{2}.read();

    % STEP 3: Split and send SIGNAL batch immediately (unblocks channel_decode)
    sigOut = zeros(BATCH * OUT_SIG_PKT, 1, 'int8');
    for i = 1:actualCount
        dOff = (i-1) * IN_DATA_PKT;
        sOff = (i-1) * OUT_SIG_PKT;
        sigOut(sOff+1 : sOff+6) = dataRaw(dOff+1 : dOff+6);
    end
    pipeOut{1}.write(sigOut, actualCount);

    % STEP 4: Wait for rate_length feedback batch (GATE — from ppdu_decapsulate)
    [feedbackRaw, ~] = pipeIn{3}.read();

    % STEP 5: Send rate + encoded DATA batch (unblocks channel_decode DATA read)
    dataOut = zeros(BATCH * OUT_DATA_PKT, 1, 'int8');
    for i = 1:actualCount
        dOff  = (i-1) * IN_DATA_PKT;
        fOff  = (i-1) * 3;             % feedback packet = 3 bytes
        doOff = (i-1) * OUT_DATA_PKT;

        % First byte of output = rate from feedback
        dataOut(doOff+1) = feedbackRaw(fOff+1);

        % Remaining bytes = encoded DATA (bytes 7..3030 of encoded packet)
        % bytes 1-6 were SIGNAL, bytes 7..3030 are DATA
        copyLen = min(IN_DATA_PKT - 6, OUT_DATA_PKT - 1);
        dataOut(doOff+2 : doOff+1+copyLen) = dataRaw(dOff+7 : dOff+6+copyLen);
    end
    pipeOut{2}.write(dataOut, actualCount);
end
