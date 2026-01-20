# Launch Mesen and capture ALL console output to a file
$logFile = "C:\Users\Surface Pro\Documents\mesen_debug.txt"
$mesenExe = "C:\Users\Surface Pro\Documents\source\repos\Mesen2\bin\win-x64\Release\Mesen.exe"

Write-Host "Starting Mesen..."
Write-Host "Log will be saved to: $logFile"
Write-Host ""
Write-Host "Instructions:"
Write-Host "1. Wait for Mesen to fully launch"
Write-Host "2. Load Sonic 2"
Write-Host "3. Start HD Pack Recording (4x)"
Write-Host "4. Play for 5 seconds"
Write-Host "5. Stop recording"
Write-Host "6. Close Mesen"
Write-Host "7. Open $logFile and copy the ENTIRE contents"
Write-Host ""

# Start Mesen and redirect all output to file
& $mesenExe *>&1 | Tee-Object -FilePath $logFile

Write-Host ""
Write-Host "Mesen closed. Log saved to: $logFile"
