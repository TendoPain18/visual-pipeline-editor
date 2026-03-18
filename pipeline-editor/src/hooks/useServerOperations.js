import { useRef } from 'react';
import { topologicalSort } from '../utils/blockUtils';

// Module-level registry: blockId (string) -> { resolve, reject, timer }
// Shared across all hook instances via module scope.
const _blockReadyRegistry = new Map();

/**
 * Called by App.jsx whenever a BLOCK_READY socket message arrives.
 * This replaces the old polling-against-stale-state approach.
 */
export const signalBlockReady = (blockId) => {
  const key = String(blockId);
  const entry = _blockReadyRegistry.get(key);
  if (entry) {
    clearTimeout(entry.timer);
    _blockReadyRegistry.delete(key);
    entry.resolve(true);
  }
};

/**
 * Called by App.jsx (or cleanup) to reject a pending waiter.
 */
export const signalBlockError = (blockId, reason) => {
  const key = String(blockId);
  const entry = _blockReadyRegistry.get(key);
  if (entry) {
    clearTimeout(entry.timer);
    _blockReadyRegistry.delete(key);
    entry.reject(new Error(reason || `Block ${blockId} errored`));
  }
};

// -----------------------------------------------------------------------
// Detect whether a C++ block file is a CUDA file (.cu extension)
// -----------------------------------------------------------------------
const isCudaBlock = (block) => {
  return block.fileName && block.fileName.toLowerCase().endsWith('.cu');
};

// -----------------------------------------------------------------------
// Build the compile command for a single C++ / CUDA block.
//
// For CUDA blocks (.cu):
//   nvcc -o bin/<name>.exe <file>.cu -lws2_32 -O3 -arch=sm_86 -std=c++17
//
// For regular C++ blocks (.cpp):
//   g++ -fopenmp -o bin/<name>.exe <file>.cpp -lws2_32 -O3 -march=native -std=c++17
//
// The CUDA architecture (-arch) is set to sm_86 (RTX 3000/A-series) by default.
// Override by setting the CUDA_ARCH environment variable in your shell, e.g.:
//   CUDA_ARCH=sm_89   (RTX 4090)
//   CUDA_ARCH=sm_80   (A100)
//   CUDA_ARCH=sm_75   (RTX 2000)
//   CUDA_ARCH=sm_70   (V100)
// -----------------------------------------------------------------------
const buildCompileCommand = (block, projectDir) => {
  const exeName = block.fileName.replace(/\.(cpp|cu)$/i, '.exe');

  if (isCudaBlock(block)) {
    const cudaArch = 'sm_86';  // Change to match your GPU: sm_75, sm_80, sm_86, sm_89, sm_90

    // nvcc on Windows requires MSVC (cl.exe) — we wrap the call in a
    // VsDevCmd.bat environment initialisation so nvcc can find cl.exe
    // regardless of whether the user launched from a Developer Prompt.
    //
    // Flags:
    //   -O3 / -O2          : optimisation (nvcc passes -O2 to cl.exe via -Xcompiler)
    //   -arch=<sm>         : target GPU SM version
    //   -std=c++17         : C++17
    //   -lws2_32           : Winsock
    //   -Xcompiler /W0     : suppress MSVC warnings (no quotes needed here)
    //   --expt-relaxed-constexpr : allow constexpr in device code
    //   -I.                : so #include "core/run_generic_block.h" resolves

    // Find VsDevCmd.bat — try the two most common VS installation paths.
    // If neither exists nvcc will still attempt to compile but may error on cl.exe.
    const vsDevCmdPaths = [
      `"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\Tools\\VsDevCmd.bat"`,
      `"C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools\\Common7\\Tools\\VsDevCmd.bat"`,
      `"C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\Common7\\Tools\\VsDevCmd.bat"`,
      `"C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\BuildTools\\Common7\\Tools\\VsDevCmd.bat"`,
    ];

    // We emit a cmd chain: try each VsDevCmd in order, fall through to bare nvcc if none found.
    // Simpler approach: just call vcvars64.bat if it exists, otherwise run nvcc directly.
    // The cleanest portable solution is to use `cmd /C "call VsDevCmd.bat 2>nul & nvcc ..."`.
    // We try the most common path and silently ignore if not found (2>nul).
    // Try Professional edition first (detected from user environment), then Community/BuildTools
    const vcvars = `"C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat"`;

    const nvccCmd =
      `nvcc -O3 -arch=${cudaArch} -std=c++17 ` +
      `-o bin/${exeName} ${block.fileName} ` +
      `-I. -I.. ` +
      `-lws2_32 ` +
      `-Xcompiler /W0 ` +
      `-diag-suppress 177 ` +
      `--expt-relaxed-constexpr`;

    // Wrap in cmd so we can chain vcvars64 initialisation before nvcc.
    // `call vcvars64.bat 2>nul` silently fails if the path doesn't exist,
    // then nvcc runs anyway (works if nvcc + cl.exe are already on PATH).
    return `cmd /C "call ${vcvars} 2>nul & ${nvccCmd}"`;
  }

  // Standard C++ block
  return (
    `g++ -fopenmp ` +
    `-o bin/${exeName} ${block.fileName} ` +
    `-lws2_32 -O3 -march=native -std=c++17`
  );
};

export const useServerOperations = ({
  blocks,
  connections,
  projectDir,
  instanceConfig,
  setServerRunning,
  setBlockProcesses,
  setIsStarting,
  addLog,
  setBlockMetrics
}) => {

  // Cancellation flag
  const startupCancelledRef = useRef(false);
  // Track launched processes during startup for cleanup
  const launchedDuringStartupRef = useRef([]);

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
      // Support both .cpp and .cu extensions — exe name strips either
      const exeName = block.fileName.replace(/\.(cpp|cu)$/i, '.exe');
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

  /**
   * Returns a Promise that resolves when signalBlockReady(blockId) is called
   * from the socket message handler in App.jsx.
   */
  const waitForBlockReady = (blockId, blockName, timeout = 30000) => {
    return new Promise((resolve, reject) => {
      const key = String(blockId);

      const existing = _blockReadyRegistry.get(key);
      if (existing) {
        clearTimeout(existing.timer);
        _blockReadyRegistry.delete(key);
      }

      const timer = setTimeout(() => {
        _blockReadyRegistry.delete(key);
        reject(new Error(`Timeout waiting for ${blockName}`));
      }, timeout);

      _blockReadyRegistry.set(key, { resolve, reject, timer });
    });
  };

  // ─── Combined Start (Server + All Blocks) ─────────────────────────────────
  const handleStartAll = async () => {
    if (!projectDir) { alert('Loading project directory...'); return; }
    if (!instanceConfig) { alert('Instance configuration not loaded...'); return; }

    startupCancelledRef.current = false;
    launchedDuringStartupRef.current = [];

    setIsStarting(true);

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

      if (startupCancelledRef.current) { addLog('warning', 'Startup cancelled by user'); return; }

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

      if (startupCancelledRef.current) { addLog('warning', 'Startup cancelled by user'); return; }

      // ── Compile C++ / CUDA blocks ──────────────────────────────────────────
      const cppBlocks = blocks.filter(b => b.language === 'cpp');
      if (cppBlocks.length > 0) {
        addLog('info', `Found ${cppBlocks.length} C++/CUDA block(s) to compile...`);
        for (const block of cppBlocks) {
          if (startupCancelledRef.current) { addLog('warning', 'Startup cancelled by user'); return; }

          const isCuda = isCudaBlock(block);
          const exeName = block.fileName.replace(/\.(cpp|cu)$/i, '.exe');
          const exePath = `${projectDir}/cpp_blocks/bin/${exeName}`;
          const compiler = isCuda ? 'nvcc' : 'g++';
          const label    = isCuda ? 'CUDA' : 'C++';

          addLog('info', `Checking ${block.name} [${label}] | startWithAll=${block.startWithAll}`);

          try {
            await window.electronAPI.readFile(exePath);
            addLog('success', `Found existing ${exeName} in bin/ - skipping`);
          } catch (e) {
            addLog('info', `Compiling ${block.fileName} [${label}] to bin/ using ${compiler}...`);

            // Write the source file to disk first
            await window.electronAPI.writeFile(
              `${projectDir}/cpp_blocks/${block.fileName}`,
              block.code
            );

            // For CUDA blocks: run a quick diagnostic before attempting full compile
            if (isCudaBlock(block)) {
              const nvccCheck = await window.electronAPI.execCommand('nvcc --version', projectDir);
              if (!nvccCheck.success) {
                throw new Error(
                  `nvcc not found on PATH.\n\n` +
                  `Please install the CUDA Toolkit from https://developer.nvidia.com/cuda-downloads\n` +
                  `and make sure nvcc.exe is on your system PATH.\n\n` +
                  `Typical location: C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\vX.X\\bin\\`
                );
              }
              addLog('info', `nvcc found: ${(nvccCheck.stdout || '').split('\n')[0].trim()}`);

              // Check if cl.exe (MSVC) is accessible
              const clCheck = await window.electronAPI.execCommand('cl.exe 2>&1 | findstr /i "version"', projectDir);
              if (!clCheck.success && !clCheck.stdout) {
                addLog('warning',
                  `cl.exe (MSVC) not found on PATH — nvcc will try to initialise ` +
                  `Visual Studio automatically via vcvars64.bat.\n` +
                  `If compilation fails, open a "Developer Command Prompt for VS 2022" and restart the app.`
                );
              } else {
                addLog('info', `cl.exe found — MSVC available for nvcc`);
              }
            }

            const compileCmd = buildCompileCommand(block, projectDir);
            addLog('info', `Compile command: ${compileCmd}`);

            const r = await window.electronAPI.execCommand(
              compileCmd,
              `${projectDir}/cpp_blocks`
            );

            if (!r.success) {
              // Log the full stderr so the real compiler error is visible in the UI
              if (r.stderr) {
                addLog('error', `${label} compiler output:\n${r.stderr}`);
              }
              if (r.stdout) {
                addLog('error', `${label} compiler stdout:\n${r.stdout}`);
              }
              throw new Error(
                `${label} compilation failed for ${block.fileName}:\n${r.stderr || r.error}`
              );
            }
            addLog('success', `Compiled ${block.fileName} → bin/${exeName}`);
          }
        }
      }

      if (startupCancelledRef.current) { addLog('warning', 'Startup cancelled by user'); return; }

      try {
        await window.electronAPI.readFile(`${projectDir}/server/pipe_server.exe`);
        addLog('success', 'Found existing pipe server - skipping compilation');
      } catch (e) {
        try { await window.electronAPI.readFile(`${projectDir}/server/pipe_server.cpp`); }
        catch (e2) { throw new Error('pipe_server.cpp not found in server/ folder.'); }
        addLog('info', 'Compiling pipe server...');
        const r = await window.electronAPI.execCommand(
          'g++ -o pipe_server.exe pipe_server.cpp -lws2_32 -O2',
          `${projectDir}/server`
        );
        if (!r.success) throw new Error(`Pipe server compilation failed:\n${r.stderr || r.error}`);
        addLog('success', 'Pipe server compiled');
      }

      if (startupCancelledRef.current) { addLog('warning', 'Startup cancelled by user'); return; }

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
      const serverProc = await window.electronAPI.startServerWithSocket(
        serverCmd, `${projectDir}/server`, 'server'
      );
      if (!serverProc.success) throw new Error('Failed to start server: ' + serverProc.error);

      launchedDuringStartupRef.current.push({ type: 'server', pid: serverProc.pid });

      setServerRunning(true);
      setBlockProcesses(prev => ({ ...prev, server: { pid: serverProc.pid, status: 'running', name: 'server' } }));
      addLog('success', `Pipe server started (PID: ${serverProc.pid}, Port: ${serverPort})`);
      addLog('info', 'Waiting for socket connection...');

      if (startupCancelledRef.current) {
        addLog('warning', 'Startup cancelled - cleaning up launched processes');
        await cleanupLaunchedProcesses();
        return;
      }

      await new Promise(resolve => setTimeout(resolve, 2000));

      if (startupCancelledRef.current) {
        addLog('warning', 'Startup cancelled - cleaning up launched processes');
        await cleanupLaunchedProcesses();
        return;
      }

      addLog('info', '=== STARTING BLOCK SEQUENCE ===');

      for (const block of blocks) {
        if (block.language === 'matlab') {
          await window.electronAPI.writeFile(`${projectDir}/matlab_blocks/${block.fileName}`, block.code);
        } else if (block.language === 'cpp') {
          await window.electronAPI.writeFile(`${projectDir}/cpp_blocks/${block.fileName}`, block.code);
        }
      }

      const blocksToStart = sortedBlocks.filter(b => b.startWithAll);
      addLog('info', `Blocks to start: ${blocksToStart.map(b => b.name).join(', ')}`);

      let startedCount = 0;
      let skippedCount = 0;

      for (let i = 0; i < sortedBlocks.length; i++) {
        if (startupCancelledRef.current) {
          addLog('warning', 'Block startup sequence cancelled - cleaning up');
          await cleanupLaunchedProcesses();
          break;
        }

        const block = sortedBlocks[i];
        addLog('info', `Block ${i + 1}/${sortedBlocks.length}: ${block.name} (startWithAll=${block.startWithAll})`);

        if (!block.startWithAll) {
          addLog('info', `Skipping ${block.name}`);
          skippedCount++;
          continue;
        }

        try {
          const languagePort = getBlockLanguagePort(block, instanceConfig);
          const label = isCudaBlock(block) ? 'CUDA' : (block.language?.toUpperCase() || 'C++');
          addLog('info', `[${startedCount + 1}/${blocksToStart.length}] Starting ${block.name} [${label}] → port ${languagePort}`);

          const parsed = buildBlockArgs(block, connections);
          let fullCommand;

          if (parsed.type === 'cpp') {
            const { exeName, args } = parsed;
            const exePath = `${projectDir}\\cpp_blocks\\bin\\${exeName}`;
            const argsString = args.join(' ');
            fullCommand = `cmd /C "set BLOCK_ID=${block.id}&& set INSTANCE_ID=${instanceConfig.instanceId}&& set CPP_PORT=${languagePort}&& "${exePath}" ${argsString}"`;
            addLog('info', `Direct launch: "${exePath}" ${argsString}`);
          } else {
            const { functionCall } = parsed;
            const matlabCommand = `cd('${projectDir}/matlab_blocks'); addpath('${projectDir}/server'); addpath('${projectDir}/matlab_blocks/core'); ${functionCall}`;

            if (platform === 'win32') {
              fullCommand = `cmd /C "set BLOCK_ID=${block.id}&& set INSTANCE_ID=${instanceConfig.instanceId}&& set MATLAB_PORT=${languagePort}&& matlab -batch \\"${matlabCommand}\\""`;
            } else {
              fullCommand = `BLOCK_ID=${block.id} INSTANCE_ID=${instanceConfig.instanceId} MATLAB_PORT=${languagePort} matlab -batch "${matlabCommand}"`;
            }
            addLog('info', `MATLAB direct launch: ${block.name}`);
          }

          const procResult = await window.electronAPI.startProcess(fullCommand, projectDir, block.name);

          if (procResult.success) {
            launchedDuringStartupRef.current.push({
              type: 'block', pid: procResult.pid, blockId: block.id, name: block.name
            });

            setBlockProcesses(prev => ({
              ...prev,
              [block.id]: {
                pid: procResult.pid,
                status: 'starting',
                name: block.name,
                language: block.language || 'matlab'
              }
            }));

            addLog('info', `${block.name} launched (PID: ${procResult.pid}), waiting for BLOCK_READY...`);

            if (startupCancelledRef.current) {
              addLog('warning', 'Startup cancelled - cleaning up launched processes');
              await cleanupLaunchedProcesses();
              break;
            }

            try {
              await waitForBlockReady(block.id, block.name);

              if (startupCancelledRef.current) {
                addLog('warning', 'Startup cancelled during block initialization - cleaning up');
                await cleanupLaunchedProcesses();
                break;
              }

              addLog('success', `✓ ${block.name} is READY`);
              startedCount++;
            } catch (err) {
              if (err.message === 'Startup cancelled') {
                addLog('warning', 'Block startup cancelled - cleaning up');
                await cleanupLaunchedProcesses();
                break;
              }
              addLog('error', `✗ ${block.name} failed: ${err.message}`);
              addLog('warning', 'Continuing with next block...');
            }
          } else {
            addLog('error', `Failed to launch ${block.name}: ${procResult.error}`);
          }
        } catch (err) {
          if (startupCancelledRef.current) {
            addLog('warning', 'Startup cancelled - cleaning up');
            await cleanupLaunchedProcesses();
            break;
          }
          addLog('error', `Error starting ${block.name}: ${err.message}`);
        }
      }

      if (!startupCancelledRef.current) {
        addLog('info', '========================================');
        addLog('success', `Startup complete: ${startedCount} started, ${skippedCount} skipped`);
        addLog('info', '========================================');
      } else {
        addLog('warning', 'Startup was cancelled - all processes cleaned up');
      }

    } catch (err) {
      addLog('error', `Startup failed: ${err.message}`);
      alert('Failed to start system:\n' + err.message);
      if (launchedDuringStartupRef.current.length > 0) {
        await cleanupLaunchedProcesses();
      }
    } finally {
      setIsStarting(false);
      startupCancelledRef.current = false;
      launchedDuringStartupRef.current = [];
    }
  };

  // ─── Cleanup launched processes during cancelled startup ──────────────────
  const cleanupLaunchedProcesses = async () => {
    const launched = launchedDuringStartupRef.current;
    if (launched.length === 0) return;

    addLog('info', `Cleaning up ${launched.length} launched process(es)...`);

    for (const proc of launched) {
      try {
        await window.electronAPI.killProcess(proc.pid);
        addLog('info', `Killed ${proc.type}: ${proc.name || 'server'} (PID: ${proc.pid})`);
      } catch (err) {
        addLog('warning', `Failed to kill PID ${proc.pid}: ${err.message}`);
      }
    }

    setBlockProcesses({});
    setServerRunning(false);
    setBlockMetrics({});
    launchedDuringStartupRef.current = [];
  };

  // ─── Combined Stop All ────────────────────────────────────────────────────
  const handleStopAll = async () => {
    startupCancelledRef.current = true;

    for (const [key, entry] of _blockReadyRegistry.entries()) {
      clearTimeout(entry.timer);
      entry.reject(new Error('Startup cancelled'));
    }
    _blockReadyRegistry.clear();

    addLog('info', 'Stopping all processes...');
    setIsStarting(false);

    const result = await window.electronAPI.killAllProcesses();
    if (result.success) {
      addLog('success', `Terminated ${result.killedCount} process(es)`);
      setBlockProcesses({});
      setServerRunning(false);
      setBlockMetrics({});
      launchedDuringStartupRef.current = [];
    } else {
      addLog('error', 'Failed to terminate all processes');
    }
  };

  // ─── Individual Block Start ───────────────────────────────────────────────
  const handleStartBlock = async (block, blockProcesses) => {
    if (!instanceConfig) { addLog('error', 'Instance configuration not loaded'); return; }
    if (blockProcesses[block.id]) { addLog('warning', `${block.name} is already running`); return; }

    try {
      const platform = await window.electronAPI.getPlatform();
      const languagePort = getBlockLanguagePort(block, instanceConfig);
      const label = isCudaBlock(block) ? 'CUDA' : (block.language?.toUpperCase() || 'C++');
      addLog('info', `Manually starting ${block.name} [${label}] → port ${languagePort}`);

      if (block.language === 'matlab') {
        await window.electronAPI.writeFile(`${projectDir}/matlab_blocks/${block.fileName}`, block.code);
      } else if (block.language === 'cpp') {
        await window.electronAPI.writeFile(`${projectDir}/cpp_blocks/${block.fileName}`, block.code);
      }

      const parsed = buildBlockArgs(block, connections);
      let fullCommand;

      if (parsed.type === 'cpp') {
        const { exeName, args } = parsed;
        const exePath = `${projectDir}\\cpp_blocks\\bin\\${exeName}`;
        const argsString = args.join(' ');
        fullCommand = `cmd /C "set BLOCK_ID=${block.id}&& set INSTANCE_ID=${instanceConfig.instanceId}&& set CPP_PORT=${languagePort}&& "${exePath}" ${argsString}"`;
      } else {
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
          [block.id]: {
            pid: procResult.pid,
            status: 'starting',
            name: block.name,
            language: block.language || 'matlab'
          }
        }));
        addLog('info', `${block.name} started (PID: ${procResult.pid})`);
      } else {
        throw new Error(procResult.error);
      }
    } catch (err) {
      addLog('error', `Failed to start ${block.name}: ${err.message}`);
    }
  };

  // ─── Individual Block Stop ────────────────────────────────────────────────
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
    handleStartAll,
    handleStopAll,
    handleStartBlock,
    handleStopBlock
  };
};