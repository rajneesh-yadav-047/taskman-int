from fastapi import FastAPI, UploadFile, File, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse
import json
import asyncio
import os
import httpx
from dotenv import load_dotenv

# Load local backend/.env if present (dev convenience). Do NOT commit backend/.env.
load_dotenv(dotenv_path=os.path.join(os.path.dirname(__file__), '.env'))

# VOSK setup for live STT
try:
    from vosk import Model, KaldiRecognizer
    VOSK_MODEL_PATH = "vosk-model"
    if not os.path.exists(VOSK_MODEL_PATH):
        print(f"ERROR: Vosk model not found at '{VOSK_MODEL_PATH}'. Please download a model.")
        VOSK_MODEL = None
    else:
        VOSK_MODEL = Model(VOSK_MODEL_PATH)
except ImportError:
    print("WARNING: 'vosk' not installed. Live transcription will not work. `pip install vosk`")
    VOSK_MODEL = None

app = FastAPI(title="taskman-int-backend")

@app.get('/api/ping')
async def ping():
    return {"ok": True}


@app.post('/api/llm')
async def llm_query(payload: dict):
    """Proxy endpoint to an LLM.

    Expects JSON with one of the keys: 'q', 'question', 'prompt'.
    Reads the OpenAI key from environment variable TASKMAN_API_KEY.
    Returns JSON: {"answer": "<assistant reply>"}.
    """
    # Delegate to helper that can be called by other transports (HTTP or local socket)
    OPENAI_KEY = os.getenv("TASKMAN_API_KEY")
    if not OPENAI_KEY:
        return JSONResponse(status_code=500, content={"error": "TASKMAN_API_KEY not set on server"})

    # Accept several common payload field names
    prompt = payload.get('q') or payload.get('question') or payload.get('prompt') or payload.get('query')
    if not prompt:
        return JSONResponse(status_code=400, content={"error": "no prompt provided (send 'q' or 'question')"})

    try:
        answer = await process_llm_prompt(prompt)
        return JSONResponse({"answer": answer})
    except httpx.RequestError as e:
        return JSONResponse(status_code=502, content={"error": "request error", "details": str(e)})


async def process_llm_prompt(prompt: str) -> str:
    """Core LLM proxy logic. Can be reused by different transports."""
    OPENAI_KEY = os.getenv("TASKMAN_API_KEY")
    provider = os.getenv("TASKMAN_API_PROVIDER", "auto").lower()
    use_gemini = provider == "gemini" or (OPENAI_KEY and OPENAI_KEY.startswith("AIza") and provider == "auto")

    async with httpx.AsyncClient(timeout=30.0) as client:
        if use_gemini:
            # Use a modern Gemini model and the generateContent endpoint.
            gemini_model = os.getenv("GEMINI_MODEL", "gemini-flash-latest")
            url = f"https://generativelanguage.googleapis.com/v1beta/models/{gemini_model}:generateContent"

            headers = {"Content-Type": "application/json"}
            params = None
            if OPENAI_KEY and OPENAI_KEY.startswith("AIza"):
                params = {"key": OPENAI_KEY}
            else:
                if OPENAI_KEY:
                    headers["Authorization"] = f"Bearer {OPENAI_KEY}"

            # New payload structure for generateContent
            body = {
                "contents": [{
                    "parts": [{"text": prompt}]
                }],
                "generationConfig": {
                    "temperature": float(os.getenv("GEMINI_TEMPERATURE", "0.7")),
                    "maxOutputTokens": int(os.getenv("GEMINI_MAX_TOKENS", "512")),
                }
            }

            print(f"[proxy] Gemini request -> URL: {url} params={params} headers_keys={list(headers.keys())}")
            resp = await client.post(url, params=params, headers=headers, json=body)
            if resp.status_code != 200:
                print(f"[proxy] Gemini response status={resp.status_code} text={resp.text.strip()}")
                raise httpx.RequestError(f"Gemini API error: {resp.status_code}, {resp.text}")
            data = resp.json()
            # extract answer from the new structure
            answer = None
            try:
                answer = data['candidates'][0]['content']['parts'][0]['text']
            except (KeyError, IndexError, TypeError):
                answer = json.dumps(data)
            return answer
        else:
            model = os.getenv("OPENAI_MODEL", "gpt-3.5-turbo")
            body = {
                "model": model,
                "messages": [{"role": "user", "content": prompt}],
                "max_tokens": int(os.getenv("OPENAI_MAX_TOKENS", "512")),
            }
            headers = {"Authorization": f"Bearer {OPENAI_KEY}", "Content-Type": "application/json"}
            resp = await client.post("https://api.openai.com/v1/chat/completions", headers=headers, json=body)
            if resp.status_code != 200:
                raise httpx.RequestError(f"OpenAI API error: {resp.status_code}")
            data = resp.json()
            answer = None
            try:
                answer = data["choices"][0]["message"]["content"]
            except Exception:
                answer = json.dumps(data)
            return answer


@app.post('/api/upload_resume')
async def upload_resume(file: UploadFile = File(...)):
    # For now, just acknowledge the upload
    contents = await file.read()
    size = len(contents)
    return {"filename": file.filename, "size": size}


@app.websocket('/ws/stt')
async def websocket_stt(websocket: WebSocket):
    await websocket.accept()
    if not VOSK_MODEL:
        await websocket.send_json({"error": "Vosk model not configured on server."})
        await websocket.close()
        return

    # Assumes client sends 16-bit PCM mono at 16000 Hz
    recognizer = KaldiRecognizer(VOSK_MODEL, 16000)

    try:
        while True:
            data = await websocket.receive_bytes()
            if recognizer.AcceptWaveform(data):
                result = json.loads(recognizer.Result())
                if result.get("text"):
                    # Send the final transcript
                    await websocket.send_json(
                        {"transcript": result["text"], "is_final": True}
                    )
                    # Also send an event to signal the client to stop and process
                    await websocket.send_json({"event": "utterance_end"})
            else:
                partial_result = json.loads(recognizer.PartialResult())
                if partial_result.get("partial"):
                    await websocket.send_json({
                        "transcript": partial_result["partial"],
                        "is_final": False
                    })
    except WebSocketDisconnect:
        print("[ws/stt] Client disconnected.")
    except Exception as e:
        print(f"[ws/stt] Error: {e}")
