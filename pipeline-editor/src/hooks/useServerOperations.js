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

  const getBlockLanguagePort = (block, instanceConfig) => {
    if (block.language === 'cpp') return instanceConfig.cppPort;
    if (block.language === 'matlab') return instanceConfig.matlabPort;
    return instanceConfig.matlabPort;
  };

  const buildBlockArgs = (block, connections) => {
    const shortPipes = connections.map((_, i) => `P${i + 1}`);

    if (block.language === 'matlab') {
      const functionName = block.fileName.replace('.m', '');
      const inputConns = connections.filter(c => c.toBlock === block.id).sort((a, b) => a.toPort - b.toPort);
      const outputConns = connections.filter(c => c.fromBlock === block.id).sort((a, b) => a.fromPort - b.fromPort);
      const args = [];
      for (let i = 0; i < block.inputs; i++) {
        const conn = inputConns.find(c => c.toPort === i);
        if (conn) args.push(`'${shortPipes[connections.indexOf(conn)]}'`);
        else throw new Error(`Block ${block.name} input ${i} is not connected`);
      }
      for (let i = 0; i < block.outputs; i++) {
        const conn = outputConns.find(c => c.fromPort === i);
        if (conn) args.push(`'${shortPipes[connections.indexOf(conn)]}'`);
        else throw new Error(`Block ${block.name} output ${i} is not connected`);
      }
      return { type: 'matlab', functionCall: `${functionName}(${args.join(', ')})` };
    }

    if (block.language === 'cpp') {
      const exeName = block.fileName.replace('.cpp', '.exe');
      const inputConns = connections.filter(c => c.toBlock === block.id).sort((a, b) => a.toPort - b.toPort);
      const outputConns = connections.filter(c => c.fromBlock === block.id).sort((a, b) => a.fromPort - b.fromPort);
      const args = [];
      for (let i = 0; i < block.inputs; i++) {
        const conn = inputConns.find(c => c.toPort === i);
        if (conn) args.push(shortPipes[connections.indexOf(conn)]);
        else throw new Error(`Block ${block.name} input ${i} is not connected`);
      }
      for (let i = 0; i < block.outputs; i++) {
        const conn = outputConns.find(c => c.fromPort === i);
        if (conn) args.push(shortPipes[connections.indexOf(conn)]);
        else throw new Error(`Block ${block.name} output ${i} is not connected`);
      }
      return { type: 'cpp', exeName, args };
    }

    throw new Error(`Unsupported block language: ${block.language}`);
  };

  // ─── Start Server ──────────────────────────────────────────────────────────
  const handleStartServer = async () => {
    if (!projectDir) { alert('Loading project directory...'); return; }
    if (!instanceConfig) { alert('Instance configuration not loaded...'); return; }

    setIsStartingServer(true);
    try {
      addLog('info', `Project directory: ${projectDir}`);
      addLog('info', `Instance config: ${JSON.stringify(instanceConfig)}`);
      addLog('info', 'Validating pipeline...');
      if (blocks.length < 2) throw new Error('Need at least 2 blocks');
      if (connections.length === 0) throw new Error('No connections found');

      await window.electronAPI.ensureDir(`${projectDir}/server`);
      await window.electronAPI.ensureDir(`${projectDir}/matlab_blocks`);
      await window.electronAPI.ensureDir(`${projectDir}/matlab_blocks/core`);
      await window.electronAPI.ensureDir(`${projectDir}/cpp_blocks`);
      await window.electronAPI.ensureDir(`${projectDir}/cpp_blocks/bin`);

      const platform = await window.electronAPI.getPlatform();
      const mexExt = platform === 'win32' ? 'mexw64' : (platform === 'darwin' ? 'mexmaci64' : 'mexa64');
      try {
        await window.electronAPI.readFile(`${projectDir}/server/pipeline_mex.${mexExt}`);
        addLog('success', 'Found existing MEX file - skipping compilation');
      } catch (e) {
        try { await window.electronAPI.readFile(`${projectDir}/server/pipeline_mex.cpp`); }
        catch (e2) { throw new Error('pipeline_mex.cpp not found in server/ folder.'); }
        addLog('info', 'Compiling MEX file...');
        const r = await window.electronAPI.execCommand('matlab -batch "mex pipeline_mex.cpp"', `${projectDir}/server`);
        if (!r.success) throw new Error(`MEX compilation failed:\n${r.stderr || r.error}`);
        addLog('success', 'MEX compiled successfully');
      }

      const cppBlocks = blocks.filter(b => b.language === 'cpp');
      if (cppBlocks.length > 0) {
        addLog('info', `Found ${cppBlocks.length} C++ block(s) to compile...`);
        for (const block of cppBlocks) {
          addLog('info', `Checking ${block.name} | startWithAll=${block.startWithAll}`);
          const exeName = block.fileName.replace('.cpp', '.exe');
          const exePath = `${projectDir}/cpp_blocks/bin/${exeName}`;
          
          try {
            await window.electronAPI.readFile(exePath);
            addLog('success', `Found existing ${exeName} in bin/ - skipping`);
          } catch (e) {
            addLog('info', `Compiling ${block.fileName} to bin/...`);
            await window.electronAPI.writeFile(`${projectDir}/cpp_blocks/${block.fileName}`, block.code);
            const r = await window.electronAPI.execCommand(
              `g++ -o bin/${exeName} ${block.fileName} -lws2_32 -O2 -std=c++17`,
              `${projectDir}/cpp_blocks`
            );
            if (!r.success) throw new Error(`C++ compilation failed for ${block.fileName}:\n${r.stderr || r.error}`);
            addLog('success', `Compiled ${block.fileName} → bin/${exeName}`);
          }
        }
      }

      try {
        await window.electronAPI.readFile(`${projectDir}/server/pipe_server.exe`);
        addLog('success', 'Found existing pipe server - skipping compilation');
      } catch (e) {
        try { await window.electronAPI.readFile(`${projectDir}/server/pipe_server.cpp`); }
        catch (e2) { throw new Error('pipe_server.cpp not found in server/ folder.'); }
        addLog('info', 'Compiling pipe server...');
        const r = await window.electronAPI.execCommand('g++ -o pipe_server.exe pipe_server.cpp -lws2_32 -O2', `${projectDir}/server`);
        if (!r.success) throw new Error(`Pipe server compilation failed:\n${r.stderr || r.error}`);
        addLog('success', 'Pipe server compiled');
      }

      const sortedBlocks = topologicalSort(blocks, connections);
      const { instanceId, serverPort } = instanceConfig;
      const instancePipes = connections.map((_, i) => `Instance_${instanceId}_P${i + 1}`);

      let serverCmd = `pipe_server.exe ${connections.length} ${serverPort}`;
      connections.forEach((conn, i) => {
        const fromBlock = sortedBlocks.find(b => b.id === conn.fromBlock);
        let size = 67108864;
        if (fromBlock?.outputBufferSizes?.[conn.fromPort]) {
          size = fromBlock.outputBufferSizes[conn.fromPort];
          addLog('info', `Pipe ${i}: ${fromBlock.name}[${conn.fromPort}] ${fromBlock.outputPacketSizes[conn.fromPort]}×${fromBlock.outputBatchSizes[conn.fromPort]} = ${(size / 1024).toFixed(1)}KB`);
        }
        serverCmd += ` ${instancePipes[i]} ${size}`;
      });

      addLog('info', `Starting pipe server: ${serverCmd}`);
      const serverProc = await window.electronAPI.startServerWithSocket(serverCmd, `${projectDir}/server`, 'server');
      if (!serverProc.success) throw new Error('Failed to start server: ' + serverProc.error);

      setServerRunning(true);
      setBlockProcesses(prev => ({ ...prev, server: { pid: serverProc.pid, status: 'running', name: 'server' } }));
      addLog('success', `Pipe server started (PID: ${serverProc.pid}, Port: ${serverPort})`);
      addLog('info', 'Waiting for socket connection...');

    } catch (err) {
      addLog('error', `Server start failed: ${err.message}`);
      alert('Failed to start server:\n' + err.message);
    } finally {
      setIsStartingServer(false);
    }
  };

  const waitForBlockReady = (blockId, blockName, setBlockProcesses, timeout = 30000) => {
    return new Promise((resolve, reject) => {
      const startTime = Date.now();
      let resolved = false;
      const iv = setInterval(() => {
        setBlockProcesses(prev => {
          const proc = prev[blockId];
          if (proc?.status === 'running' && !resolved) {
            resolved = true; clearInterval(iv); resolve(true);
          } else if (Date.now() - startTime > timeout && !resolved) {
            resolved = true; clearInterval(iv);
            reject(new Error(`Timeout waiting for ${blockName}`));
          }
          return prev;
        });
      }, 100);
    });
  };

  // ─── Start all auto-start blocks ──────────────────────────────────────────
  const handleStart = async () => {
    if (!projectDir) { alert('Loading project directory...'); return; }
    if (!instanceConfig) { alert('Instance configuration not loaded...'); return; }

    try {
      addLog('info', '=== STARTING BLOCK SEQUENCE ===');

      for (const block of blocks) {
        if (block.language === 'matlab') await window.electronAPI.writeFile(`${projectDir}/matlab_blocks/${block.fileName}`, block.code);
        else if (block.language === 'cpp') await window.electronAPI.writeFile(`${projectDir}/cpp_blocks/${block.fileName}`, block.code);
      }

      const sortedBlocks = topologicalSort(blocks, connections);
      const blocksToStart = sortedBlocks.filter(b => b.startWithAll);
      addLog('info', `Blocks to start: ${blocksToStart.map(b => b.name).join(', ')}`);

      const platform = await window.electronAPI.getPlatform();

      let startedCount = 0;
      let skippedCount = 0;

      for (let i = 0; i < sortedBlocks.length; i++) {
        const block = sortedBlocks[i];
        addLog('info', `Block ${i + 1}/${sortedBlocks.length}: ${block.name} (startWithAll=${block.startWithAll})`);

        if (!block.startWithAll) {
          addLog('info', `Skipping ${block.name}`);
          skippedCount++;
          continue;
        }

        try {
          const languagePort = getBlockLanguagePort(block, instanceConfig);
          addLog('info', `[${startedCount + 1}/${blocksToStart.length}] Starting ${block.name} [${block.language?.toUpperCase()}] → port ${languagePort}`);

          const parsed = buildBlockArgs(block, connections);
          let fullCommand;

          if (parsed.type === 'cpp') {
            const { exeName, args } = parsed;
            const exePath = `${projectDir}\\cpp_blocks\\bin\\${exeName}`;
            const argsString = args.join(' ');
            fullCommand = `cmd /C "set BLOCK_ID=${block.id}&& set INSTANCE_ID=${instanceConfig.instanceId}&& set CPP_PORT=${languagePort}&& "${exePath}" ${argsString}"`;
            addLog('info', `Direct launch: "${exePath}" ${argsString}`);
          } else {
            // MATLAB - Launch directly without bat file
            const { functionCall } = parsed;
            const matlabCommand = `cd('${projectDir}/matlab_blocks'); addpath('${projectDir}/server'); addpath('${projectDir}/matlab_blocks/core'); ${functionCall}`;
            
            if (platform === 'win32') {
              // Windows: Use cmd to set environment variables and launch MATLAB
              fullCommand = `cmd /C "set BLOCK_ID=${block.id}&& set INSTANCE_ID=${instanceConfig.instanceId}&& set MATLAB_PORT=${languagePort}&& matlab -batch \\"${matlabCommand}\\""`;
            } else {
              // Linux/Mac: Use env to set environment variables
              fullCommand = `BLOCK_ID=${block.id} INSTANCE_ID=${instanceConfig.instanceId} MATLAB_PORT=${languagePort} matlab -batch "${matlabCommand}"`;
            }
            addLog('info', `MATLAB direct launch: ${block.name}`);
          }

          const procResult = await window.electronAPI.startProcess(fullCommand, projectDir, block.name);
          if (procResult.success) {
            setBlockProcesses(prev => ({
              ...prev,
              [block.id]: { pid: procResult.pid, status: 'starting', name: block.name, language: block.language || 'matlab' }
            }));
            addLog('info', `${block.name} launched (PID: ${procResult.pid}), waiting for BLOCK_READY...`);
            try {
              await waitForBlockReady(block.id, block.name, setBlockProcesses);
              addLog('success', `✓ ${block.name} is READY`);
              startedCount++;
            } catch (err) {
              addLog('error', `✗ ${block.name} failed: ${err.message}`);
              addLog('warning', 'Continuing with next block...');
            }
          } else {
            addLog('error', `Failed to launch ${block.name}: ${procResult.error}`);
          }
        } catch (err) {
          addLog('error', `Error starting ${block.name}: ${err.message}`);
        }
      }

      addLog('info', '========================================');
      addLog('success', `Startup complete: ${startedCount} started, ${skippedCount} skipped`);
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
      const info = blockProcesses[block.id];
      if (info) {
        const r = await window.electronAPI.killProcess(info.pid);
        if (r.success) addLog('success', `${block.name} stopped`);
        else addLog('error', `Failed to stop ${block.name}: ${r.error}`);
      }
    }
    setBlockProcesses(prev => { const n = { ...prev }; blocksToStop.forEach(b => delete n[b.id]); return n; });
    setBlockMetrics(prev => { const n = { ...prev }; blocksToStop.forEach(b => delete n[b.id]); return n; });
    addLog('success', 'All auto-start blocks stopped');
  };

  const handleTerminate = async (setBlockMetrics) => {
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
    if (!instanceConfig) { addLog('error', 'Instance configuration not loaded'); return; }
    if (blockProcesses[block.id]) { addLog('warning', `${block.name} is already running`); return; }

    try {
      const platform = await window.electronAPI.getPlatform();
      const languagePort = getBlockLanguagePort(block, instanceConfig);
      addLog('info', `Manually starting ${block.name} [${block.language?.toUpperCase()}] → port ${languagePort}`);

      if (block.language === 'matlab') await window.electronAPI.writeFile(`${projectDir}/matlab_blocks/${block.fileName}`, block.code);
      else if (block.language === 'cpp') await window.electronAPI.writeFile(`${projectDir}/cpp_blocks/${block.fileName}`, block.code);

      const parsed = buildBlockArgs(block, connections);
      let fullCommand;

      if (parsed.type === 'cpp') {
        const { exeName, args } = parsed;
        const exePath = `${projectDir}\\cpp_blocks\\bin\\${exeName}`;
        const argsString = args.join(' ');
        fullCommand = `cmd /C "set BLOCK_ID=${block.id}&& set INSTANCE_ID=${instanceConfig.instanceId}&& set CPP_PORT=${languagePort}&& "${exePath}" ${argsString}"`;
      } else {
        // MATLAB - Launch directly without bat file
        const { functionCall } = parsed;
        const matlabCommand = `cd('${projectDir}/matlab_blocks'); addpath('${projectDir}/server'); addpath('${projectDir}/matlab_blocks/core'); ${functionCall}`;
        
        if (platform === 'win32') {
          fullCommand = `cmd /C "set BLOCK_ID=${block.id}&& set INSTANCE_ID=${instanceConfig.instanceId}&& set MATLAB_PORT=${languagePort}&& matlab -batch \\"${matlabCommand}\\""`;
        } else {
          fullCommand = `BLOCK_ID=${block.id} INSTANCE_ID=${instanceConfig.instanceId} MATLAB_PORT=${languagePort} matlab -batch "${matlabCommand}"`;
        }
      }

      addLog('info', `Launching ${block.name}...`);
      const procResult = await window.electronAPI.startProcess(fullCommand, projectDir, block.name);
      if (procResult.success) {
        setBlockProcesses(prev => ({
          ...prev,
          [block.id]: { pid: procResult.pid, status: 'starting', name: block.name, language: block.language || 'matlab' }
        }));
        addLog('info', `${block.name} started (PID: ${procResult.pid})`);
      } else {
        throw new Error(procResult.error);
      }
    } catch (err) {
      addLog('error', `Failed to start ${block.name}: ${err.message}`);
    }
  };

  const handleStopBlock = async (block, blockProcesses, setBlockMetrics) => {
    const info = blockProcesses[block.id];
    if (info) {
      const r = await window.electronAPI.killProcess(info.pid);
      if (r.success) {
        addLog('success', `${block.name} stopped`);
        setBlockProcesses(prev => { const n = { ...prev }; delete n[block.id]; return n; });
        setBlockMetrics(prev => { const n = { ...prev }; delete n[block.id]; return n; });
      } else {
        addLog('error', `Failed to stop ${block.name}: ${r.error}`);
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