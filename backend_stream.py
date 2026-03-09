import asyncio
import websockets

async def stream_file():
    uri = "ws://127.0.0.1:8001/ws/stt"
    async with websockets.connect(uri) as ws:
        print('connected')
        with open('capture.wav','rb') as f:
            while True:
                chunk = f.read(8192)
                if not chunk:
                    break
                await ws.send(chunk)
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=1.0)
                    print('recv:', msg)
                except asyncio.TimeoutError:
                    pass
        await ws.close()

asyncio.run(stream_file())
