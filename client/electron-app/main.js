const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');

function createWindow () {
  const win = new BrowserWindow({
    width: 600,
    height: 200,
    transparent: true,
    frame: false,
    alwaysOnTop: true,
    skipTaskbar: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false
    }
  })

  win.loadFile(path.join(__dirname, 'renderer', 'index.html'));
}

app.whenReady().then(() => {
  createWindow();

  app.on('activate', function () {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', function () {
  if (process.platform !== 'darwin') app.quit();
});

// Example IPC handlers (expand later)
ipcMain.handle('ping', async () => {
  return { ok: true };
});