# Build and Sign script targeting version folder E:\ToolProjects\EchoBridge\bin\Release\v1.0.0
# All comments and messages are in ASCII to prevent Windows PowerShell encoding parse crashes

Write-Host ">>> Step 1: Stopping running EchoBridge processes..." -ForegroundColor Cyan
$process = Get-Process -Name "EchoBridge" -ErrorAction SilentlyContinue
if ($process) {
    Stop-Process -Name "EchoBridge" -Force
    Start-Sleep -Seconds 1
}

Write-Host ">>> Step 2: Running dotnet publish (Lightweight / Framework-Dependent)..." -ForegroundColor Cyan

# Establish target output directory
$targetDir = "E:\ToolProjects\EchoBridge\bin\Release\v1.0.0"
if (Test-Path $targetDir) {
    Remove-Item -Path $targetDir -Recurse -Force | Out-Null
}
New-Item -ItemType Directory -Path $targetDir -Force | Out-Null

$publishDir = "E:\ToolProjects\EchoBridge\bin\Release\net10.0-windows10.0.19041.0\win-x64\publish"

# Lightweight Single File (Framework-Dependent): ~24MB, requires .NET 10 Desktop Runtime on target machine
dotnet publish -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true

if (-not (Test-Path "$publishDir\EchoBridge.exe")) {
    Write-Error "Build failed: EchoBridge.exe not found in publish directory!"
    exit 1
}

# Copy only the EXE, deliberately exclude PDB (debug symbols - not needed for release, leaks source structure)
Copy-Item -Path "$publishDir\EchoBridge.exe" -Destination "$targetDir\EchoBridge.exe" -Force

# Clean intermediate publish folder
Remove-Item -Path "E:\ToolProjects\EchoBridge\bin\Release\net10.0-windows10.0.19041.0\win-x64" -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ">>> Step 3: Applying Local Code Signature..." -ForegroundColor Cyan
try {
    $cert = Get-ChildItem -Path Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=EchoBridge Local Developer" } | Select-Object -First 1

    if (-not $cert) {
        Write-Host "No local code signing certificate found. Creating one under CurrentUser..." -ForegroundColor Yellow
        $cert = New-SelfSignedCertificate -Type CodeSigning -Subject "CN=EchoBridge Local Developer" -FriendlyName "EchoBridge Dev Certificate" -NotAfter (Get-Date).AddYears(10) -CertStoreLocation "Cert:\CurrentUser\My" -ErrorAction Stop

        if ($cert) {
            $tempCertPath = Join-Path $env:TEMP "echobridge_temp.cer"
            $certBytes = $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
            [System.IO.File]::WriteAllBytes($tempCertPath, $certBytes)

            Import-Certificate -FilePath $tempCertPath -CertStoreLocation "Cert:\CurrentUser\Root" -ErrorAction Stop | Out-Null
            Remove-Item -Path $tempCertPath -Force
            Write-Host "Local certificate created and trusted successfully." -ForegroundColor Green
        } else {
            throw "Failed to generate certificate object."
        }
    } else {
        Write-Host "Existing code signing certificate found." -ForegroundColor Green
    }

    $exePath = Join-Path $targetDir "EchoBridge.exe"
    Write-Host "Signing EchoBridge.exe in bin/Release/v1.0.0..." -ForegroundColor Cyan
    $signResult = Set-AuthenticodeSignature -FilePath $exePath -Certificate $cert -ErrorAction Stop
    if ($signResult.Status -eq "Valid") {
        Write-Host "Success: EchoBridge.exe code-signed!" -ForegroundColor Green
    } else {
        Write-Warning "EchoBridge.exe signature status: $($signResult.Status)."
    }
}
catch {
    Write-Warning "Failed to apply local digital signature: $_"
    Write-Host "Tip: To enable digital code signing, please run this script ONCE as Administrator (Right click PowerShell -> Run as Administrator)." -ForegroundColor Yellow
}

Write-Host ">>> Build and sign process completed successfully!" -ForegroundColor Green
