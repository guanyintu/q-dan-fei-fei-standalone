param([string]$Generator = 'Visual Studio 18 2026')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repoRoot '.tools\native-build'
$reports = Join-Path $repoRoot 'dist\standalone-reports'
New-Item -ItemType Directory -Path $reports -Force | Out-Null
& cmake -S $PSScriptRoot -B $build -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& cmake --build $build --config Release --parallel 4 2>&1 | Tee-Object -FilePath (Join-Path $reports 'build.log')
if ($LASTEXITCODE -ne 0) { throw 'Native build failed; close the previous standalone EXE if it is still running.' }
& ctest --test-dir $build -C Release --output-on-failure 2>&1 | Tee-Object -FilePath (Join-Path $reports 'ctest.log')
if ($LASTEXITCODE -ne 0) { throw 'Native test failure' }
& (Join-Path $build 'Release\QBounceNativeTests.exe') | Tee-Object -FilePath (Join-Path $reports 'test-cases.log')
if ($LASTEXITCODE -ne 0) { throw 'Native regression failure' }
& (Join-Path $build 'Release\QBouncePerfBench.exe') --assert-efficient | Tee-Object -FilePath (Join-Path $reports 'performance-after.log')
if ($LASTEXITCODE -ne 0) { throw 'Native frame/lifecycle regression failure' }
$projectText = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'CMakeLists.txt') -Raw
if ($projectText -notmatch 'project\(q弹狒狒 VERSION ([0-9.]+)') { throw 'Project version not found' }
$version = $Matches[1]
$package = Join-Path $repoRoot "dist\q弹狒狒-独立版-$version"
New-Item -ItemType Directory -Path $package -Force | Out-Null
$exe = Join-Path $package 'q弹狒狒.exe'
Copy-Item -LiteralPath (Join-Path $build "Release\q弹狒狒-$version.exe") -Destination $exe -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md'),(Join-Path $PSScriptRoot 'THIRD-PARTY-NOTICES.md') -Destination $package -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'vendor\minhook\LICENSE.txt') -Destination (Join-Path $package 'MinHook-LICENSE.txt') -Force
Get-FileHash -LiteralPath $exe -Algorithm SHA256 | Select-Object Hash | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $reports 'sha256.json') -Encoding utf8
Compress-Archive -LiteralPath $package -DestinationPath (Join-Path $repoRoot "dist\q弹狒狒-独立版-$version.zip") -Force
$sourceRoot = Join-Path $repoRoot ('.tools\standalone-source-' + [guid]::NewGuid().ToString('N'))
$sourceStandalone = Join-Path $sourceRoot 'standalone'
$sourceVendor = Join-Path $sourceStandalone 'vendor\minhook'
$sourceAssets = Join-Path $sourceRoot 'QBounce\Assets'
New-Item -ItemType Directory -Path $sourceStandalone,$sourceVendor,$sourceAssets -Force | Out-Null
Get-ChildItem -LiteralPath $PSScriptRoot -File | Copy-Item -Destination $sourceStandalone
foreach ($part in @('src','include','cmake','CMakeLists.txt','LICENSE.txt','README.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "vendor\minhook\$part") -Destination $sourceVendor -Recurse
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'QBounce\Assets\bgm.mp3'),(Join-Path $repoRoot 'QBounce\Assets\bgm-source.json') -Destination $sourceAssets
Compress-Archive -Path (Join-Path $sourceRoot '*') -DestinationPath (Join-Path $repoRoot "dist\q弹狒狒-独立版-$version-source.zip") -Force
Write-Output "Standalone EXE: $exe"
