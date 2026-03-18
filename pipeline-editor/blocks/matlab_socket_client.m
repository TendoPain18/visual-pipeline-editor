function socketObj = matlab_socket_client(host, port, maxRetries)
% MATLAB_SOCKET_CLIENT - Connect to Electron via TCP socket
%
% Usage:
%   socketObj = matlab_socket_client('localhost', 9001)
%   socketObj = matlab_socket_client('localhost', 9001, 10)
%
% Inputs:
%   host       - Socket host (default: 'localhost')
%   port       - Socket port (default: 9001)
%   maxRetries - Max connection attempts (default: 10)
%
% Output:
%   socketObj  - TCP client object, or [] if connection failed

    if nargin < 1, host = 'localhost'; end
    if nargin < 2, port = 9001; end
    if nargin < 3, maxRetries = 10; end
    
    socketObj = [];
    
    for attempt = 1:maxRetries
        try
            % Create TCP client
            socketObj = tcpclient(host, port);
            socketObj.Timeout = 5;
            
            fprintf('[SOCKET] ✓ Connected to %s:%d (attempt %d/%d)\n', ...
                    host, port, attempt, maxRetries);
            return;
            
        catch ME
            if attempt < maxRetries
                fprintf('[SOCKET] Connection failed (attempt %d/%d), retrying in 0.5s...\n', ...
                        attempt, maxRetries);
                pause(0.5);
            else
                fprintf('[SOCKET] ✗ Failed to connect after %d attempts: %s\n', ...
                        maxRetries, ME.message);
                socketObj = [];
                return;
            end
        end
    end
end