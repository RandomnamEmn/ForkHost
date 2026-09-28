$ErrorActionPreference = "Stop"

$repo = "RandomnamEmn/ForkHost"
$asset = "ForkHost-windows-x64.zip"
$installDir = if ($env:LHC_INSTALL_DIR) {
    $env:LHC_INSTALL_DIR
} else {
    Join-Path $env:LOCALAPPDATA "ForkHost\bin"
}

if (-not [Environment]::Is64BitOperatingSystem) {
    throw "lhc: only 64-bit Windows is currently supported."
}

$tempDir = Join-Path ([IO.Path]::GetTempPath()) ("lhc-" + [Guid]::NewGuid().ToString("N"))
$archive = Join-Path $tempDir "lhc.zip"
$extracted = Join-Path $tempDir "extracted"
$url = "https://github.com/$repo/releases/latest/download/$asset"

try {
    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

    Write-Host "Downloading latest lhc release..."
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
    }
    catch {
        throw "lhc: failed to download $asset from the latest release. The latest release may predate the lhc CLI; publish a newer tagged release and retry."
    }

    Expand-Archive -Path $archive -DestinationPath $extracted -Force

    $lhc = Get-ChildItem -Path $extracted -Recurse -File -Filter "lhc.exe" |
        Select-Object -First 1

    if ($null -eq $lhc) {
        throw "lhc: the latest release archive does not contain lhc.exe."
    }

    New-Item -ItemType Directory -Force -Path $installDir | Out-Null
    Copy-Item -Force $lhc.FullName (Join-Path $installDir "lhc.exe")

    Get-ChildItem -Path $extracted -Recurse -File -Filter "*.pdb" |
        ForEach-Object {
            Copy-Item -Force $_.FullName (Join-Path $installDir $_.Name)
        }

    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $userPathEntries = @(
        ($userPath -split ";") |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )

    if ($userPathEntries -notcontains $installDir) {
        $newUserPath = if ([string]::IsNullOrWhiteSpace($userPath)) {
            $installDir
        } else {
            "$userPath;$installDir"
        }

        [Environment]::SetEnvironmentVariable("Path", $newUserPath, "User")
    }

    $currentPathEntries = @(
        ($env:Path -split ";") |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )

    if ($currentPathEntries -notcontains $installDir) {
        $env:Path = "$env:Path;$installDir"
    }

    Write-Host "Installed lhc to $(Join-Path $installDir 'lhc.exe')"
}
finally {
    if (Test-Path $tempDir) {
        Remove-Item -Recurse -Force $tempDir
    }
}
