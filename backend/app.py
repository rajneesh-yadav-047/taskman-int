from fastapi import FastAPI, UploadFile, File, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse
import json
import asyncio
import os
import httpx
from dotenv import load_dotenv

# Load local backend/.env if present (dev convenience). Do NOT commit backend/.env.
load_dotenv(dotenv_path=os.path.join(os.path.dirname(__file__), '.env'))

app = FastAPI(title="taskman-int-backend")


@app.on_event("startup")
def startup_event():
    provider = os.getenv("TASKMAN_API_PROVIDER", "auto")
    model = os.getenv("GEMINI_MODEL", os.getenv("OPENAI_MODEL", "(none)"))
    key = os.getenv("TASKMAN_API_KEY")
    masked = None
    if key:
        # show only first/last chars to avoid leaking secrets in logs
        masked = key[:5] + "..." + key[-4:]
    print(f"[startup] TASKMAN_API_PROVIDER={provider} GEMINI/OPENAI_MODEL={model} TASKMAN_API_KEY={'set' if key else 'MISSING'} ({masked})")


@app.get('/api/ping')
async def ping():
    return {"ok": True}


@app.post('/api/llm')
async def llm_query(payload: dict):
    """Proxy endpoint to OpenAI Chat Completions.

    Expects JSON with one of the keys: 'q', 'question', 'prompt'.
    Reads the OpenAI key from environment variable TASKMAN_API_KEY.
    Returns JSON: {"answer": "<assistant reply>"}.
    """
    OPENAI_KEY = os.getenv("TASKMAN_API_KEY")
    if not OPENAI_KEY:
        return JSONResponse(status_code=500, content={"error": "TASKMAN_API_KEY not set on server"})

    # Accept several common payload field names
    prompt = payload.get('q') or payload.get('question') or payload.get('prompt') or payload.get('query')
    if not prompt:
        return JSONResponse(status_code=400, content={"error": "no prompt provided (send 'q' or 'question')"})

    # Support multiple providers. If the API key looks like a Google API key (starts with 'AIza')
    # or if TASKMAN_API_PROVIDER=gemini, send the request to Google's Generative Models API (Gemini).
    provider = os.getenv("TASKMAN_API_PROVIDER", "auto").lower()
    use_gemini = provider == "gemini" or (OPENAI_KEY.startswith("AIza") and provider == "auto")

    try:
        async with httpx.AsyncClient(timeout=30.0) as client:
            if use_gemini:
                # Gemini / Google Generative Language API (v1beta2).
                # Be tolerant with GEMINI_MODEL: allow values like "text-bison-001" or "models/text-bison-001".
                gemini_model = os.getenv("GEMINI_MODEL", "models/text-bison-001")
                gemini_model = gemini_model.strip().lstrip('/')
                if not gemini_model.startswith("models/"):
                    gemini_model = "models/" + gemini_model
                url = f"https://generativelanguage.googleapis.com/v1beta2/{gemini_model}:generateText"

                # Support either API key (key=...) or OAuth bearer token depending on the value provided.
                headers = {"Content-Type": "application/json"}
                params = None
                if OPENAI_KEY.startswith("AIza"):
                    # API key style
                    params = {"key": OPENAI_KEY}
                else:
                    # Treat as bearer token (e.g. 'ya29....') and send Authorization header
                    headers["Authorization"] = f"Bearer {OPENAI_KEY}"

                body = {
                    "prompt": {"text": prompt},
                    "temperature": float(os.getenv("GEMINI_TEMPERATURE", "0.0")),
                    "maxOutputTokens": int(os.getenv("GEMINI_MAX_TOKENS", "512")),
                }

                # Log the outgoing request parameters for easier debugging in dev (printed to uvicorn stdout)
                print(f"[proxy] Gemini request -> URL: {url} params={params} headers_keys={list(headers.keys())} maxTokens={body['maxOutputTokens']}")

                resp = await client.post(url, params=params, headers=headers, json=body)
                if resp.status_code != 200:
                    # Include remote details to help diagnose 404s (model not found / permission issues)
                    print(f"[proxy] Gemini response status={resp.status_code} text={resp.text}")
                    return JSONResponse(status_code=502, content={"error": "Gemini API error", "details": resp.text, "url": url, "status": resp.status_code})
                data = resp.json()
                # Gemini returns candidates with 'output' or 'content' depending on version; try common keys
                answer = None
                try:
                    # v1beta2 often has 'candidates' list with 'output' or 'content'
                    if "candidates" in data and len(data["candidates"]) > 0:
                        cand = data["candidates"][0]
                        answer = cand.get("output") or cand.get("content") or cand.get("text")
                    # older variants use 'candidates'[0]['content'] or top-level 'output'
                    if not answer:
                        answer = data.get("output") or data.get("candidates", [])[0].get("content")
                except Exception:
                    answer = json.dumps(data)
                return JSONResponse({"answer": answer})
            else:
                # Default: OpenAI-compatible Chat Completions
                model = os.getenv("OPENAI_MODEL", "gpt-3.5-turbo")
                body = {
                    "model": model,
                    "messages": [{"role": "user", "content": prompt}],
                    "max_tokens": int(os.getenv("OPENAI_MAX_TOKENS", "512")),
                }
                headers = {"Authorization": f"Bearer {OPENAI_KEY}", "Content-Type": "application/json"}
                resp = await client.post("https://api.openai.com/v1/chat/completions", headers=headers, json=body)
                if resp.status_code != 200:
                    return JSONResponse(status_code=502, content={"error": "OpenAI API error", "details": resp.text})
                data = resp.json()
                answer = None
                try:
                    answer = data["choices"][0]["message"]["content"]
                except Exception:
                    answer = json.dumps(data)
                return JSONResponse({"answer": answer})
    except httpx.RequestError as e:
        return JSONResponse(status_code=502, content={"error": "request error", "details": str(e)})

@app.post('/api/upload_resume')
async def upload_resume(file: UploadFile = File(...)):
    # For now, just acknowledge the upload
    contents = await file.read()
    size = len(contents)
    return {"filename": file.filename, "size": size}


@app.websocket('/ws/stt')
async def websocket_stt(websocket: WebSocket):
    """Simple STT WebSocket stub.

    Client should send binary audio chunks. This stub reads incoming binary frames
    and replies with a small JSON transcript message for each chunk received.
    This is intentionally simple for local testing and prototyping.
    """
    await websocket.accept()
    try:
        while True:
            # receive_bytes will raise WebSocketDisconnect when closed
            data = await websocket.receive_bytes()
            # produce a tiny fake transcript based on bytes received
            transcript = f"(stub) received {len(data)} bytes"
            print(f"[ws/stt] received {len(data)} bytes")
            await websocket.send_json({"transcript": transcript})
            # small pause to simulate processing latency
            await asyncio.sleep(0.01)
    except WebSocketDisconnect:
        return
