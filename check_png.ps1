Add-Type -AssemblyName System.Drawing
$folder = 'C:\Users\Surface Pro\Documents\Mesen2\HdPacks\Sonic The Hedgehog 2 (Europe, Brazil) (En)'

function Check-PNG($filename) {
    $path = "$folder\$filename"
    if (-not (Test-Path $path)) { Write-Host "$filename NOT FOUND"; return }
    $img = [System.Drawing.Bitmap]::new($path)
    Write-Host "`n=== $filename : $($img.Width)x$($img.Height) ==="
    
    $coloredTiles = 0
    $blackTiles = 0
    $transparentTiles = 0
    $tilesPerRow = 16
    $tileSize = [math]::Max(1, $img.Width / $tilesPerRow)
    Write-Host "Tile size: ${tileSize}x${tileSize}"
    
    $maxTiles = [math]::Min(32, ($img.Width / $tileSize) * ($img.Height / $tileSize))
    
    for ($t = 0; $t -lt $maxTiles; $t++) {
        $tx = ($t % $tilesPerRow) * $tileSize
        $ty = [math]::Floor($t / $tilesPerRow) * $tileSize
        $hasColor = $false
        $hasTransparent = $false
        $hasBlack = $false
        $samplePx = $null
        
        for ($y = $ty; $y -lt ($ty + $tileSize); $y += [math]::Max(1, $tileSize/4)) {
            for ($x = $tx; $x -lt ($tx + $tileSize); $x += [math]::Max(1, $tileSize/4)) {
                $px = $img.GetPixel($x, $y)
                if ($px.A -eq 0) { $hasTransparent = $true }
                elseif ($px.R -eq 0 -and $px.G -eq 0 -and $px.B -eq 0) { $hasBlack = $true }
                else { 
                    $hasColor = $true
                    if ($null -eq $samplePx) { $samplePx = $px }
                }
            }
        }
        
        if ($hasColor) {
            $coloredTiles++
            if ($coloredTiles -le 3) {
                Write-Host "  Tile $t at ($tx,$ty): HAS COLOR - sample R=$($samplePx.R) G=$($samplePx.G) B=$($samplePx.B) A=$($samplePx.A)"
            }
        } elseif ($hasTransparent -and -not $hasBlack) {
            $transparentTiles++
        } else {
            $blackTiles++
        }
    }
    Write-Host "Summary (first $maxTiles tiles): colored=$coloredTiles, black=$blackTiles, transparent=$transparentTiles"
    $img.Dispose()
}

Check-PNG "SPRITES_000.png"
Check-PNG "SPRITES_001.png"
Check-PNG "BGTILES_000.png"
Check-PNG "BGTILES_001.png"
