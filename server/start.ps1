# Vox Memo — start server + USB sync watcher
# Run from the server/ directory: pwsh start.ps1

Write-Host "Starting Vox Memo server..." -ForegroundColor Green
docker compose up -d --build

Write-Host "Starting USB sync watcher..." -ForegroundColor Green
Write-Host "Press Ctrl+C to stop" -ForegroundColor DarkGray
uv run usb_sync.py --watch
