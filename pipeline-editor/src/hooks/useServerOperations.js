import { topologicalSort } from '../utils/blockUtils';

export const useServerOperations = ({
  blocks,
  connections,
  projectDir,
  instanceConfig,
  setServerRunning,
  setBlockProcesses,
  setIsStartingServer,
  addLog
}) => {
  const handleStartServer = async () => {
    if (!projectDir) {
      alert('Loading project directory...');
      return;
    }

    if (!instanceConfig) {
      alert('Instance configuration not loaded...');
      return;
    }

    setIsStartingServer(true);

    try {
      addLog('info', 'Validating pipeline...');
      
      if (blocks.length < 2) {
        throw new Error('Need at least 2 blocks (Source and Sink)');
      }
      
      if (connections.length === 0) {
        throw new Error('No connections found');
      }

      addLog('info', 'Creating directories...');
      await window.electronAPI.ensureDir(`${projectDir}/server`);
      await window.electronAPI.ensureDir(`${projectDir}/matlab_blocks`);

      // Check if MEX file exists
      const platform = await window.electronAPI.getPlatform();
      const mexExtension = platform === 'win32' ? 'mexw64' : (platform === 'darwin' ? 'mexmaci64' : 'mexa64');
      const mexPath = `${projectDir}/server/pipeline_mex.${mexExtension}`;
      
      let mexExists = false;
      try {
        await window.electronAPI.readFile(mexPath);
        mexExists = true;
        addLog('success', 'Found existing MEX file - skipping compilation');
      } catch (e) {
        addLog('info', 'MEX file not found - will compile');
      }

      if (!mexExists) {
        const cppSourcePath = `${projectDir}/server/pipeline_mex.cpp`;
        let sourceExists = false;
        try {
          await window.electronAPI.readFile(cppSourcePath);
          sourceExists = true;
        } catch (e) {
          throw new Error('pipeline_mex.cpp not found in server/ folder. Please add the pipeline_mex.cpp file to the server/ directory.');
        }

        if (sourceExists) {
          addLog('info', 'Found pipeline_mex.cpp - compiling MEX file...');
          const mexResult = await window.electronAPI.execCommand(
            'matlab -batch "mex pipeline_mex.cpp"',
            `${projectDir}/server`
          );

          if (!mexResult.success) {
            throw new Error(`MEX compilation failed:\n${mexResult.stderr || mexResult.error}\n\nMake sure MATLAB is installed and in your PATH.`);
          }
          addLog('success', 'MEX compiled successfully');
        }
      }

      // Check if pipe_server.exe exists
      const serverPath = `${projectDir}/server/pipe_server.exe`;
      let serverExists = false;
      try {
        await window.electronAPI.readFile(serverPath);
        serverExists = true;
        addLog('success', 'Found existing pipe server - skipping compilation');
      } catch (e) {
        addLog('info', 'Pipe server not found - checking for source...');
      }

      if (!serverExists) {
        const cppSourcePath = `${projectDir}/server/pipe_server.cpp`;
        let sourceExists = false;
        try {
          await window.electronAPI.readFile(cppSourcePath);
          sourceExists = true;
        } catch (e) {
          throw new Error('pipe_server.cpp not found in server/ folder. Please add the pipe_server.cpp file to the server/ directory.');
        }

        if (sourceExists) {
          addLog('info', 'Found pipe_server.cpp - compiling with socket support...');
          const cppResult = await window.electronAPI.execCommand(
            'g++ -o pipe_server.exe pipe_server.cpp -lws2_32 -O2',
            `${projectDir}/server`
          );

          if (!cppResult.success) {
            throw new Error(`Pipe server compilation failed:\n${cppResult.stderr || cppResult.error}\n\nMake sure g++ is installed and in your PATH.`);
          }
          addLog('success', 'Pipe server compiled successfully with socket support');
        }
      }

      // Build server command with INSTANCE-SPECIFIC parameters
      const sortedBlocks = topologicalSort(blocks, connections);
      const instanceId = instanceConfig.instanceId;
      const serverPort = instanceConfig.serverPort;
      
      // Instance-specific pipe names
      const pipes = connections.map((_, i) => `Instance_${instanceId}_P${i + 1}`);
      
      let serverCmd = `pipe_server.exe ${connections.length} ${serverPort}`;
      
      // BATCH PROCESSING: Use buffer size from output port (includes length header + all packets)
      connections.forEach((conn, i) => {
        const fromBlock = sortedBlocks.find(b => b.id === conn.fromBlock);
        
        let size = 67108864; // default 64MB
        if (fromBlock && fromBlock.outputBufferSizes && fromBlock.outputBufferSizes[conn.fromPort]) {
          // Use the TOTAL buffer size (length header + packet_size × batch_size)
          size = fromBlock.outputBufferSizes[conn.fromPort];
          
          const packetSize = fromBlock.outputPacketSizes[conn.fromPort];
          const batchSize = fromBlock.outputBatchSizes[conn.fromPort];
          const lengthBytes = fromBlock.outputLengthBytes[conn.fromPort];
          
          addLog('info', 
            `Pipe ${i}: ${fromBlock.name} port ${conn.fromPort} - ` +
            `${packetSize}×${batchSize} (${lengthBytes}B header + ${(size - lengthBytes) / 1024}KB data = ${(size / 1024).toFixed(2)}KB total)`
          );
        }
        
        serverCmd += ` ${pipes[i]} ${size}`;
      });

      addLog('info', `Starting instance-specific pipe server...`);
      addLog('info', `Instance ID: ${instanceId}`);
      addLog('info', `Server Port: ${serverPort}`);
      addLog('info', `Command: ${serverCmd}`);
      
      const serverProc = await window.electronAPI.startServerWithSocket(
        serverCmd,
        `${projectDir}/server`,
        'server'
      );

      if (!serverProc.success) {
        throw new Error('Failed to start server: ' + serverProc.error);
      }

      setServerRunning(true);
      setBlockProcesses(prev => ({ 
        ...prev, 
        server: { pid: serverProc.pid, status: 'running', name: 'server' } 
      }));
      addLog('success', `Pipe server started (PID: ${serverProc.pid}, Port: ${serverPort})`);
      addLog('info', 'Waiting for socket connection...');
      
    } catch (err) {
      addLog('error', `Server start failed: ${err.message}`);
      alert('Failed to start server:\n' + err.message);
    } finally {
      setIsStartingServer(false);
    }
  };

  const buildBlockCommand = (block, connections, instanceConfig) => {
    const instanceId = instanceConfig.instanceId;
    const matlabPort = instanceConfig.matlabPort;
    
    // Instance-specific pipe names
    const pipes = connections.map((_, i) => `Instance_${instanceId}_P${i + 1}`);
    const functionName = block.fileName.replace('.m', '');
    
    const inputConnections = connections.filter(c => c.toBlock === block.id)
      .sort((a, b) => a.toPort - b.toPort);
    
    const outputConnections = connections.filter(c => c.fromBlock === block.id)
      .sort((a, b) => a.fromPort - b.fromPort);
    
    const args = [];
    
    for (let i = 0; i < block.inputs; i++) {
      const conn = inputConnections.find(c => c.toPort === i);
      if (conn) {
        const pipeIndex = connections.indexOf(conn);
        args.push(`'${pipes[pipeIndex]}'`);
      } else {
        throw new Error(`Block ${block.name} input ${i} is not connected`);
      }
    }
    
    for (let i = 0; i < block.outputs; i++) {
      const conn = outputConnections.find(c => c.fromPort === i);
      if (conn) {
        const pipeIndex = connections.indexOf(conn);
        args.push(`'${pipes[pipeIndex]}'`);
      } else {
        throw new Error(`Block ${block.name} output ${i} is not connected`);
      }
    }
    
    const matlabCmd = `${functionName}(${args.join(', ')})`;
    
    return `addpath('${projectDir}/matlab_blocks/core'); ${matlabCmd}`;
  };

  const waitForBlockReady = (blockId, blockName, setBlockProcesses, timeout = 30000) => {
    return new Promise((resolve, reject) => {
      const startTime = Date.now();
      let isReady = false;
      
      const checkInterval = setInterval(() => {
        setBlockProcesses(prev => {
          const proc = prev[blockId];
          if (proc && proc.status === 'running' && !isReady) {
            isReady = true;
            clearInterval(checkInterval);
            resolve(true);
          } else if (Date.now() - startTime > timeout && !isReady) {
            clearInterval(checkInterval);
            reject(new Error(`Timeout waiting for ${blockName} to become ready`));
          }
          return prev;
        });
      }, 100);
    });
  };

  const handleStart = async () => {
    if (!projectDir) {
      alert('Loading project directory...');
      return;
    }

    if (!instanceConfig) {
      alert('Instance configuration not loaded...');
      return;
    }

    try {
      addLog('info', '========================================');
      addLog('info', 'Starting blocks sequentially (waiting for each to be ready)...');
      addLog('info', '========================================');
      
      for (const block of blocks) {
        await window.electronAPI.writeFile(
          `${projectDir}/matlab_blocks/${block.fileName}`,
          block.code
        );
      }

      const sortedBlocks = topologicalSort(blocks, connections);
      const blocksToStart = sortedBlocks.filter(b => b.startWithAll);
      
      let startedCount = 0;
      let skippedCount = 0;

      for (let i = 0; i < sortedBlocks.length; i++) {
        const block = sortedBlocks[i];
        
        if (!block.startWithAll) {
          addLog('info', `Skipping ${block.name} (startWithAll=false)`);
          skippedCount++;
          continue;
        }

        try {
          const matlabCmd = buildBlockCommand(block, connections, instanceConfig);
          
          const currentBlockNum = startedCount + 1;
          const totalBlocks = blocksToStart.length;
          addLog('info', `[${currentBlockNum}/${totalBlocks}] Starting ${block.name}...`);
          
          const platform = await window.electronAPI.getPlatform();
          
          // Pass BOTH block ID, instance ID, and MATLAB port as environment variables
          const envCmd = platform === 'win32' 
            ? `set BLOCK_ID=${block.id} && set INSTANCE_ID=${instanceConfig.instanceId} && set MATLAB_PORT=${instanceConfig.matlabPort} && `
            : `export BLOCK_ID=${block.id} && export INSTANCE_ID=${instanceConfig.instanceId} && export MATLAB_PORT=${instanceConfig.matlabPort} && `;
          
          const fullCommand = `${envCmd}matlab -batch "cd('${projectDir}/matlab_blocks'); addpath('${projectDir}/server'); ${matlabCmd}"`;
          
          const procResult = await window.electronAPI.startProcess(
            fullCommand,
            projectDir,
            block.name
          );

          if (procResult.success) {
            setBlockProcesses(prev => ({
              ...prev,
              [block.id]: {
                pid: procResult.pid,
                status: 'starting',
                name: block.name
              }
            }));
            addLog('info', `${block.name} process launched (PID: ${procResult.pid}), waiting for BLOCK_READY...`);
            
            try {
              await waitForBlockReady(block.id, block.name, setBlockProcesses);
              addLog('success', `✓ ${block.name} is READY`);
              startedCount++;
            } catch (err) {
              addLog('error', `✗ ${block.name} failed to become ready: ${err.message}`);
              addLog('warning', 'Continuing with next block...');
            }
          } else {
            addLog('error', `Failed to start ${block.name}: ${procResult.error}`);
          }
        } catch (err) {
          addLog('error', `Failed to start ${block.name}: ${err.message}`);
        }
      }

      addLog('info', '========================================');
      addLog('success', `Sequential startup complete: ${startedCount} started, ${skippedCount} skipped`);
      addLog('info', '========================================');
    } catch (err) {
      addLog('error', `Block start failed: ${err.message}`);
      alert('Failed to start blocks:\n' + err.message);
    }
  };

  const handleStop = async (blockProcesses, setBlockMetrics) => {
    addLog('info', 'Stopping all blocks with startWithAll=true...');
    
    const blocksToStop = blocks.filter(b => b.startWithAll);
    
    for (const block of blocksToStop) {
      const processInfo = blockProcesses[block.id];
      if (processInfo) {
        addLog('info', `Stopping ${block.name} (PID: ${processInfo.pid})...`);
        const result = await window.electronAPI.killProcess(processInfo.pid);
        if (result.success) {
          addLog('success', `${block.name} stopped`);
        } else {
          addLog('error', `Failed to stop ${block.name}: ${result.error}`);
        }
      }
    }
    
    setBlockProcesses(prev => {
      const newProcs = { ...prev };
      blocksToStop.forEach(block => {
        delete newProcs[block.id];
      });
      return newProcs;
    });
    
    setBlockMetrics(prev => {
      const newMetrics = { ...prev };
      blocksToStop.forEach(block => {
        delete newMetrics[block.id];
      });
      return newMetrics;
    });
    
    addLog('success', 'All auto-start blocks stopped');
  };

  const handleTerminate = async (setBlockMetrics) => {
    addLog('info', 'Terminating all processes...');
    
    const result = await window.electronAPI.killAllProcesses();
    
    if (result.success) {
      addLog('success', `Terminated ${result.killedCount} process(es)`);
      setBlockProcesses({});
      setServerRunning(false);
      setBlockMetrics({});
    } else {
      addLog('error', 'Failed to terminate all processes');
    }
  };

  const handleStartBlock = async (block, blockProcesses) => {
    if (!instanceConfig) {
      addLog('error', 'Instance configuration not loaded');
      return;
    }

    if (blockProcesses[block.id]) {
      addLog('warning', `${block.name} is already running`);
      return;
    }

    try {      
      const sortedBlocks = topologicalSort(blocks, connections);

      await window.electronAPI.writeFile(
        `${projectDir}/blocks/${block.fileName}`,
        block.code
      );

      const matlabCmd = buildBlockCommand(block, connections, instanceConfig);

      addLog('info', `Starting ${block.name}...`);
      
      const platform = await window.electronAPI.getPlatform();
      const envCmd = platform === 'win32' 
        ? `set BLOCK_ID=${block.id} && set INSTANCE_ID=${instanceConfig.instanceId} && set MATLAB_PORT=${instanceConfig.matlabPort} && `
        : `export BLOCK_ID=${block.id} && export INSTANCE_ID=${instanceConfig.instanceId} && export MATLAB_PORT=${instanceConfig.matlabPort} && `;
      
      const procResult = await window.electronAPI.startProcess(
        `${envCmd}matlab -batch "cd('${projectDir}/blocks'); addpath('${projectDir}/server'); ${matlabCmd}"`,
        projectDir,
        block.name
      );

      if (procResult.success) {
        setBlockProcesses(prev => ({
          ...prev,
          [block.id]: { pid: procResult.pid, status: 'starting', name: block.name }
        }));
        addLog('info', `${block.name} process started (PID: ${procResult.pid}), waiting for BLOCK_READY...`);
      } else {
        throw new Error(procResult.error);
      }
    } catch (err) {
      addLog('error', `Failed to start ${block.name}: ${err.message}`);
    }
  };

  const handleStopBlock = async (block, blockProcesses, setBlockMetrics) => {
    const processInfo = blockProcesses[block.id];
    if (processInfo) {
      addLog('info', `Stopping ${block.name} (PID: ${processInfo.pid})...`);
      const result = await window.electronAPI.killProcess(processInfo.pid);
      if (result.success) {
        addLog('success', `${block.name} stopped`);
        setBlockProcesses(prev => {
          const newProcs = { ...prev };
          delete newProcs[block.id];
          return newProcs;
        });
        setBlockMetrics(prev => {
          const newMetrics = { ...prev };
          delete newMetrics[block.id];
          return newMetrics;
        });
      } else {
        addLog('error', `Failed to stop ${block.name}: ${result.error}`);
      }
    }
  };

  return {
    handleStartServer,
    handleStart,
    handleStop,
    handleTerminate,
    handleStartBlock,
    handleStopBlock
  };
};