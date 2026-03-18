const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('electronAPI', {
  // File operations
  readFile: (filepath) => ipcRenderer.invoke('read-file', filepath),
  writeFile: (filepath, content) => ipcRenderer.invoke('write-file', filepath, content),
  selectFile: (filters) => ipcRenderer.invoke('select-file', filters),
  saveFileDialog: (defaultPath, filters) => ipcRenderer.invoke('save-file-dialog', defaultPath, filters),
  
  // Command execution
  execCommand: (command, cwd) => ipcRenderer.invoke('exec-command', command, cwd),
  
  // Process management
  startProcess: (command, cwd, processName) => ipcRenderer.invoke('start-process', command, cwd, processName),
  killProcess: (pid) => ipcRenderer.invoke('kill-process', pid),
  getRunningProcesses: () => ipcRenderer.invoke('get-running-processes'),
  killAllProcesses: () => ipcRenderer.invoke('kill-all-processes'),
  
  // Directory operations
  selectDirectory: () => ipcRenderer.invoke('select-directory'),
  ensureDir: (dirpath) => ipcRenderer.invoke('ensure-dir', dirpath),
  getAppPath: () => ipcRenderer.invoke('get-app-path'),
  
  // Platform information
  getPlatform: () => process.platform,
  
  // Listen to process output
  onProcessOutput: (callback) => {
    const subscription = (event, data) => callback(data);
    ipcRenderer.on('process-output', subscription);
    return () => ipcRenderer.removeListener('process-output', subscription);
  },
  removeProcessOutputListener: () => ipcRenderer.removeAllListeners('process-output')
});