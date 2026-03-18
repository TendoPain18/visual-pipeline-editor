import { topologicalSort } from '../utils/blockUtils';

export const useServerOperations = ({
  blocks,
  connections,
  projectDir,
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
      await window.electronAPI.ensureDir(`${projectDir}/cpp`);
      await window.electronAPI.ensureDir(`${projectDir}/blocks`);

      // Check if MEX file exists
      const platform = await window.electronAPI.getPlatform();
      const mexExtension = platform === 'win32' ? 'mexw64' : (platform === 'darwin' ? 'mexmaci64' : 'mexa64');
      const mexPath = `${projectDir}/cpp/pipeline_mex.${mexExtension}`;
      
      let mexExists = false;
      try {
        await window.electronAPI.readFile(mexPath);
        mexExists = true;
        addLog('success', 'Found existing MEX file - skipping compilation');
      } catch (e) {
        addLog('info', 'MEX file not found - will compile');
      }

      if (!mexExists) {
        // Check if pipeline_mex.cpp exists
        const cppSourcePath = `${projectDir}/cpp/pipeline_mex.cpp`;
        let sourceExists = false;
        try {
          await window.electronAPI.readFile(cppSourcePath);
          sourceExists = true;
        } catch (e) {
          throw new Error('pipeline_mex.cpp not found in cpp/ folder. Please add the pipeline_mex.cpp file to the cpp/ directory.');
        }

        if (sourceExists) {
          addLog('info', 'Found pipeline_mex.cpp - compiling MEX file...');
          const mexResult = await window.electronAPI.execCommand(
            'matlab -batch "mex pipeline_mex.cpp"',
            `${projectDir}/cpp`
          );

          if (!mexResult.success) {
            throw new Error(`MEX compilation failed:\n${mexResult.stderr || mexResult.error}\n\nMake sure MATLAB is installed and in your PATH.`);
          }
          addLog('success', 'MEX compiled successfully');
        }
      }

      // Check if pipe_server.exe exists
      const serverPath = `${projectDir}/cpp/pipe_server.exe`;
      let serverExists = false;
      try {
        await window.electronAPI.readFile(serverPath);
        serverExists = true;
        addLog('success', 'Found existing pipe server - skipping compilation');
      } catch (e) {
        addLog('info', 'Pipe server not found - checking for source...');
      }

      if (!serverExists) {
        // Check if pipe_server.cpp exists
        const cppSourcePath = `${projectDir}/cpp/pipe_server.cpp`;
        let sourceExists = false;
        try {
          await window.electronAPI.readFile(cppSourcePath);
          sourceExists = true;
        } catch (e) {
          throw new Error('pipe_server.cpp not found in cpp/ folder. Please add the pipe_server.cpp file to the cpp/ directory.');
        }

        if (sourceExists) {
          addLog('info', 'Found pipe_server.cpp - compiling...');
          const cppResult = await window.electronAPI.execCommand(
            'g++ -o pipe_server.exe pipe_server.cpp -lkernel32 -O2',
            `${projectDir}/cpp`
          );

          if (!cppResult.success) {
            throw new Error(`Pipe server compilation failed:\n${cppResult.stderr || cppResult.error}\n\nMake sure g++ is installed and in your PATH.`);
          }
          addLog('success', 'Pipe server compiled successfully');
        }
      }

      // Build server command with parameters
      const sortedBlocks = topologicalSort(blocks, connections);
      const pipes = connections.map((_, i) => `GlobalP${i + 1}`);
      
      let serverCmd = `pipe_server.exe ${connections.length}`;
      
      connections.forEach((conn, i) => {
        const fromBlock = sortedBlocks.find(b => b.id === conn.fromBlock);
        
        // Get size for specific output port
        let size = 67108864; // default 64MB
        if (fromBlock) {
          if (Array.isArray(fromBlock.outputSize)) {
            size = fromBlock.outputSize[conn.fromPort] || fromBlock.outputSize[0] || size;
          } else {
            size = fromBlock.outputSize || size;
          }
        }
        
        serverCmd += ` ${pipes[i]} ${size}`;
      });

      addLog('info', `Starting parameterized pipe server...`);
      addLog('info', `Command: ${serverCmd}`);
      
      const serverProc = await window.electronAPI.startProcess(
        serverCmd,
        `${projectDir}/cpp`,
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
      addLog('success', `Pipe server started (PID: ${serverProc.pid})`);
      
    } catch (err) {
      addLog('error', `Server start failed: ${err.message}`);
      alert('Failed to start server:\n' + err.message);
    } finally {
      setIsStartingServer(false);
    }
  };

  const buildBlockCommand = (block, connections) => {
    const pipes = connections.map((_, i) => `GlobalP${i + 1}`);
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
    
    return matlabCmd;
  };

  const handleStart = async () => {
    if (!projectDir) {
      alert('Loading project directory...');
      return;
    }

    try {
      addLog('info', '========================================');
      addLog('info', 'Starting all blocks with startWithAll=true...');
      addLog('info', '========================================');
      
      for (const block of blocks) {
        await window.electronAPI.writeFile(
          `${projectDir}/blocks/${block.fileName}`,
          block.code
        );
      }

      const sortedBlocks = topologicalSort(blocks, connections);
      
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
          const matlabCmd = buildBlockCommand(block, connections);
          
          addLog('info', `Starting ${block.name}...`);
          
          const platform = await window.electronAPI.getPlatform();
          const envCmd = platform === 'win32' 
            ? `set BLOCK_ID=${block.id} && `
            : `export BLOCK_ID=${block.id} && `;
          
          const fullCommand = `${envCmd}matlab -batch "cd('${projectDir}/blocks'); addpath('${projectDir}/cpp'); ${matlabCmd}"`;
          
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
            addLog('info', `${block.name} process started (PID: ${procResult.pid}), waiting for BLOCK_READY...`);
            startedCount++;
          } else {
            addLog('error', `Failed to start ${block.name}: ${procResult.error}`);
          }

          await new Promise(resolve => setTimeout(resolve, 2000));
        } catch (err) {
          addLog('error', `Failed to start ${block.name}: ${err.message}`);
        }
      }

      addLog('info', '========================================');
      addLog('success', `Block launch complete: ${startedCount} started, ${skippedCount} skipped`);
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
    if (blockProcesses[block.id]) {
      addLog('warning', `${block.name} is already running`);
      return;
    }

    try {
      addLog('info', `Manually starting block: ${block.name}`);
      
      const sortedBlocks = topologicalSort(blocks, connections);

      await window.electronAPI.writeFile(
        `${projectDir}/blocks/${block.fileName}`,
        block.code
      );

      const matlabCmd = buildBlockCommand(block, connections);

      addLog('info', `Starting ${block.name}...`);
      
      const platform = await window.electronAPI.getPlatform();
      const envCmd = platform === 'win32' 
        ? `set BLOCK_ID=${block.id} && `
        : `export BLOCK_ID=${block.id} && `;
      
      const procResult = await window.electronAPI.startProcess(
        `${envCmd}matlab -batch "cd('${projectDir}/blocks'); addpath('${projectDir}/cpp'); ${matlabCmd}"`,
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
