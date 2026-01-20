$mesenExe = ".\bin\win-x64\Release\Mesen.exe"
$logFile = ".\mesen_log.txt"

Write-Host "Starting Mesen..."
Write-Host "Log will be saved to: $logFile"

& $mesenExe > $logFile 2>&1

Write-Host "Mesen closed."
