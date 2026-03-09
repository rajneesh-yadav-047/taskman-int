const startBtn = document.getElementById('startBtn');
const stopBtn = document.getElementById('stopBtn');
const captions = document.getElementById('captions');
const status = document.getElementById('status');

let streamTrack = null;

startBtn.onclick = async () => {
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
    streamTrack = stream.getTracks()[0];
    captions.innerText = 'Capturing audio... (local only)';
    status.innerText = 'Status: Capturing (visible)';

    // Placeholder: send audio to backend STT via WebRTC/WebSocket or process locally
  } catch (err) {
    captions.innerText = 'Audio permission denied or error: ' + err.message;
  }
};

stopBtn.onclick = () => {
  if (streamTrack) {
    streamTrack.stop();
    streamTrack = null;
    captions.innerText = 'Stopped.';
    status.innerText = 'Status: Ready';
  }
};

// Ping main process
window.electronAPI.ping().then(res => {
  console.log('Backend ping:', res);
});