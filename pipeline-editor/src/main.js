import { app, BrowserWindow, ipcMain, dialog, screen } from 'electron';
import path from 'node:path';
import fs from 'fs/promises';
import { exec, spawn } from 'child_process';
import { promisify } from 'util';
import started from 'electron-squirrel-startup';
import net from 'net';

const execAsync = promisify(exec);
const runningProcesses = new Map();
const shellToProcessPid = new Map();
const blockIdToProcessPid = new Map();

// Language-specific socket servers
let serverSocket = null;
let serverSocketConnected = false;
let matlabSocketServer = null;
let cppSocketServer = null;
const matlabClients = new Map();
const cppClients = new Map();

let instanceId = null;
let serverPort = null;
let matlabPort = null;
let cppPort = null;

// Safe block ID counter (avoids overflow)
let blockIdCounter = 1000;

const generateInstanceId = () => `${Date.now()}_${Math.floor(Math.random() * 10000)}`;

const findAvailablePort = (basePort) => {
  return new Promise((resolve, reject) => {
    const testServer = net.createServer();
    testServer.once('error', (err) => {
      if (err.code === 'EADDRINUSE') resolve(findAvailablePort(basePort + 1));
      else reject(err);
    });
    testServer.once('listening', () => {
      const port = testServer.address().port;
      testServer.close(() => resolve(port));
    });
    testServer.listen(basePort);
  });
};

if (started) app.quit();

let mainWindow;

// ============================================================
// PIPE SERVER PID COMMUNICATION
// ============================================================

const sendToServer = (messageObj) => {
  if (!serverSocket || !serverSocketConnected) return;
  try {
    serverSocket.write(JSON.stringify(messageObj) + '\n');
  } catch (err) {
    console.error('[PID Registry] Failed to send to pipe server:', err.message);
  }
};

const registerPidWithServer = (realPid, blockName) => {
  console.log(`[PID Registry] ✓ Registering PID ${realPid} (${blockName})`);
  sendToServer({ type: 'REGISTER_PID', pid: realPid, name: blockName });
};

const unregisterPidWithServer = (realPid, blockName) => {
  console.log(`[PID Registry] Unregistering PID ${realPid} (${blockName})`);
  sendToServer({ type: 'UNREGISTER_PID', pid: realPid, name: blockName });
};

// ============================================================
// LANGUAGE-AGNOSTIC BLOCK INIT HANDLER
// ============================================================

const onBlockInit = (blockId, blockName, processPid, language) => {
  if (!processPid || processPid <= 0) return;

  console.log(`[PID Registry] ${language} block "${blockName}" (ID:${blockId}) self-reported PID: ${processPid}`);

  blockIdToProcessPid.set(String(blockId), processPid);

  for (const [shellPid, entry] of runningProcesses.entries()) {
    if (entry.name === blockName && !entry.realProcessPid) {
      entry.realProcessPid = processPid;
      entry.language = language;
      shellToProcessPid.set(shellPid, processPid);
      console.log(`[PID Registry] Linked shell PID ${shellPid} -> ${language} PID ${processPid} (${blockName})`);
      break;
    }
  }

  registerPidWithServer(processPid, blockName);
};

const onBlockStopped = (blockId, blockName) => {
  const processPid = blockIdToProcessPid.get(String(blockId));
  if (processPid) {
    unregisterPidWithServer(processPid, blockName);
    blockIdToProcessPid.delete(String(blockId));
  }
};

// ============================================================
// WINDOW CREATION
// ============================================================

const createWindow = async () => {
  console.log('\n╔════════════════════════════════════════╗');
  console.log('║   ELECTRON MAIN PROCESS STARTING      ║');
  console.log('╚════════════════════════════════════════╝\n');

  instanceId = generateInstanceId();
  serverPort = await findAvailablePort(9000);
  matlabPort = await findAvailablePort(9001);
  cppPort = await findAvailablePort(9002);

  console.log('========================================');
  console.log('INSTANCE CONFIGURATION');
  console.log('========================================');
  console.log(`Instance ID:  ${instanceId}`);
  console.log(`Server Port:  ${serverPort}`);
  console.log(`MATLAB Port:  ${matlabPort}`);
  console.log(`C++ Port:     ${cppPort}`);
  console.log('========================================\n');

  const primaryDisplay = screen.getPrimaryDisplay();
  const { width, height } = primaryDisplay.workAreaSize;
  const windowWidth = Math.min(1400, Math.floor(width * 0.9));
  const windowHeight = Math.min(900, Math.floor(height * 0.9));

  console.log('Creating browser window...');
  mainWindow = new BrowserWindow({
    width: windowWidth,
    height: windowHeight,
    x: primaryDisplay.bounds.x + Math.floor((width - windowWidth) / 2),
    y: primaryDisplay.bounds.y + Math.floor((height - windowHeight) / 2),
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      nodeIntegration: false,
      contextIsolation: true,
    },
  });

  if (MAIN_WINDOW_VITE_DEV_SERVER_URL) {
    mainWindow.loadURL(MAIN_WINDOW_VITE_DEV_SERVER_URL);
    console.log('✓ Loaded development URL');
  } else {
    mainWindow.loadFile(path.join(__dirname, `../renderer/${MAIN_WINDOW_VITE_NAME}/index.html`));
    console.log('✓ Loaded production HTML');
  }

  mainWindow.webContents.openDevTools();
  console.log('✓ DevTools opened\n');

  mainWindow.on('close', async (e) => {
    console.log('\n[CLEANUP] Window closing...');
    e.preventDefault();
    await cleanupAllProcesses();
    if (serverSocket) { serverSocket.destroy(); serverSocket = null; }
    stopLanguageSocketServers();
    mainWindow.destroy();
    console.log('[CLEANUP] Complete\n');
  });

  // ============================================================
  // START LANGUAGE SOCKET SERVERS IMMEDIATELY
  // ============================================================
  console.log('╔════════════════════════════════════════╗');
  console.log('║   STARTING SOCKET SERVERS NOW         ║');
  console.log('╚════════════════════════════════════════╝\n');
  
  startLanguageSocketServers();
  
  console.log('╔════════════════════════════════════════╗');
  console.log('║   SOCKET SERVERS INITIALIZATION DONE  ║');
  console.log('╚════════════════════════════════════════╝\n');
};

const killProcessTree = (pid) => {
  return new Promise((resolve) => {
    if (process.platform === 'win32') {
      exec(`taskkill /pid ${pid} /T /F`, () => resolve());
    } else {
      try { process.kill(-pid, 'SIGTERM'); } catch (_) {}
      resolve();
    }
  });
};

const cleanupAllProcesses = async () => {
  console.log('[CLEANUP] Cleaning up all processes...');
  for (const [pid, info] of runningProcesses.entries()) {
    try {
      await killProcessTree(pid);
      try { info.process.kill('SIGKILL'); } catch (_) {}
    } catch (err) {
      console.error(`[CLEANUP] Error killing process ${pid}:`, err);
    }
  }
  runningProcesses.clear();
  shellToProcessPid.clear();
  blockIdToProcessPid.clear();
  console.log('[CLEANUP] All processes cleaned up');
};

const connectToServer = (port, retries = 30) => {
  return new Promise((resolve, reject) => {
    let attemptCount = 0;

    const attemptConnection = () => {
      attemptCount++;
      console.log(`[SERVER CONNECT] Connecting to pipe server on port ${port} (attempt ${attemptCount}/${retries})...`);

      const client = new net.Socket();
      client.setTimeout(1000);

      client.connect(port, '127.0.0.1', () => {
        console.log(`[SERVER CONNECT] ✓ Connected to C++ server on port ${port}`);
        serverSocketConnected = true;
        mainWindow.webContents.send('server-socket-status', { connected: true, port });
        client.setTimeout(0);
        resolve(client);
      });

      client.on('data', (data) => {
        const messages = data.toString().split('\n').filter(msg => msg.trim());
        messages.forEach(msg => {
          try {
            mainWindow.webContents.send('server-message', JSON.parse(msg));
          } catch (err) {
            console.error('[SERVER CONNECT] Failed to parse server message:', msg, err);
          }
        });
      });

      client.on('error', (error) => {
        if (error.code === 'ECONNRESET') return;
        if (error.code === 'ECONNREFUSED' && attemptCount < retries) {
          setTimeout(attemptConnection, 500);
        } else if (serverSocketConnected) {
          mainWindow.webContents.send('server-socket-status', { connected: false, error: error.message });
        } else {
          reject(error);
        }
      });

      client.on('close', () => {
        serverSocketConnected = false;
        serverSocket = null;
        mainWindow.webContents.send('server-socket-status', { connected: false });
      });

      client.on('timeout', () => {
        client.destroy();
        if (attemptCount < retries) setTimeout(attemptConnection, 500);
        else reject(new Error('Connection timeout'));
      });
    };

    attemptConnection();
  });
};

// ============================================================
// LANGUAGE SOCKET SERVERS
// ============================================================

const startLanguageSocketServers = () => {
  console.log('━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━');
  console.log('  INITIALIZING LANGUAGE SOCKET SERVERS');
  console.log('━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n');
  
  // MATLAB Socket Server
  console.log(`[MATLAB SERVER] Creating server for port ${matlabPort}...`);
  try {
    matlabSocketServer = net.createServer((client) => {
      const clientId = `${client.remoteAddress}:${client.remotePort}`;
      console.log(`[MATLAB SERVER] ✓ Client connected: ${clientId}`);
      matlabClients.set(clientId, { socket: client, buffer: '' });

      client.on('data', (data) => {
        const info = matlabClients.get(clientId);
        if (!info) return;
        info.buffer += data.toString();
        const lines = info.buffer.split('\n');
        info.buffer = lines.pop();

        lines.forEach(line => {
          if (!line.trim()) return;
          try {
            const parsed = JSON.parse(line);
            console.log(`[MATLAB SERVER] Received message from ${clientId}:`, parsed.type, `(block: ${parsed.blockName})`);

            if (parsed.type === 'BLOCK_INIT' && parsed.pid && parsed.pid > 0) {
              onBlockInit(parsed.blockId, parsed.blockName, parsed.pid, 'MATLAB');
            }

            if (parsed.type === 'BLOCK_STOPPED' || parsed.type === 'BLOCK_ERROR') {
              onBlockStopped(parsed.blockId, parsed.blockName);
            }

            mainWindow.webContents.send('block-message', { ...parsed, language: 'MATLAB' });
          } catch (err) {
            console.error(`[MATLAB SERVER] Parse error from ${clientId}:`, err, 'Data:', line);
          }
        });
      });

      client.on('close', () => { 
        console.log(`[MATLAB SERVER] Client disconnected: ${clientId}`);
        matlabClients.delete(clientId); 
      });
      client.on('error', (err) => { 
        console.error(`[MATLAB SERVER] Client error ${clientId}:`, err.message);
        matlabClients.delete(clientId); 
      });
    });

    matlabSocketServer.on('error', (err) => {
      console.error(`[MATLAB SERVER] ✗ SERVER ERROR:`, err.message);
      if (err.code === 'EADDRINUSE') {
        console.error(`[MATLAB SERVER] ✗ Port ${matlabPort} is already in use!`);
      }
    });
    
    // FIX: Use '127.0.0.1' instead of 'localhost'.
    // On Windows, 'localhost' resolves to ::1 (IPv6) in Node.js,
    // but C++ blocks connect to 127.0.0.1 (IPv4) — causing ECONNREFUSED.
    matlabSocketServer.listen(matlabPort, '127.0.0.1', () => {
      console.log(`[MATLAB SERVER] ✓✓✓ LISTENING ON 127.0.0.1:${matlabPort} ✓✓✓`);
      console.log(`[MATLAB SERVER] Ready to accept MATLAB block connections\n`);
    });
  } catch (err) {
    console.error(`[MATLAB SERVER] ✗ Failed to create server:`, err);
  }

  // C++ Socket Server
  console.log(`[C++ SERVER] Creating server for port ${cppPort}...`);
  try {
    cppSocketServer = net.createServer((client) => {
      const clientId = `${client.remoteAddress}:${client.remotePort}`;
      console.log(`[C++ SERVER] ✓ Client connected: ${clientId}`);
      cppClients.set(clientId, { socket: client, buffer: '' });

      client.on('data', (data) => {
        const info = cppClients.get(clientId);
        if (!info) return;
        info.buffer += data.toString();
        const lines = info.buffer.split('\n');
        info.buffer = lines.pop();

        lines.forEach(line => {
          if (!line.trim()) return;
          try {
            const parsed = JSON.parse(line);
            console.log(`[C++ SERVER] Received message from ${clientId}:`, parsed.type, `(block: ${parsed.blockName})`);

            if (parsed.type === 'BLOCK_INIT' && parsed.pid && parsed.pid > 0) {
              onBlockInit(parsed.blockId, parsed.blockName, parsed.pid, 'C++');
            }

            if (parsed.type === 'BLOCK_STOPPED' || parsed.type === 'BLOCK_ERROR') {
              onBlockStopped(parsed.blockId, parsed.blockName);
            }

            mainWindow.webContents.send('block-message', { ...parsed, language: 'C++' });
          } catch (err) {
            console.error(`[C++ SERVER] Parse error from ${clientId}:`, err, 'Data:', line);
          }
        });
      });

      client.on('close', () => { 
        console.log(`[C++ SERVER] Client disconnected: ${clientId}`);
        cppClients.delete(clientId); 
      });
      client.on('error', (err) => { 
        console.error(`[C++ SERVER] Client error ${clientId}:`, err.message);
        cppClients.delete(clientId); 
      });
    });

    cppSocketServer.on('error', (err) => {
      console.error(`[C++ SERVER] ✗ SERVER ERROR:`, err.message);
      if (err.code === 'EADDRINUSE') {
        console.error(`[C++ SERVER] ✗ Port ${cppPort} is already in use!`);
      }
    });
    
    // FIX: Use '127.0.0.1' instead of 'localhost'.
    // On Windows, 'localhost' resolves to ::1 (IPv6) in Node.js,
    // but C++ blocks connect to 127.0.0.1 (IPv4) — causing ECONNREFUSED.
    cppSocketServer.listen(cppPort, '127.0.0.1', () => {
      console.log(`[C++ SERVER] ✓✓✓ LISTENING ON 127.0.0.1:${cppPort} ✓✓✓`);
      console.log(`[C++ SERVER] Ready to accept C++ block connections\n`);
    });
  } catch (err) {
    console.error(`[C++ SERVER] ✗ Failed to create server:`, err);
  }
  
  console.log('━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━');
  console.log('  SOCKET SERVER INITIALIZATION COMPLETE');
  console.log('━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n');
};

const stopLanguageSocketServers = () => {
  console.log('[SHUTDOWN] Stopping language socket servers...');
  
  if (matlabSocketServer) {
    matlabClients.forEach((info) => { try { info.socket.destroy(); } catch (_) {} });
    matlabClients.clear();
    matlabSocketServer.close();
    matlabSocketServer = null;
    console.log('[SHUTDOWN] MATLAB Socket Server stopped');
  }
  
  if (cppSocketServer) {
    cppClients.forEach((info) => { try { info.socket.destroy(); } catch (_) {} });
    cppClients.clear();
    cppSocketServer.close();
    cppSocketServer = null;
    console.log('[SHUTDOWN] C++ Socket Server stopped');
  }
};

// ============================================================
// IPC HANDLERS
// ============================================================

ipcMain.handle('get-instance-config', async () => {
  console.log('[IPC] get-instance-config called');
  return { instanceId, serverPort, matlabPort, cppPort };
});

ipcMain.handle('get-app-path', async () => {
  const cwd = process.cwd();
  console.log('[IPC] get-app-path called:', cwd);
  return cwd;
});

ipcMain.handle('get-next-block-id', async () => {
  const id = blockIdCounter++;
  console.log(`[IPC] get-next-block-id called: returning ${id}`);
  return id;
});

ipcMain.handle('read-file', async (event, filepath) => {
  try { return await fs.readFile(filepath, 'utf-8'); }
  catch (err) { throw new Error(`Failed to read file: ${err.message}`); }
});

ipcMain.handle('write-file', async (event, filepath, content) => {
  try { await fs.writeFile(filepath, content, 'utf-8'); return { success: true }; }
  catch (err) { throw new Error(`Failed to write file: ${err.message}`); }
});

ipcMain.handle('select-file', async (event, filters) => {
  const result = await dialog.showOpenDialog(mainWindow, {
    properties: ['openFile', 'multiSelections'],
    filters: filters || [{ name: 'All Files', extensions: ['*'] }]
  });
  return result.filePaths;
});

ipcMain.handle('save-file-dialog', async (event, defaultPath, filters) => {
  const result = await dialog.showSaveDialog(mainWindow, {
    defaultPath,
    filters: filters || [{ name: 'All Files', extensions: ['*'] }]
  });
  return result.filePath;
});

ipcMain.handle('select-directory', async () => {
  const result = await dialog.showOpenDialog(mainWindow, { properties: ['openDirectory'] });
  return result.filePaths[0];
});

ipcMain.handle('ensure-dir', async (event, dirpath) => {
  try { await fs.mkdir(dirpath, { recursive: true }); return { success: true }; }
  catch (err) { throw new Error(`Failed to create directory: ${err.message}`); }
});

ipcMain.handle('exec-command', async (event, command, cwd) => {
  console.log(`[IPC] exec-command: ${command.substring(0, 100)}...`);
  try {
    const { stdout, stderr } = await execAsync(command, { cwd: cwd || process.cwd(), shell: true, windowsHide: true });
    return { success: true, stdout, stderr };
  } catch (err) {
    return { success: false, error: err.message, stdout: err.stdout, stderr: err.stderr };
  }
});

ipcMain.handle('start-server-with-socket', async (event, command, cwd, processName) => {
  console.log(`[IPC] start-server-with-socket: ${processName}`);
  try {
    const child = spawn(command, [], {
      cwd: cwd || process.cwd(), shell: true, detached: false,
      windowsHide: false, stdio: ['ignore', 'pipe', 'pipe']
    });

    const pid = child.pid;
    if (!pid) throw new Error('Failed to get process PID');

    runningProcesses.set(pid, { process: child, name: processName || 'unnamed', command, startTime: new Date(), isServer: true });

    child.stdout.on('data', (data) => console.log(`[${processName}] STDOUT:`, data.toString()));
    child.stderr.on('data', (data) => console.log(`[${processName}] STDERR:`, data.toString()));
    child.on('exit', (code, signal) => {
      runningProcesses.delete(pid);
      mainWindow.webContents.send('process-output', { pid, type: 'exit', name: processName, code, signal });
    });
    child.on('error', () => runningProcesses.delete(pid));

    setTimeout(async () => {
      try {
        serverSocket = await connectToServer(serverPort, 30);
      } catch (err) {
        mainWindow.webContents.send('server-socket-status', { connected: false, error: 'Failed to connect to server' });
      }
    }, 500);

    return { success: true, pid };
  } catch (err) {
    return { success: false, error: err.message };
  }
});

ipcMain.handle('start-process', async (event, command, cwd, processName) => {
  console.log(`[IPC] start-process: ${processName}`);
  console.log(`[IPC]   Command: ${command.substring(0, 200)}...`);
  try {
    const child = spawn(command, [], {
      cwd: cwd || process.cwd(),
      shell: true,
      detached: false,
      windowsHide: false,
      stdio: ['ignore', 'pipe', 'pipe']
    });

    const shellPid = child.pid;
    if (!shellPid) throw new Error('Failed to get process PID');

    runningProcesses.set(shellPid, {
      process: child,
      name: processName || 'unnamed',
      command,
      startTime: new Date(),
      realProcessPid: null
    });

    console.log(`[PROCESS] Started ${processName} (Shell PID: ${shellPid})`);
    console.log(`[PROCESS]   Waiting for block to self-report via BLOCK_INIT...`);

    mainWindow.webContents.send('process-output', {
      pid: shellPid, type: 'started', name: processName,
      data: `Process started: ${processName || command}`
    });

    child.stdout.on('data', (data) => {
      const output = data.toString();
      console.log(`[${shellPid}/${processName}] STDOUT:`, output);
      mainWindow.webContents.send('process-output', { pid: shellPid, type: 'stdout', name: processName, data: output });
    });

    child.stderr.on('data', (data) => {
      const output = data.toString();
      console.log(`[${shellPid}/${processName}] STDERR:`, output);
      mainWindow.webContents.send('process-output', { pid: shellPid, type: 'stderr', name: processName, data: output });
    });

    child.on('exit', (code, signal) => {
      console.log(`[PROCESS] ${processName} (Shell PID: ${shellPid}) exited with code ${code}`);
      const realPid = runningProcesses.get(shellPid)?.realProcessPid;
      if (realPid) {
        unregisterPidWithServer(realPid, processName || 'unnamed');
      }
      shellToProcessPid.delete(shellPid);
      runningProcesses.delete(shellPid);
      mainWindow.webContents.send('process-output', { pid: shellPid, type: 'exit', name: processName, code, signal });
    });

    child.on('error', (err) => {
      console.error(`[PROCESS] ${processName} (Shell PID: ${shellPid}) error:`, err);
      const realPid = runningProcesses.get(shellPid)?.realProcessPid;
      if (realPid) unregisterPidWithServer(realPid, processName || 'unnamed');
      shellToProcessPid.delete(shellPid);
      runningProcesses.delete(shellPid);
      mainWindow.webContents.send('process-output', { pid: shellPid, type: 'error', name: processName, data: err.message });
    });

    return { success: true, pid: shellPid };
  } catch (err) {
    console.error(`[PROCESS] Failed to start ${processName}:`, err);
    return { success: false, error: err.message };
  }
});

ipcMain.handle('kill-process', async (event, pid) => {
  try {
    const processInfo = runningProcesses.get(pid);
    if (!processInfo) return { success: false, error: 'Process not found' };

    const realPid = processInfo.realProcessPid || shellToProcessPid.get(pid);
    if (realPid) unregisterPidWithServer(realPid, processInfo.name);
    shellToProcessPid.delete(pid);

    for (const [bId, mPid] of blockIdToProcessPid.entries()) {
      if (mPid === realPid) { blockIdToProcessPid.delete(bId); break; }
    }

    await killProcessTree(pid);
    try { processInfo.process.kill('SIGTERM'); } catch (_) {}
    setTimeout(() => { try { processInfo.process.kill('SIGKILL'); } catch (_) {} }, 1000);

    runningProcesses.delete(pid);
    mainWindow.webContents.send('process-output', { pid, type: 'killed', name: processInfo.name, data: 'Process terminated' });
    return { success: true };
  } catch (err) {
    return { success: false, error: err.message };
  }
});

ipcMain.handle('get-running-processes', async () => {
  const processes = Array.from(runningProcesses.entries()).map(([pid, info]) => ({
    pid,
    realProcessPid: info.realProcessPid || null,
    name: info.name,
    command: info.command,
    startTime: info.startTime,
    language: info.language || 'unknown'
  }));
  return { success: true, processes };
});

ipcMain.handle('kill-all-processes', async () => {
  for (const [shellPid, info] of runningProcesses.entries()) {
    if (!info.isServer && info.realProcessPid) {
      unregisterPidWithServer(info.realProcessPid, info.name);
    }
  }
  shellToProcessPid.clear();
  blockIdToProcessPid.clear();
  await cleanupAllProcesses();
  return { success: true, killedCount: runningProcesses.size };
});

ipcMain.handle('send-to-server', async (event, message) => {
  if (!serverSocket || !serverSocketConnected) return { success: false, error: 'Server not connected' };
  try {
    serverSocket.write(JSON.stringify(message) + '\n');
    return { success: true };
  } catch (err) {
    return { success: false, error: err.message };
  }
});

// ============================================================
// APP LIFECYCLE
// ============================================================

app.whenReady().then(() => {
  console.log('[APP] Electron app is ready');
  createWindow();
  app.on('activate', () => { 
    if (BrowserWindow.getAllWindows().length === 0) createWindow(); 
  });
});

app.on('window-all-closed', async () => {
  console.log('[APP] All windows closed');
  await cleanupAllProcesses();
  if (serverSocket) { serverSocket.destroy(); serverSocket = null; }
  stopLanguageSocketServers();
  if (process.platform !== 'darwin') app.quit();
});

app.on('before-quit', async () => {
  console.log('[APP] App quitting...');
  await cleanupAllProcesses();
  if (serverSocket) { serverSocket.destroy(); serverSocket = null; }
  stopLanguageSocketServers();
});

process.on('SIGINT', async () => {
  console.log('[APP] SIGINT received');
  await cleanupAllProcesses();
  if (serverSocket) { serverSocket.destroy(); serverSocket = null; }
  process.exit(0);
});

process.on('SIGTERM', async () => {
  console.log('[APP] SIGTERM received');
  await cleanupAllProcesses();
  if (serverSocket) { serverSocket.destroy(); serverSocket = null; }
  process.exit(0);
});