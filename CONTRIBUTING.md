# Contributing

Thanks for wanting to contribute.

Getting started (development):

1. Backend (Python)
   - Create a virtual environment and install dependencies:

```powershell
python -m venv .venv; .\.venv\Scripts\Activate.ps1; pip install -r backend\requirements.txt
```

   - Run the backend:

```powershell
uvicorn backend.app:app --reload
```

2. Client (Electron)
   - From `client/electron-app` run:

```powershell
cd client\electron-app
npm install
npm run start
```

Design notes:
- This project emphasizes explicit consent and visible UI for any audio or screen capture.
- Create issues for new features and link implementation to milestones in the repo.

Please follow code style and open a PR for non-trivial changes.