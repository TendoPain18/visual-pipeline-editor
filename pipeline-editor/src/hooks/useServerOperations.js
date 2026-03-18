import { topologicalSort } from '../utils/blockUtils';
import { generateCppHeader, generateCppServer } from '../utils/codeGenerator';

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

      addLog('info', 'Generating pipeline configuration...');
      const sortedBlocks = topologicalSort(blocks, connections);
      const headerContent = generateCppHeader(sortedBlocks, connections);
      const serverContent = generateCppServer(sortedBlocks, connections);

      addLog('info', 'Creating directories...');
      await window.electronAPI.ensureDir(`${projectDir}/cpp`);
      await window.electronAPI.ensureDir(`${projectDir}/blocks`);

      // Copy protocol helper
      addLog('info', 'Copying protocol helper...');
      // Note: You'll need to ensure send_protocol_message.m is in your blocks folder

      addLog('info', 'Writing pipeline_config.h...');
      await window.electronAPI.writeFile(
        `${projectDir}/cpp/pipeline_config.h`,
        headerContent
      );

      addLog('info', 'Writing pipe_server.cpp...');
      await window.electronAPI.writeFile(
        `${projectDir}/cpp/pipe_server.cpp`,
        serverContent
      );

      addLog('info', 'Compiling MEX file...');
      const mexResult = await window.electronAPI.execCommand(
        'matlab -batch "mex pipeline_mex.cpp"',
        `${projectDir}/cpp`
      );

      if (!mexResult.success) {
        throw new Error(`MEX compilation failed:\n${mexResult.stderr || mexResult.error}`);
      }
      addLog('success', 'MEX compiled successfully');

      addLog('info', 'Compiling pipe server...');
      const cppResult = await window.electronAPI.execCommand(
        'g++ -o pipe_server.exe pipe_server.cpp -lkernel32 -O2',
        `${projectDir}/cpp`
      );

      if (!cppResult.success) {
        throw new Error(`C++ compilation failed:\n${cppResult.stderr || cppResult.error}`);
      }
      addLog('success', 'Pipe server compiled');

      addLog('info', 'Starting pipe server...');
      const serverProc = await window.electronAPI.startProcess(
        'pipe_server.exe',
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
      addLog('info', 'DEBUG: Starting block launch sequence...');
      addLog('info', '========================================');
      
      // DEBUG: Log all blocks and their startWithAll status
      addLog('info', `DEBUG: Total blocks in system: ${blocks.length}`);
      blocks.forEach((block, idx) => {
        addLog('info', `DEBUG: Block ${idx + 1}: ${block.name}`);
        addLog('info', `  - ID: ${block.id}`);
        addLog('info', `  - fileName: ${block.fileName}`);
        addLog('info', `  - startWithAll: ${block.startWithAll} (type: ${typeof block.startWithAll})`);
        addLog('info', `  - inputs: ${block.inputs}, outputs: ${block.outputs}`);
        console.log(`[DEBUG] Block: ${block.name}`, {
          id: block.id,
          startWithAll: block.startWithAll,
          type: typeof block.startWithAll,
          config: block.config,
          fullBlock: block
        });
      });
      
      addLog('info', '========================================');
      addLog('info', 'Starting all blocks with startWithAll=true...');
      
      for (const block of blocks) {
        await window.electronAPI.writeFile(
          `${projectDir}/blocks/${block.fileName}`,
          block.code
        );
      }

      const sortedBlocks = topologicalSort(blocks, connections);
      
      addLog('info', `DEBUG: Topologically sorted ${sortedBlocks.length} blocks`);
      sortedBlocks.forEach((block, idx) => {
        addLog('info', `DEBUG: Sorted order ${idx + 1}: ${block.name} (startWithAll: ${block.startWithAll})`);
      });

      let startedCount = 0;
      let skippedCount = 0;

      for (let i = 0; i < sortedBlocks.length; i++) {
        const block = sortedBlocks[i];
        
        addLog('info', `DEBUG: Processing block ${i + 1}/${sortedBlocks.length}: ${block.name}`);
        addLog('info', `DEBUG: startWithAll value: ${block.startWithAll}, type: ${typeof block.startWithAll}`);
        
        if (!block.startWithAll) {
          addLog('info', `Skipping ${block.name} (startWithAll=${block.startWithAll})`);
          skippedCount++;
          continue;
        }

        try {
          const matlabCmd = buildBlockCommand(block, connections);
          
          addLog('info', `Starting ${block.name}...`);
          addLog('info', `DEBUG: MATLAB command: ${matlabCmd}`);
          
          // FIX: Get platform from the API instead of using process directly
          const platform = await window.electronAPI.getPlatform();
          const envCmd = platform === 'win32' 
            ? `set BLOCK_ID=${block.id} && `
            : `export BLOCK_ID=${block.id} && `;
          
          const fullCommand = `${envCmd}matlab -batch "cd('${projectDir}/blocks'); addpath('${projectDir}/cpp'); ${matlabCmd}"`;
          addLog('info', `DEBUG: Full command: ${fullCommand}`);
          
          const procResult = await window.electronAPI.startProcess(
            fullCommand,
            projectDir,
            block.name
          );

          if (procResult.success) {
            // Don't mark as running yet - wait for BLOCK_READY protocol message
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
          console.error(`[DEBUG] Error starting ${block.name}:`, err);
        }
      }

      addLog('info', '========================================');
      addLog('success', `Block launch complete: ${startedCount} started, ${skippedCount} skipped`);
      addLog('info', '========================================');
    } catch (err) {
      addLog('error', `Block start failed: ${err.message}`);
      console.error('[DEBUG] handleStart error:', err);
      alert('Failed to start blocks:\n' + err.message);
    }
  };

  const handleStop = async (blockProcesses, setBlockMetrics) => {
    addLog('info', 'Stopping all blocks with startWithAll=true...');
    
    const blocksToStop = blocks.filter(b => b.startWithAll);
    
    addLog('info', `DEBUG: Found ${blocksToStop.length} blocks to stop`);
    blocksToStop.forEach(block => {
      addLog('info', `DEBUG: Will stop: ${block.name} (ID: ${block.id})`);
    });
    
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
      } else {
        addLog('warning', `DEBUG: No process info found for ${block.name} (ID: ${block.id})`);
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
      addLog('info', `DEBUG: Manually starting block: ${block.name}`);
      addLog('info', `DEBUG: Block startWithAll: ${block.startWithAll}`);
      
      const sortedBlocks = topologicalSort(blocks, connections);

      await window.electronAPI.writeFile(
        `${projectDir}/blocks/${block.fileName}`,
        block.code
      );

      const matlabCmd = buildBlockCommand(block, connections);

      addLog('info', `Starting ${block.name}...`);
      
      // FIX: Get platform from the API instead of using process directly
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
      console.error('[DEBUG] handleStartBlock error:', err);
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