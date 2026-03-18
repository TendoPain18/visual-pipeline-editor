import { app, BrowserWindow, ipcMain, dialog, screen } from 'electron';
import path from 'node:path';
import fs from 'fs/promises';
import { exec, spawn } from 'child_process';
import { promisify } from 'util';
import started from 'electron-squirrel-startup';

const execAsync = promisify(exec);
const runningProcesses = new Map();

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

// Process management
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

app.whenReady().then(() => {
  createWindow();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow();
    }
  });
});

app.on('window-all-closed', async () => {
  console.log('App closing - killing all processes');
  await cleanupAllProcesses();
  
  if (process.platform !== 'darwin') {
    app.quit();
  }
});

app.on('before-quit', async () => {
  console.log('App quitting - cleanup');
  await cleanupAllProcesses();
});

// Handle SIGINT (Ctrl+C)
process.on('SIGINT', async () => {
  console.log('Received SIGINT - cleaning up...');
  await cleanupAllProcesses();
  process.exit(0);
});

// Handle SIGTERM
process.on('SIGTERM', async () => {
  console.log('Received SIGTERM - cleaning up...');
  await cleanupAllProcesses();
  process.exit(0);
});
