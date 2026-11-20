# Native integration test of the exact production adapter. Only new disposable
# VHDs and uniquely named fixtures are used. No dialog is ever approved.
$ErrorActionPreference = 'Stop'
$evidence = Join-Path $env:RUNNER_TEMP 'rawnode-trash-evidence'
New-Item -ItemType Directory -Force $evidence | Out-Null
$volumes = [Collections.Generic.List[object]]::new()
$observations = [Collections.Generic.List[object]]::new()
$rootName = 'RawNodeTrashTest-' + [Guid]::NewGuid().ToString('N') + '-相片'

function New-TestVolume([string]$Letter, [int]$Nuke, [int]$Capacity) {
    if (Get-Volume -DriveLetter $Letter -ErrorAction SilentlyContinue) { throw "Refuse existing drive $Letter" }
    $image = Join-Path $evidence "$Letter.vhd"
    $script = Join-Path $evidence "diskpart-$Letter.txt"
    @"
create vdisk file="$image" maximum=128 type=expandable
select vdisk file="$image"
attach vdisk
convert mbr
create partition primary
format fs=NTFS quick label=RAWNODE
assign letter=$Letter
"@ | Set-Content -LiteralPath $script -Encoding ascii
    & diskpart.exe /s $script | Set-Content (Join-Path $evidence "diskpart-$Letter.log")
    $disk = Get-DiskImage -ImagePath $image | Get-Disk
    $partition = Get-Partition -DriveLetter $Letter
    $volume = Get-Volume -DriveLetter $Letter
    if ($partition.DiskNumber -ne $disk.Number -or $volume.FileSystemLabel -ne 'RAWNODE' -or $volume.FileSystem -ne 'NTFS') {
        throw 'Test-volume identity failed; no bin settings will be changed'
    }
    $guid = [regex]::Match($volume.UniqueId, '\{[^}]+\}').Value
    if (!$guid) { throw 'No test-volume GUID' }
    # Fixture configuration only, scoped to the newly created volume GUID.
    # The production adapter never inspects the registry or bin capacity.
    $key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\BitBucket\Volume\$guid"
    New-Item -Path $key -Force | Out-Null
    New-ItemProperty -Path $key -Name NukeOnDelete -Value $Nuke -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $key -Name MaxCapacity -Value $Capacity -PropertyType DWord -Force | Out-Null
    $folder = New-Item -ItemType Directory -Path "$Letter`:\$rootName"
    $volumes.Add([ordered]@{ image=$image; settings=$key; folder=$folder.FullName; nuke=$Nuke; capacityMB=$Capacity })
    return $folder.FullName
}

function Find-BinItem([string]$Id) {
    $shell = New-Object -ComObject Shell.Application
    $bin = $shell.NameSpace(10)
    if (!$bin) { throw 'Native Recycle Bin namespace unavailable' }
    return @($bin.Items() | Where-Object { $_.Name -like "*$Id*" })
}

function Test-Fixture([string]$Folder, [string]$Scenario, [string]$Suffix, [int]$Size, [string]$Mode, [string]$Expected) {
    $id = [Guid]::NewGuid().ToString('N')
    $source = Join-Path $Folder "RawNodeTrashFixture-$id-$Suffix"
    $hash = $null
    if ($Scenario -ne 'missing') {
        $data = [byte[]]::new($Size)
        [Random]::new(42).NextBytes($data)
        [IO.File]::WriteAllBytes($source, $data)
        $hash = (Get-FileHash -LiteralPath $source).Hash
    }
    $report = Join-Path $evidence "$id.json"
    $record = [ordered]@{ scenario=$Scenario; source=$source; originalSha256=$hash; expected=$Expected; mode=$Mode }
    try {
        $process = Start-Process -FilePath (Join-Path $pwd 'WindowsTrashTests.exe') -PassThru -ArgumentList @(
            "`"$source`"", "`"$report`"", $Mode, 'RAWNODE_DISPOSABLE_FIXTURE')
        if (!$process.WaitForExit(20000)) {
            $process.Kill(); $process.WaitForExit()
            throw 'Native call/dialog timed out; result is inconclusive'
        }
        if ($process.ExitCode -ne 0) { throw "Fixture driver failed: $($process.ExitCode)" }
        $record.native = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
        $record.originalExists = Test-Path -LiteralPath $source
        $found = @(Find-BinItem $id)
        $record.binEntries = $found.Count
        if ($record.native.outcome -ne $Expected) { throw "Expected $Expected, got $($record.native.outcome): $($record.native.error)" }
        if ($Expected -eq 'trashed') {
            if ($record.originalExists -or $found.Count -ne 1 -or $record.native.declined) { throw 'Recycling disposition did not match bin/source state' }
            $from = [string]$found[0].ExtendedProperty('System.Recycle.DeletedFrom')
            if ($from.TrimEnd('\') -ne $Folder.TrimEnd('\')) { throw 'Bin original-path metadata differs' }
            $found[0].InvokeVerb('undelete')
            for ($attempt=0; $attempt -lt 30 -and !(Test-Path -LiteralPath $source); ++$attempt) { Start-Sleep -Milliseconds 200 }
            $record.restoredHashMatches = (Get-FileHash -LiteralPath $source).Hash -eq $hash
            $record.binEntriesAfterRestore = @(Find-BinItem $id).Count
            if (!$record.restoredHashMatches -or $record.binEntriesAfterRestore -ne 0) { throw 'Native restoration failed byte/path checks' }
        } elseif ($Scenario -ne 'missing') {
            $record.originalHashMatches = $record.originalExists -and (Get-FileHash -LiteralPath $source).Hash -eq $hash
            if (!$record.originalHashMatches -or $found.Count -ne 0) { throw 'Declined/failed operation did not preserve the original' }
            if ($Expected -eq 'cancelled') {
                $dialogText = $record.native.dialogs -join ' '
                if (!$record.native.declined -or $dialogText -notmatch 'permanently delete') {
                    throw 'No explicit permanent-deletion warning was observed and declined'
                }
            }
        } elseif ($record.originalExists -or $found.Count) { throw 'Missing-path call changed the filesystem' }
        $record.passed = $true
    } catch {
        $record.passed = $false
        $record.error = $_.Exception.Message
    }
    $observations.Add($record)
    $observations.ToArray() | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'observations.json') -Encoding utf8
    Write-Output "$Scenario`: passed=$($record.passed) $($record.error)"
}

try {
    $os = Get-CimInstance Win32_OperatingSystem
    [ordered]@{
        commit=(git rev-parse HEAD); os=$os.Caption; build=$os.BuildNumber
        run="https://github.com/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID"
        adapterSha256=(Get-FileHash src/platform/FileTrash.cpp).Hash
        hardware='Fixed virtual NTFS volumes; physical external/removable media and Windows 10/11 require manual validation'
    } | ConvertTo-Json | Set-Content (Join-Path $evidence 'environment.json') -Encoding utf8
    $enabled = New-TestVolume 'R' 0 64
    $disabled = New-TestVolume 'S' 1 64
    $tiny = New-TestVolume 'T' 0 1
    Test-Fixture $enabled 'ordinary-raw' 'été.dng' 12582912 'no' 'trashed'
    Test-Fixture $enabled 'current-sidecar' 'été.dng.rawnode.json' 127 'no' 'trashed'
    Test-Fixture $enabled 'legacy-sidecar' 'été.dng.ofxrawhost.json' 83 'no' 'trashed'
    Test-Fixture $disabled 'disabled-bin' 'photo.dng' 4096 'no' 'cancelled'
    Test-Fixture $tiny 'oversized-no' 'photo.dng' 12582912 'no' 'cancelled'
    Test-Fixture $tiny 'oversized-close' 'photo.dng' 12582912 'close' 'cancelled'
    Test-Fixture $enabled 'locked' 'photo.dng' 4096 'locked' 'failed'
    Test-Fixture $enabled 'missing' 'missing.dng' 0 'no' 'failed'
    if ($observations.Count -ne 8 -or @($observations | Where-Object { !$_.passed }).Count) {
        throw 'Native Trash checks failed; inspect observations.json and individual reports'
    }
} finally {
    $volumes.ToArray() | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $evidence 'volumes.json') -Encoding utf8
    foreach ($volume in $volumes) {
        Dismount-DiskImage -ImagePath $volume.image -ErrorAction Continue
        Remove-Item -LiteralPath $volume.settings -Recurse -ErrorAction Continue
    }
}
