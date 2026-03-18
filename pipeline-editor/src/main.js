import { app, BrowserWindow, ipcMain, dialog, screen } from 'electron';
import path from 'node:path';
import fs from 'fs/promises';
import { exec, spawn } from 'child_process';
import { promisify } from 'util';
import started from 'electron-squirrel-startup';
import net from 'net'; // Added for socket communication

const execAsync = promisify(exec);
const runningProcesses = new Map();
let serverSocket = null; // Socket connection to C++ server
let serverSocketConnected = false;

// MATLAB socket server
let matlabSocketServer = null;
const matlabClients = new Map();

if (started) {
  app.quit();
}

let mainWindow;

const createWindow = () => {
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
  
  // Handle window close event
  mainWindow.on('close', async (e) => {
    e.preventDefault();
    await cleanupAllProcesses();
    if (serverSocket) {
      serverSocket.destroy(); // This will trigger server shutdown
      serverSocket = null;
    }
    mainWindow.destroy();
  });
};

const killProcessTree = (pid) => {
  return new Promise((resolve, reject) => {
    if (process.platform === 'win32') {
      exec(`taskkill /pid ${pid} /T /F`, (error) => {
        resolve();
      });
    } else {
      try {
        process.kill(-pid, 'SIGTERM');
        resolve();
      } catch (error) {
        resolve();
      }
    }
  });
};

const cleanupAllProcesses = async () => {
  console.log('Cleaning up all processes...');
  const pids = Array.from(runningProcesses.keys());
  
  for (const pid of pids) {
    try {
      await killProcessTree(pid);
      const processInfo = runningProcesses.get(pid);
      if (processInfo) {
        try {
          processInfo.process.kill('SIGKILL');
        } catch (e) {
          // Ignore
        }
      }
    } catch (error) {
      console.error(`Error killing process ${pid}:`, error);
    }
  }
  
  runningProcesses.clear();
  console.log('All processes cleaned up');
};

// Connect to C++ server via socket
const connectToServer = (port = 9000, retries = 30) => {
  return new Promise((resolve, reject) => {
    let attemptCount = 0;
    
    const attemptConnection = () => {
      attemptCount++;
      console.log(`Attempting to connect to server on port ${port} (attempt ${attemptCount}/${retries})...`);
      
      const client = new net.Socket();
      
      // Set timeout for connection attempt
      client.setTimeout(1000);
      
      client.connect(port, 'localhost', () => {
        console.log('Connected to C++ server via socket');
        serverSocketConnected = true;
        
        // Send connection confirmation to UI
        mainWindow.webContents.send('server-socket-status', {
          connected: true,
          port: port
        });
        
        // Clear timeout
        client.setTimeout(0);
        
        resolve(client);
      });
      
      client.on('data', (data) => {
        // Parse JSON messages from server
        const messages = data.toString().split('\n').filter(msg => msg.trim());
        
        messages.forEach(msg => {
          try {
            const parsed = JSON.parse(msg);
            console.log('Received from server:', parsed);
            
            // Forward to UI
            mainWindow.webContents.send('server-message', parsed);
            
          } catch (error) {
            console.error('Failed to parse server message:', msg, error);
          }
        });
      });
      
      client.on('error', (error) => {
        if (error.code === 'ECONNREFUSED' && attemptCount < retries) {
          // Server not ready yet, retry
          setTimeout(attemptConnection, 500);
        } else if (serverSocketConnected) {
          console.error('Socket error:', error);
          mainWindow.webContents.send('server-socket-status', {
            connected: false,
            error: error.message
          });
        } else {
          reject(error);
        }
      });
      
      client.on('close', () => {
        console.log('Socket connection closed');
        serverSocketConnected = false;
        serverSocket = null;
        
        mainWindow.webContents.send('server-socket-status', {
          connected: false
        });
      });
      
      client.on('timeout', () => {
        client.destroy();
        if (attemptCount < retries) {
          setTimeout(attemptConnection, 500);
        } else {
          reject(new Error('Connection timeout'));
        }
      });
    };
    
    attemptConnection();
  });
};

// Get application path
ipcMain.handle('get-app-path', async () => {
  return process.cwd();
});

// File operations
ipcMain.handle('read-file', async (event, filepath) => {
  try {
    return await fs.readFile(filepath, 'utf-8');
  } catch (error) {
    throw new Error(`Failed to read file: ${error.message}`);
  }
});

ipcMain.handle('write-file', async (event, filepath, content) => {
  try {
    await fs.writeFile(filepath, content, 'utf-8');
    return { success: true };
  } catch (error) {
    throw new Error(`Failed to write file: ${error.message}`);
  }
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
  const result = await dialog.showOpenDialog(mainWindow, {
    properties: ['openDirectory']
  });
  return result.filePaths[0];
});

ipcMain.handle('ensure-dir', async (event, dirpath) => {
  try {
    await fs.mkdir(dirpath, { recursive: true });
    return { success: true };
  } catch (error) {
    throw new Error(`Failed to create directory: ${error.message}`);
  }
});

// Command execution
ipcMain.handle('exec-command', async (event, command, cwd) => {
  try {
    const { stdout, stderr } = await execAsync(command, { 
      cwd: cwd || process.cwd(),
      shell: true,
      windowsHide: true
    });
    return { success: true, stdout, stderr };
  } catch (error) {
    return { 
      success: false, 
      error: error.message,
      stdout: error.stdout,
      stderr: error.stderr
    };
  }
});

// Start server with socket connection
ipcMain.handle('start-server-with-socket', async (event, command, cwd, processName) => {
  try {
    console.log(`Starting server: ${processName}`);
    console.log(`Command: ${command}`);
    console.log(`CWD: ${cwd}`);

    // Start the C++ server process
    const child = spawn(command, [], {
      cwd: cwd || process.cwd(),
      shell: true,
      detached: false,
      windowsHide: false,
      stdio: ['ignore', 'pipe', 'pipe']
    });

    const pid = child.pid;
    
    if (!pid) {
      throw new Error('Failed to get process PID');
    }

    runningProcesses.set(pid, {
      process: child,
      name: processName || 'unnamed',
      command: command,
      startTime: new Date(),
      isServer: true
    });

    console.log(`Server process started with PID: ${pid}`);

    // Still capture stdout/stderr for debugging
    child.stdout.on('data', (data) => {
      const output = data.toString();
      console.log(`[${pid}] STDOUT:`, output);
    });

    child.stderr.on('data', (data) => {
      const output = data.toString();
      console.log(`[${pid}] STDERR:`, output);
    });

    child.on('exit', (code, signal) => {
      console.log(`[${pid}] Server process exited with code ${code}, signal ${signal}`);
      runningProcesses.delete(pid);
      
      // Notify UI
      mainWindow.webContents.send('process-output', {
        pid,
        type: 'exit',
        name: processName,
        code,
        signal
      });
    });

    child.on('error', (error) => {
      console.error(`[${pid}] Server process error:`, error);
      runningProcesses.delete(pid);
    });

    // Wait a bit for server to start, then connect socket
    setTimeout(async () => {
      try {
        serverSocket = await connectToServer(9000, 30);
        console.log('Socket connection established');
      } catch (error) {
        console.error('Failed to connect to server socket:', error);
        mainWindow.webContents.send('server-socket-status', {
          connected: false,
          error: 'Failed to connect to server'
        });
      }
    }, 500);

    return { success: true, pid };
  } catch (error) {
    console.error('Failed to start server:', error);
    return { success: false, error: error.message };
  }
});

// Process management (for MATLAB blocks - keep existing functionality)
ipcMain.handle('start-process', async (event, command, cwd, processName) => {
  try {
    console.log(`Starting process: ${processName || 'unnamed'}`);
    console.log(`Command: ${command}`);
    console.log(`CWD: ${cwd}`);

    const child = spawn(command, [], {
      cwd: cwd || process.cwd(),
      shell: true,
      detached: false,
      windowsHide: false,
      stdio: ['ignore', 'pipe', 'pipe']
    });

    const pid = child.pid;
    
    if (!pid) {
      throw new Error('Failed to get process PID');
    }

    runningProcesses.set(pid, {
      process: child,
      name: processName || 'unnamed',
      command: command,
      startTime: new Date()
    });

    console.log(`Process started with PID: ${pid}`);

    mainWindow.webContents.send('process-output', {
      pid,
      type: 'started',
      name: processName,
      data: `Process started: ${processName || command}`
    });

    child.stdout.on('data', (data) => {
      const output = data.toString();
      console.log(`[${pid}] STDOUT:`, output);
      mainWindow.webContents.send('process-output', {
        pid,
        type: 'stdout',
        name: processName,
        data: output
      });
    });

    child.stderr.on('data', (data) => {
      const output = data.toString();
      console.log(`[${pid}] STDERR:`, output);
      mainWindow.webContents.send('process-output', {
        pid,
        type: 'stderr',
        name: processName,
        data: output
      });
    });

    child.on('exit', (code, signal) => {
      console.log(`[${pid}] Process exited with code ${code}, signal ${signal}`);
      mainWindow.webContents.send('process-output', {
        pid,
        type: 'exit',
        name: processName,
        code,
        signal
      });
      runningProcesses.delete(pid);
    });

    child.on('error', (error) => {
      console.error(`[${pid}] Process error:`, error);
      mainWindow.webContents.send('process-output', {
        pid,
        type: 'error',
        name: processName,
        data: error.message
      });
      runningProcesses.delete(pid);
    });

    return { success: true, pid };
  } catch (error) {
    console.error('Failed to start process:', error);
    return { success: false, error: error.message };
  }
});

ipcMain.handle('kill-process', async (event, pid) => {
  try {
    console.log(`Attempting to kill process PID: ${pid}`);
    
    const processInfo = runningProcesses.get(pid);
    if (!processInfo) {
      console.log(`Process ${pid} not found in running processes`);
      return { success: false, error: 'Process not found' };
    }

    await killProcessTree(pid);
    
    try {
      processInfo.process.kill('SIGTERM');
    } catch (e) {
      // Ignore error
    }

    setTimeout(() => {
      try {
        processInfo.process.kill('SIGKILL');
      } catch (e) {
        // Ignore error
      }
    }, 1000);

    runningProcesses.delete(pid);
    
    console.log(`Process ${pid} killed successfully`);
    
    mainWindow.webContents.send('process-output', {
      pid,
      type: 'killed',
      name: processInfo.name,
      data: 'Process terminated'
    });
    
    return { success: true };
  } catch (error) {
    console.error(`Failed to kill process ${pid}:`, error);
    return { success: false, error: error.message };
  }
});

ipcMain.handle('get-running-processes', async () => {
  const processes = Array.from(runningProcesses.entries()).map(([pid, info]) => ({
    pid,
    name: info.name,
    command: info.command,
    startTime: info.startTime
  }));
  return { success: true, processes };
});

ipcMain.handle('kill-all-processes', async () => {
  await cleanupAllProcesses();
  return { success: true, killedCount: runningProcesses.size };
});

// Send message to server via socket
ipcMain.handle('send-to-server', async (event, message) => {
  if (!serverSocket || !serverSocketConnected) {
    return { success: false, error: 'Server not connected' };
  }
  
  try {
    const jsonMsg = JSON.stringify(message) + '\n';
    serverSocket.write(jsonMsg);
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

// ========================================
// MATLAB SOCKET SERVER (Port 9001)
// ========================================
const startMatlabSocketServer = () => {
  const port = 9001;
  
  matlabSocketServer = net.createServer((client) => {
    const clientId = `${client.remoteAddress}:${client.remotePort}`;
    console.log(`[MATLAB Socket] Client connected: ${clientId}`);
    
    matlabClients.set(clientId, {
      socket: client,
      buffer: ''
    });
    
    client.on('data', (data) => {
      // Handle data buffering for JSON messages
      const clientInfo = matlabClients.get(clientId);
      if (!clientInfo) return;
      
      clientInfo.buffer += data.toString();
      
      // Process complete JSON messages (separated by newlines)
      const lines = clientInfo.buffer.split('\n');
      clientInfo.buffer = lines.pop(); // Keep incomplete line in buffer
      
      lines.forEach(line => {
        if (line.trim()) {
          try {
            const parsed = JSON.parse(line);
            console.log('[MATLAB Socket] Received:', parsed);
            
            // Forward to UI
            mainWindow.webContents.send('matlab-message', parsed);
            
          } catch (error) {
            console.error('[MATLAB Socket] Parse error:', error, 'Data:', line);
          }
        }
      });
    });
    
    client.on('close', () => {
      console.log(`[MATLAB Socket] Client disconnected: ${clientId}`);
      matlabClients.delete(clientId);
    });
    
    client.on('error', (error) => {
      console.error(`[MATLAB Socket] Client error:`, error);
      matlabClients.delete(clientId);
    });
  });
  
  matlabSocketServer.on('error', (error) => {
    console.error('[MATLAB Socket Server] Error:', error);
  });
  
  matlabSocketServer.listen(port, () => {
    console.log(`[MATLAB Socket Server] Listening on port ${port} for MATLAB blocks`);
  });
};

const stopMatlabSocketServer = () => {
  if (matlabSocketServer) {
    console.log('[MATLAB Socket Server] Closing...');
    
    // Close all client connections
    matlabClients.forEach((clientInfo, clientId) => {
      try {
        clientInfo.socket.destroy();
      } catch (e) {
        console.error(`Error closing MATLAB client ${clientId}:`, e);
      }
    });
    matlabClients.clear();
    
    // Close server
    matlabSocketServer.close(() => {
      console.log('[MATLAB Socket Server] Closed');
    });
    matlabSocketServer = null;
  }
};

app.whenReady().then(() => {
  createWindow();
  startMatlabSocketServer(); // Start MATLAB socket server

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow();
    }
  });
});

app.on('window-all-closed', async () => {
  console.log('App closing - killing all processes');
  await cleanupAllProcesses();
  
  if (serverSocket) {
    serverSocket.destroy();
    serverSocket = null;
  }
  
  stopMatlabSocketServer(); // Stop MATLAB socket server
  
  if (process.platform !== 'darwin') {
    app.quit();
  }
});

app.on('before-quit', async () => {
  console.log('App quitting - cleanup');
  await cleanupAllProcesses();
  
  if (serverSocket) {
    serverSocket.destroy();
    serverSocket = null;
  }
  
  stopMatlabSocketServer(); // Stop MATLAB socket server
});

// Handle SIGINT (Ctrl+C)
process.on('SIGINT', async () => {
  console.log('Received SIGINT - cleaning up...');
  await cleanupAllProcesses();
  if (serverSocket) {
    serverSocket.destroy();
    serverSocket = null;
  }
  process.exit(0);
});

// Handle SIGTERM
process.on('SIGTERM', async () => {
  console.log('Received SIGTERM - cleaning up...');
  await cleanupAllProcesses();
  if (serverSocket) {
    serverSocket.destroy();
    serverSocket = null;
  }
  process.exit(0);
});