function socketObj = matlab_socket_client(host, port, maxRetries)
% MATLAB_SOCKET_CLIENT - Connect to Electron via TCP socket (INSTANCE-AWARE)
%
% Usage:
%   socketObj = matlab_socket_client()  % Uses MATLAB_PORT env var
%   socketObj = matlab_socket_client('localhost', 9001)
%   socketObj = matlab_socket_client('localhost', 9001, 10)
%
% Inputs:
%   host       - Socket host (default: 'localhost')
%   port       - Socket port (default: from MATLAB_PORT env var, or 9001)
%   maxRetries - Max connection attempts (default: 10)
%
% Output:
%   socketObj  - TCP client object, or [] if connection failed
%
% Environment Variables:
%   MATLAB_PORT - Instance-specific MATLAB socket port (set by Electron)

    if nargin < 1, host = 'localhost'; end
    
    % Get port from environment variable if available
    if nargin < 2
        matlabPortStr = getenv('MATLAB_PORT');
        if ~isempty(matlabPortStr)
            port = str2double(matlabPortStr);
            fprintf('[SOCKET] Using instance-specific port from env: %d\n', port);
        else
            port = 9001;  % Fallback default
            fprintf('[SOCKET] Warning: MATLAB_PORT not set, using default: %d\n', port);
        end
    end
    
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