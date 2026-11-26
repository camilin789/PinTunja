# PIN Tunja - Agrega el codigo de la app Android al repositorio (Windows PowerShell)
# Uso: powershell -ExecutionPolicy Bypass -File .\agregar_app.ps1
# Requisitos: Git instalado y sesion de GitHub (camilin789) en el PC.
$ErrorActionPreference = "Stop"
$app  = "C:\Users\HERNAN ARIAS\AndroidStudioProjects\PinTunja"
$dest = "$env:TEMP\PinTunja"
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
git clone https://github.com/camilin789/PinTunja.git $dest
robocopy $app "$dest\app" /E /XD build .gradle .idea captures .cxx "respaldo_*" /XF local.properties *.apk *.aab | Out-Null
Set-Location $dest
git add app
git commit -m "App Android PinTunja (Kotlin + Jetpack Compose)"
git push
Write-Host "Listo: https://github.com/camilin789/PinTunja"
