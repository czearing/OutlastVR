param(
    [string]$BuildDirectory = 'build',
    [string]$OutputDirectory = 'dist',
    [string]$Version = '0.1.0'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') {throw 'Invalid release version'}
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(OutlastVR VERSION (\d+\.\d+\.\d+)') {throw 'Project version missing'}
if (($Version -split '-')[0] -ne $Matches[1]) {throw 'Release tag and CMake project version disagree'}
$commit = & git -C $root rev-parse HEAD 2>$null
if ($LASTEXITCODE -ne 0) {throw 'Packages must identify a committed source revision'}
$dirty = & git -C $root status --porcelain --untracked-files=normal
if ($LASTEXITCODE -ne 0 -or $dirty) {throw 'Commit source changes before creating a release package'}
$build = [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$stage = Join-Path $output "OutlastVR-$Version-win64"
$zip = "$stage.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip)) {throw 'Refusing to overwrite an existing package'}
New-Item -ItemType Directory -Path $stage -Force | Out-Null
& cmake --install $build --prefix $stage --component Runtime
if ($LASTEXITCODE -ne 0) {throw 'Runtime installation failed'}
$readme = Join-Path $stage 'README.md'
$readmeText = [IO.File]::ReadAllText($readme).Replace(
    'https://github.com/czearing/OutlastVR/blob/master/',
    "https://github.com/czearing/OutlastVR/blob/$($commit.Trim())/")
[IO.File]::WriteAllText($readme, $readmeText, [Text.UTF8Encoding]::new($false))
$expected = @('OutlastVRLauncher.exe','OutlastVRSinglePass.dll','d3d9.dll','openxr_loader.dll',
    'OutlastVR.ini','README.md','LICENSE','THIRD_PARTY_NOTICES.md',
    'licenses\MinHook.txt','licenses\OpenXR.txt','licenses\OpenXR-MIT.txt','licenses\JsonCpp.txt',
    'licenses\Winpthreads.txt')
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
$relative = @($files | ForEach-Object {$_.FullName.Substring($stage.Length + 1)})
if (@(Compare-Object ($expected | Sort-Object) ($relative | Sort-Object)).Count) {
    throw "Package contains missing or unexpected files: $($relative -join ', ')"
}
& python (Join-Path $root 'tools\verify_package.py') --config-only (Join-Path $stage 'OutlastVR.ini')
if ($LASTEXITCODE -ne 0) {throw 'Release must default to native full-view night vision'}
$manifest = [ordered]@{
    name = 'Outlast VR'
    version = $Version
    source_commit = $commit.Trim()
    physical_camcorder_default = $false
    files = @($files | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = $_.FullName.Substring($stage.Length + 1).Replace('\','/')
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding UTF8
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
$digest = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$digest  $([IO.Path]::GetFileName($zip))" | Set-Content -LiteralPath "$zip.sha256" -Encoding ASCII
Write-Output "Packaged $zip"
