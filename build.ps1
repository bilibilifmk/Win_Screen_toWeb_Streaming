$ErrorActionPreference = "Stop"

# 清理旧输出
$cleanDirs = @("dist", "publish", "publish_new", "publish_new2", "publish_new3", "publish_new4", "publish_small", "publish_standalone", "bin", "obj", "_tmp_standalone", "_tmp_small", ".tools")
foreach ($d in $cleanDirs) {
    if (Test-Path $d) { Remove-Item $d -Recurse -Force }
}

Write-Host "[1/3] 还原依赖..."
dotnet restore
if ($LASTEXITCODE -ne 0) { throw "dotnet restore failed" }

Write-Host "[2/3] 发布独立版（自包含，无需 .NET）..."
dotnet publish -c Release -r win-x64 --self-contained true /p:PublishSingleFile=true /p:IncludeNativeLibrariesForSelfExtract=true /p:EnableCompressionInSingleFile=true /p:DebugType=none /p:DebugSymbols=false -o _tmp_standalone
if ($LASTEXITCODE -ne 0) { throw "standalone publish failed" }

Write-Host "[3/3] 发布精简版（需已安装 .NET 8）..."
dotnet publish -c Release -r win-x64 --self-contained false /p:PublishSingleFile=true /p:EnableCompressionInSingleFile=false /p:DebugType=none /p:DebugSymbols=false -o _tmp_small
if ($LASTEXITCODE -ne 0) { throw "small publish failed" }

New-Item -ItemType Directory -Path "dist" -Force | Out-Null
Copy-Item "_tmp_standalone\obs.exe" "dist\obs-standalone.exe" -Force
Copy-Item "_tmp_small\obs.exe" "dist\obs-small.exe" -Force

Remove-Item "_tmp_standalone" -Recurse -Force
Remove-Item "_tmp_small" -Recurse -Force

$s1 = (Get-Item "dist\obs-standalone.exe").Length / 1MB
$s2 = (Get-Item "dist\obs-small.exe").Length / 1MB
Write-Host ""
Write-Host "完成:"
Write-Host ("  dist\obs-standalone.exe  {0:N1} MB (自包含)" -f $s1)
Write-Host ("  dist\obs-small.exe       {0:N1} MB (需 .NET)" -f $s2)
