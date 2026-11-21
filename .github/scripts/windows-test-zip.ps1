# Assemble and test a portable Windows ZIP for development CI or GitHub Releases.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$name = if ($env:PACKAGE_NAME) { $env:PACKAGE_NAME } else { 'RawNode-Windows-x64-Release' }
$workspace = (Get-Location).Path
$output = Join-Path $env:RUNNER_TEMP 'rawnode-windows-artifact'
$stage = Join-Path $env:RUNNER_TEMP "rawnode-windows-stage/$name"
$extracted = Join-Path $env:RUNNER_TEMP 'RawNode isolated extraction'
$hiddenWorkspace = "$workspace.unavailable"
New-Item -ItemType Directory -Path $output, $stage | Out-Null

Copy-Item build/Release/OfxRawHost.exe, build/Release/fa-solid-900.ttf $stage
Copy-Item build/Plugins, build/RawDependencyNotices $stage -Recurse
# vcpkg's bin contains Release DLLs; debug/bin is deliberately excluded.
Copy-Item native-deps/vcpkg/x64-windows/bin/*.dll $stage
Get-ChildItem build -Recurse -Filter *.dll |
    Where-Object { $_.DirectoryName -match '[\\/]Release($|[\\/])' } |
    Copy-Item -Destination $stage

# Include app-local Visual C++ and OpenMP runtimes from VS's redistributables.
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$vs) { throw 'Visual Studio installation not found' }
$redistVersion = (Get-Content "$vs/VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt").Trim()
$redist = "$vs/VC/Redist/MSVC/$redistVersion/x64"
Copy-Item "$redist/Microsoft.VC143.CRT/*.dll", "$redist/Microsoft.VC143.OpenMP/*.dll" $stage
$toolsVersion = (Get-Content "$vs/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt").Trim()
$dumpbin = "$vs/VC/Tools/MSVC/$toolsVersion/bin/Hostx64/x64/dumpbin.exe"

Copy-Item LICENSE $stage
$notices = New-Item -ItemType Directory "$stage/ThirdPartyNotices"
foreach ($dependency in 'ctl', 'glfw', 'imgui') {
    $destination = New-Item -ItemType Directory "$notices/$dependency"
    Copy-Item "build/_deps/$dependency-src/LICENSE*" $destination
}
Copy-Item third_party/fontawesome/README.md "$notices/FontAwesome.txt"
Copy-Item third_party/fontawesome/LICENSE.txt "$notices/FontAwesome-LICENSE.txt"
Copy-Item third_party/openfx/LICENSE.md "$notices/OpenFX.md"
Copy-Item third_party/openfx/Support/LICENSE "$notices/OpenFX-Support.txt"
# Preserve TinyEXR's own copyright and its embedded OpenEXR notices verbatim.
Copy-Item third_party/tinyexr/tinyexr.h "$notices/TinyEXR.h"
Get-ChildItem native-deps/vcpkg/x64-windows/share -Recurse -Filter copyright | ForEach-Object {
    Copy-Item $_.FullName "$notices/$($_.Directory.Name)-copyright.txt"
}
$commit = git rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify build commit' }
$source = "$env:GITHUB_SERVER_URL/$env:GITHUB_REPOSITORY/tree/$commit"
if ($env:RELEASE_VERSION) {
    @"
RawNode $env:RELEASE_VERSION for Windows x64
Source and build instructions: $source

Extract the entire ZIP and launch OfxRawHost.exe from this folder.
Windows 10 version 1903 or later and an OpenGL 3.3-capable driver are required.
JPEG XL DNG support is OFF. The application is unsigned.
Licence notices are in LICENSE, RawDependencyNotices and ThirdPartyNotices.
"@ | Set-Content "$stage/README.txt"
} else {
    @"
RawNode Windows x64 Release - manual validation test build
Commit: $commit
CI run: https://github.com/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID

Extract the entire ZIP and launch OfxRawHost.exe from this folder.
Windows 10 version 1903 or later and an OpenGL 3.3-capable driver are required.
JPEG XL DNG support is OFF, as in the existing Windows CI configuration.
Windows Move to Trash requests native recycling. Windows may offer explicit
permanent-deletion confirmation; RawNode never approves it automatically.
Validate Trash only with disposable photographs. Settings use %APPDATA%\RawNode.
The default text font is embedded; fa-solid-900.ttf supplies the UI icons.
This is an unsigned test build, with no installer.

CI verifies the extracted application with its full --selftest, with the
original checkout renamed and PATH limited to Windows system directories.
Interactive desktop behaviour still needs manual validation.
"@ | Set-Content "$stage/TEST-BUILD.txt"
}
"RawNode source and build instructions: $source" | Set-Content "$stage/RawDependencyNotices/Source.txt"

$zip = Join-Path $output "$name.zip"
Compress-Archive -Path $stage -DestinationPath $zip
Expand-Archive -LiteralPath $zip -DestinationPath $extracted
$app = Join-Path $extracted $name
foreach ($required in 'OfxRawHost.exe', 'fa-solid-900.ttf', 'vcruntime140.dll', 'msvcp140.dll',
    'vcomp140.dll', 'RawDependencyNotices/LibRaw/LICENSE.CDDL',
    'RawDependencyNotices/DISTRIBUTION_NOTICES.md', 'RawDependencyNotices/Source.txt', 'Plugins/OfxRawHost.ofx.bundle/Contents/Win64/OfxRawHost.ofx',
    'Plugins/OfxRawHost.ofx.bundle/Contents/Info.plist') {
    if (!(Test-Path "$app/$required" -PathType Leaf)) { throw "Missing runtime resource: $required" }
}

# Audit every PE's architecture and imports, including the OFX module. Required
# non-system DLLs must resolve inside the extracted ZIP, without a build PATH.
Get-ChildItem $app -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll', '.ofx' } | ForEach-Object {
    $headers = & $dumpbin /headers $_.FullName
    if ($LASTEXITCODE -ne 0 -or !($headers -match '8664 machine \(x64\)')) {
        throw "Not a Windows x64 PE: $($_.FullName)"
    }
    $imports = & $dumpbin /dependents $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect imports: $($_.FullName)" }
    foreach ($line in $imports) {
        if ($line -match '^\s+([\w.-]+\.dll)\s*$') {
            $dll = $Matches[1]
            if ($dll -match '^(api-ms-|ext-ms-)') { continue }
            if (!(Test-Path "$app/$dll") -and !(Test-Path "$env:SystemRoot/System32/$dll")) {
                throw "Unbundled dependency $dll required by $($_.Name)"
            }
        }
    }
}

$savedPath = $env:PATH
$savedOfx = $env:OFX_PLUGIN_PATH
$savedCtl = $env:CTL_MODULE_PATH
$savedProcessDirectory = [Environment]::CurrentDirectory
try {
    Set-Location $app
    # Set-Location alone leaves the process's directory handle on the checkout,
    # which prevents Windows from renaming that directory.
    [Environment]::CurrentDirectory = $app
    # Hide sources, build outputs, vcpkg and the compiled-in source font path.
    Move-Item -LiteralPath $workspace -Destination $hiddenWorkspace
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    $env:OFX_PLUGIN_PATH = $null
    $env:CTL_MODULE_PATH = $null
    $process = Start-Process -FilePath "$app/OfxRawHost.exe" -ArgumentList '--selftest' `
        -WorkingDirectory $app -PassThru `
        -RedirectStandardOutput "$output/extracted-selftest.stdout.log" `
        -RedirectStandardError "$output/extracted-selftest.stderr.log"
    # Keep the process handle so ExitCode remains available after it exits.
    $null = $process.Handle
    if (!$process.WaitForExit(120000)) {
        $process.Kill()
        throw 'Extracted application self-test timed out'
    }
    $process.WaitForExit()
    Get-Content "$output/extracted-selftest.stdout.log", "$output/extracted-selftest.stderr.log"
    if ($process.ExitCode -ne 0) { throw "Extracted application failed: $($process.ExitCode)" }
    "PASS: extracted x64 application completed full --selftest with original checkout unavailable and system-only PATH." |
        Set-Content "$output/verification.txt"
} finally {
    $env:PATH = $savedPath
    $env:OFX_PLUGIN_PATH = $savedOfx
    $env:CTL_MODULE_PATH = $savedCtl
    if (Test-Path $hiddenWorkspace) { Move-Item -LiteralPath $hiddenWorkspace -Destination $workspace }
    Set-Location $workspace
    [Environment]::CurrentDirectory = $savedProcessDirectory
}
Get-FileHash -Algorithm SHA256 $zip | Format-List | Out-String | Set-Content "$output/SHA256.txt"
Get-Content "$output/SHA256.txt"
