import { app, BrowserWindow, ipcMain, dialog, screen } from 'electron';
import path from 'node:path';
import fs from 'fs/promises';
import { exec, spawn } from 'child_process';
import { promisify } from 'util';
import started from 'electron-squirrel-startup';
import net from 'net';

const execAsync = promisify(exec);
const runningProcesses = new Map();   // shellPid -> { process, name, realMatlabPid, ... }
const shellToMatlabPid = new Map();   // shellPid -> realMatlabPid (set when MATLAB reports itself)
const blockIdToMatlabPid = new Map(); // blockId  -> realMatlabPid (from BLOCK_INIT message)

let serverSocket = null;
let serverSocketConnected = false;

let instanceId = null;
let serverPort = null;
let matlabPort = null;

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

let matlabSocketServer = null;
const matlabClients = new Map();

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
  console.log(`[PID Registry] ✓ Registering MATLAB PID ${realPid} (${blockName})`);
  sendToServer({ type: 'REGISTER_PID', pid: realPid, name: blockName });
};

const unregisterPidWithServer = (realPid, blockName) => {
  console.log(`[PID Registry] Unregistering MATLAB PID ${realPid} (${blockName})`);
  sendToServer({ type: 'UNREGISTER_PID', pid: realPid, name: blockName });
};

// ============================================================
// MATLAB SELF-REPORTED PID
// ============================================================
//
// MATLAB calls feature('getpid') and includes it in the BLOCK_INIT
// socket message. When we receive that message we register the real
// MATLAB PID immediately - no polling, no wmic, no PowerShell needed.
//
// Called from the MATLAB socket data handler below.

const onMatlabBlockInit = (blockId, blockName, matlabPid) => {
  if (!matlabPid || matlabPid <= 0) return;

  console.log(`[PID Registry] MATLAB block "${blockName}" (ID:${blockId}) self-reported PID: ${matlabPid}`);

  // Store mapping from blockId -> real PID
  blockIdToMatlabPid.set(String(blockId), matlabPid);

  // Also link it back to the shell process so kill-process finds it
  // Find the runningProcesses entry whose blockId matches
  for (const [shellPid, entry] of runningProcesses.entries()) {
    // Match by name since blockId isn't stored on the process entry
    if (entry.name === blockName && !entry.realMatlabPid) {
      entry.realMatlabPid = matlabPid;
      shellToMatlabPid.set(shellPid, matlabPid);
      console.log(`[PID Registry] Linked shell PID ${shellPid} -> MATLAB PID ${matlabPid} (${blockName})`);
      break;
    }
  }

  registerPidWithServer(matlabPid, blockName);
};

const onMatlabBlockStopped = (blockId, blockName) => {
  const matlabPid = blockIdToMatlabPid.get(String(blockId));
  if (matlabPid) {
    unregisterPidWithServer(matlabPid, blockName);
    blockIdToMatlabPid.delete(String(blockId));
  }
};

// ============================================================

const createWindow = async () => {
  instanceId = generateInstanceId();
  serverPort = await findAvailablePort(9000);
  matlabPort = await findAvailablePort(9001);

  console.log(`========================================`);
  console.log(`INSTANCE ID: ${instanceId}`);
  console.log(`Server Port: ${serverPort}`);
  console.log(`MATLAB Port: ${matlabPort}`);
  console.log(`========================================`);

  const primaryDisplay = screen.getPrimaryDisplay();
  const { width, height } = primaryDisplay.workAreaSize;
  const windowWidth = Math.min(1400, Math.floor(width * 0.9));
  const windowHeight = Math.min(900, Math.floor(height * 0.9));

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
  } else {
    mainWindow.loadFile(path.join(__dirname, `../renderer/${MAIN_WINDOW_VITE_NAME}/index.html`));
  }

  mainWindow.webContents.openDevTools();

  mainWindow.on('close', async (e) => {
    e.preventDefault();
    await cleanupAllProcesses();
    if (serverSocket) { serverSocket.destroy(); serverSocket = null; }
    stopMatlabSocketServer();
    mainWindow.destroy();
  });

  startMatlabSocketServer();
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
  console.log('Cleaning up all processes...');
  for (const [pid, info] of runningProcesses.entries()) {
    try {
      await killProcessTree(pid);
      try { info.process.kill('SIGKILL'); } catch (_) {}
    } catch (err) {
      console.error(`Error killing process ${pid}:`, err);
    }
  }
  runningProcesses.clear();
  shellToMatlabPid.clear();
  blockIdToMatlabPid.clear();
  console.log('All processes cleaned up');
};

const connectToServer = (port, retries = 30) => {
  return new Promise((resolve, reject) => {
    let attemptCount = 0;

    const attemptConnection = () => {
      attemptCount++;
      console.log(`Connecting to server on port ${port} (attempt ${attemptCount}/${retries})...`);

      const client = new net.Socket();
      client.setTimeout(1000);

      client.connect(port, 'localhost', () => {
        console.log(`Connected to C++ server on port ${port}`);
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
            console.error('Failed to parse server message:', msg, err);
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
// IPC HANDLERS
// ============================================================

ipcMain.handle('get-instance-config', async () => ({ instanceId, serverPort, matlabPort }));
ipcMain.handle('get-app-path', async () => process.cwd());

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
  try {
    const { stdout, stderr } = await execAsync(command, { cwd: cwd || process.cwd(), shell: true, windowsHide: true });
    return { success: true, stdout, stderr };
  } catch (err) {
    return { success: false, error: err.message, stdout: err.stdout, stderr: err.stderr };
  }
});

// Start the C++ pipe server
ipcMain.handle('start-server-with-socket', async (event, command, cwd, processName) => {
  try {
    const child = spawn(command, [], {
      cwd: cwd || process.cwd(), shell: true, detached: false,
      windowsHide: false, stdio: ['ignore', 'pipe', 'pipe']
    });

    const pid = child.pid;
    if (!pid) throw new Error('Failed to get process PID');

    runningProcesses.set(pid, { process: child, name: processName || 'unnamed', command, startTime: new Date(), isServer: true });

    child.stdout.on('data', (data) => console.log(`[server] STDOUT:`, data.toString()));
    child.stderr.on('data', (data) => console.log(`[server] STDERR:`, data.toString()));
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

// Start a MATLAB block process
// The real MATLAB PID is reported back via BLOCK_INIT socket message (see onMatlabBlockInit)
ipcMain.handle('start-process', async (event, command, cwd, processName) => {
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
      realMatlabPid: null   // filled when MATLAB sends BLOCK_INIT with its PID
    });

    console.log(`[${processName}] Shell PID: ${shellPid} — waiting for MATLAB to self-report its PID via BLOCK_INIT...`);

    mainWindow.webContents.send('process-output', {
      pid: shellPid, type: 'started', name: processName,
      data: `Process started: ${processName || command}`
    });

    child.stdout.on('data', (data) => {
      const output = data.toString();
      console.log(`[${shellPid}] STDOUT:`, output);
      mainWindow.webContents.send('process-output', { pid: shellPid, type: 'stdout', name: processName, data: output });
    });

    child.stderr.on('data', (data) => {
      const output = data.toString();
      console.log(`[${shellPid}] STDERR:`, output);
      mainWindow.webContents.send('process-output', { pid: shellPid, type: 'stderr', name: processName, data: output });
    });

    child.on('exit', (code, signal) => {
      console.log(`[${shellPid}] Shell exited code=${code}`);
      // If MATLAB already reported its PID, unregister it
      const realPid = runningProcesses.get(shellPid)?.realMatlabPid;
      if (realPid) {
        unregisterPidWithServer(realPid, processName || 'unnamed');
      }
      shellToMatlabPid.delete(shellPid);
      runningProcesses.delete(shellPid);
      mainWindow.webContents.send('process-output', { pid: shellPid, type: 'exit', name: processName, code, signal });
    });

    child.on('error', (err) => {
      console.error(`[${shellPid}] Process error:`, err);
      const realPid = runningProcesses.get(shellPid)?.realMatlabPid;
      if (realPid) unregisterPidWithServer(realPid, processName || 'unnamed');
      shellToMatlabPid.delete(shellPid);
      runningProcesses.delete(shellPid);
      mainWindow.webContents.send('process-output', { pid: shellPid, type: 'error', name: processName, data: err.message });
    });

    return { success: true, pid: shellPid };
  } catch (err) {
    console.error('Failed to start process:', err);
    return { success: false, error: err.message };
  }
});

// Kill a specific block — unregisters real PID from pipe server first
ipcMain.handle('kill-process', async (event, pid) => {
  try {
    const processInfo = runningProcesses.get(pid);
    if (!processInfo) return { success: false, error: 'Process not found' };

    // Unregister real MATLAB PID so pipe server doesn't double-kill
    const realPid = processInfo.realMatlabPid || shellToMatlabPid.get(pid);
    if (realPid) unregisterPidWithServer(realPid, processInfo.name);
    shellToMatlabPid.delete(pid);

    // Also remove from blockId map
    for (const [bId, mPid] of blockIdToMatlabPid.entries()) {
      if (mPid === realPid) { blockIdToMatlabPid.delete(bId); break; }
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
    realMatlabPid: info.realMatlabPid || null,
    name: info.name,
    command: info.command,
    startTime: info.startTime
  }));
  return { success: true, processes };
});

// Kill all - unregister all real MATLAB PIDs before killing
ipcMain.handle('kill-all-processes', async () => {
  for (const [shellPid, info] of runningProcesses.entries()) {
    if (!info.isServer && info.realMatlabPid) {
      unregisterPidWithServer(info.realMatlabPid, info.name);
    }
  }
  shellToMatlabPid.clear();
  blockIdToMatlabPid.clear();
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
// MATLAB SOCKET SERVER
// Receives messages from MATLAB blocks including BLOCK_INIT with self-reported PID
// ============================================================
const startMatlabSocketServer = () => {
  matlabSocketServer = net.createServer((client) => {
    const clientId = `${client.remoteAddress}:${client.remotePort}`;
    console.log(`[MATLAB Socket] Client connected: ${clientId}`);
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
          console.log('[MATLAB Socket] Received:', parsed);

          // PRIMARY PID REGISTRATION: MATLAB includes its own PID in BLOCK_INIT
          if (parsed.type === 'BLOCK_INIT' && parsed.pid && parsed.pid > 0) {
            onMatlabBlockInit(parsed.blockId, parsed.blockName, parsed.pid);
          }

          // Unregister when block stops cleanly
          if (parsed.type === 'BLOCK_STOPPED' || parsed.type === 'BLOCK_ERROR') {
            onMatlabBlockStopped(parsed.blockId, parsed.blockName);
          }

          // Forward all messages to renderer as before
          mainWindow.webContents.send('matlab-message', parsed);
        } catch (err) {
          console.error('[MATLAB Socket] Parse error:', err, 'Data:', line);
        }
      });
    });

    client.on('close', () => { matlabClients.delete(clientId); });
    client.on('error', () => { matlabClients.delete(clientId); });
  });

  matlabSocketServer.on('error', (err) => console.error('[MATLAB Socket Server] Error:', err));
  matlabSocketServer.listen(matlabPort, () => console.log(`[MATLAB Socket Server] Listening on port ${matlabPort}`));
};

const stopMatlabSocketServer = () => {
  if (matlabSocketServer) {
    matlabClients.forEach((info) => { try { info.socket.destroy(); } catch (_) {} });
    matlabClients.clear();
    matlabSocketServer.close();
    matlabSocketServer = null;
  }
};

// ============================================================
// APP LIFECYCLE
// ============================================================
app.whenReady().then(() => {
  createWindow();
  app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
});

app.on('window-all-closed', async () => {
  await cleanupAllProcesses();
  if (serverSocket) { serverSocket.destroy(); serverSocket = null; }
  stopMatlabSocketServer();
  if (process.platform !== 'darwin') app.quit();
});

app.on('before-quit', async () => {
  await cleanupAllProcesses();
  if (serverSocket) { serverSocket.destroy(); serverSocket = null; }
  stopMatlabSocketServer();
});

process.on('SIGINT', async () => {
  await cleanupAllProcesses();
  if (serverSocket) { serverSocket.destroy(); serverSocket = null; }
  process.exit(0);
});

process.on('SIGTERM', async () => {
  await cleanupAllProcesses();
  if (serverSocket) { serverSocket.destroy(); serverSocket = null; }
  process.exit(0);
});