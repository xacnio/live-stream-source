param(
    [string]$ObsVersion = "32.0.4",
    [string]$DepsVersion = "2025-08-23"
)

Write-Host "Installing Windows Dependencies for OBS Plugin..."

# 1. Download OBS Source (Headers)
Write-Host "Cloning OBS Studio Source (v$ObsVersion)..."
git clone --depth 1 --branch $ObsVersion https://github.com/obsproject/obs-studio.git C:\obs-studio-src

# 2. Download OBS Binaries (obs.dll etc)
Write-Host "Downloading OBS Studio Binaries (v$ObsVersion)..."
Invoke-WebRequest -Uri "https://github.com/obsproject/obs-studio/releases/download/$ObsVersion/OBS-Studio-$ObsVersion-Windows-x64.zip" -OutFile "obs-bin.zip"
Write-Host "Extracting OBS Studio Binaries..."
Expand-Archive "obs-bin.zip" -DestinationPath C:\obs-studio-bin

# 3. Download Windows Deps (obs-deps: FFmpeg, curl, etc)
Write-Host "Downloading Windows OBS Dependencies ($DepsVersion)..."
Invoke-WebRequest -Uri "https://github.com/obsproject/obs-deps/releases/download/$DepsVersion/windows-deps-$DepsVersion-x64.zip" -OutFile "windows-deps.zip"
Write-Host "Extracting Windows OBS Dependencies..."
Expand-Archive "windows-deps.zip" -DestinationPath C:\obs-deps

Remove-Item "windows-deps.zip"
Remove-Item "obs-bin.zip"

Write-Host "Windows Dependencies installation completed successfully."
