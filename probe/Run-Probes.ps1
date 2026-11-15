param([ValidateSet('Prepare','Run')][string]$Phase)
$ErrorActionPreference = 'Stop'
$baseline = '8eb251a9827133bd22e7e756070114e8e50c5949'
$evidence = Join-Path $env:RUNNER_TEMP 'RawNodeTrashEvidence'
$backupRoot = Join-Path $evidence 'backups'
New-Item -ItemType Directory -Force $backupRoot,results | Out-Null

function Save-Json($Value,[string]$Path) { $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding utf8 }
function New-Bmp([string]$Path,[int]$Width,[int]$Height,[int]$Seed) {
    $row = (($Width * 3 + 3) -band (-bnot 3))
    $size = 54 + $row * $Height
    $bytes = [byte[]]::new($size)
    [Random]::new($Seed).NextBytes($bytes)
    [Array]::Clear($bytes,0,54)
    $bytes[0] = 66; $bytes[1] = 77
    [Array]::Copy([BitConverter]::GetBytes([int]$size),0,$bytes,2,4)
    [Array]::Copy([BitConverter]::GetBytes([int]54),0,$bytes,10,4)
    [Array]::Copy([BitConverter]::GetBytes([int]40),0,$bytes,14,4)
    [Array]::Copy([BitConverter]::GetBytes([int]$Width),0,$bytes,18,4)
    [Array]::Copy([BitConverter]::GetBytes([int]$Height),0,$bytes,22,4)
    [Array]::Copy([BitConverter]::GetBytes([uint16]1),0,$bytes,26,2)
    [Array]::Copy([BitConverter]::GetBytes([uint16]24),0,$bytes,28,2)
    [Array]::Copy([BitConverter]::GetBytes([int]($size-54)),0,$bytes,34,4)
    [IO.File]::WriteAllBytes($Path,$bytes)
}
function New-TestVolume([string]$Letter,[string]$FileSystem,[int]$Nuke,[int]$Capacity) {
    if (Get-Volume -DriveLetter $Letter -ErrorAction SilentlyContinue) { throw "Refuse existing drive $Letter" }
    $image = Join-Path $evidence "RawNode-$Letter.vhd"
    $script = Join-Path $evidence "diskpart-$Letter.txt"
    @"
create vdisk file="$image" maximum=128 type=expandable
select vdisk file="$image"
attach vdisk
convert mbr
create partition primary
format fs=$FileSystem quick label=RAWNODE
assign letter=$Letter
"@ | Set-Content -LiteralPath $script -Encoding ascii
    & diskpart.exe /s $script | Set-Content (Join-Path $evidence "diskpart-$Letter.log")
    $disk = Get-DiskImage -ImagePath $image | Get-Disk
    $partition = Get-Partition -DriveLetter $Letter
    $volume = Get-Volume -DriveLetter $Letter
    if ($partition.DiskNumber -ne $disk.Number -or $volume.FileSystemLabel -ne 'RAWNODE' -or $volume.FileSystem -ne $FileSystem) {
        throw 'Test-volume identity/filesystem validation failed; no policy will be changed'
    }
    $guid = [regex]::Match($volume.UniqueId,'\{[^}]+\}').Value
    if (!$guid) { throw 'No volume GUID; no policy will be changed' }
    $key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\BitBucket\Volume\$guid"
    New-Item -Path $key -Force | Out-Null
    New-ItemProperty -Path $key -Name NukeOnDelete -Value $Nuke -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $key -Name MaxCapacity -Value $Capacity -PropertyType DWord -Force | Out-Null
    $read = Get-ItemProperty -LiteralPath $key
    return [ordered]@{
        root = "$Letter`:\"; image = $image; disk = $disk.Number; fileSystem = $volume.FileSystem
        driveType = $volume.DriveType.ToString(); volumeGuid = $guid; registryKey = $key
        nukeOnDelete = $read.NukeOnDelete; maxCapacityMB = $read.MaxCapacity
        policyEvidence = 'per-test-volume registry configuration/readback; no GUI-policy assertion'
        hardware = 'fixed virtual disk; NOT physical external/removable media'
    }
}
function Bin-Matches([string]$Id) {
    $shell = New-Object -ComObject Shell.Application
    $bin = $shell.NameSpace(10)
    if (!$bin) { throw 'Native Recycle Bin namespace is unavailable' }
    $matches = @()
    foreach ($item in $bin.Items()) {
        if ($item.Name -like "*$Id*") {
            $matches += [pscustomobject]@{
                name = $item.Name; path = $item.Path
                deletedFrom = [string]$item.ExtendedProperty('System.Recycle.DeletedFrom')
                nativeItem = $item
            }
        }
    }
    return $matches
}

if ($Phase -eq 'Prepare') {
    $rootName = 'RawNodeTrashProbe-' + $env:GITHUB_RUN_ID + '-' + [Guid]::NewGuid().ToString('N')
    $scenarios = [Collections.Generic.List[object]]::new()
    $c = Get-Volume -DriveLetter C
    if ($c.FileSystem -ne 'NTFS') { throw 'System test directory is not NTFS' }
    $scenarios.Add([ordered]@{name='default-fixed-ntfs'; root='C:\'; fileSystem='NTFS'; driveType=$c.DriveType.ToString(); policyEvidence='existing settings unmodified'; hardware='cloud VM fixed system drive'})
    foreach ($spec in @(
        @('enabled-virtual-ntfs','R','NTFS',0,64),
        @('configured-disabled-virtual-ntfs','S','NTFS',1,64),
        @('configured-oversized-virtual-ntfs','T','NTFS',0,1),
        @('virtual-fixed-fat32','U','FAT32',0,64),
        @('virtual-fixed-exfat','V','exFAT',0,64))) {
        try {
            $volume = New-TestVolume $spec[1] $spec[2] $spec[3] $spec[4]
            $volume['name'] = $spec[0]; $scenarios.Add($volume)
        } catch { $scenarios.Add([ordered]@{name=$spec[0]; skipped=$true; error=$_.Exception.Message}) }
    }
    $cases = @(); $seed = 100
    foreach ($scenario in $scenarios) {
        if ($scenario.skipped) { continue }
        $folder = Join-Path $scenario.root $rootName
        New-Item -ItemType Directory -Path $folder | Out-Null
        foreach ($mode in @('production','instrumented','without-noconfirmation','force-veto-control')) {
            $id = [Guid]::NewGuid().ToString('N')
            $file = Join-Path $folder "RawNodeProbe-$id.bmp"
            $height = if ($scenario.name -eq 'configured-oversized-virtual-ntfs') { 16384 } else { 64 }
            New-Bmp $file 256 $height (++$seed)
            $backup = Join-Path $backupRoot "RawNodeProbe-$id.bmp"
            Copy-Item -LiteralPath $file -Destination $backup
            $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
            if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne $hash) { throw 'Independent backup did not verify' }
            $cases += [ordered]@{ id=$id; scenario=$scenario.name; mode=$mode; source=$file; backup=$backup; sha256=$hash; bytes=(Get-Item -LiteralPath $file).Length; volume=$scenario }
        }
    }
    $manifest = [ordered]@{
        baseline=$baseline; runId=$env:GITHUB_RUN_ID; diagnosticCommit=$env:GITHUB_SHA
        runUrl="https://github.com/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID"
        productionSourceSha256=(Get-FileHash baseline/src/platform/FileTrash.cpp -Algorithm SHA256).Hash
        independentBackups=$backupRoot; scenarios=$scenarios.ToArray(); cases=$cases
        physicalExternalHddSsd='UNVERIFIED: no attached hardware'; physicalRemovableSd='UNVERIFIED: no attached hardware'
    }
    Save-Json $manifest 'results/manifest.json'
    Copy-Item results/manifest.json (Join-Path $evidence 'manifest.json')
    Write-Output "Prepared and byte-verified $($cases.Count) independent disposable BMP fixtures. No Trash API called yet."
    exit 0
}

$manifest = Get-Content results/manifest.json -Raw | ConvertFrom-Json
$observations = @()
New-Item -ItemType Directory -Force results/callbacks | Out-Null
foreach ($case in $manifest.cases) {
    $observation = [ordered]@{ id=$case.id; scenario=$case.scenario; mode=$case.mode; source=$case.source; originalSha256=$case.sha256 }
    try {
        if (!(Test-Path -LiteralPath $case.source) -or (Get-FileHash -LiteralPath $case.backup).Hash -ne $case.sha256 -or (Get-FileHash -LiteralPath $case.source).Hash -ne $case.sha256) { throw 'Fixture/backup preflight failed; no API call allowed' }
        $trace = Join-Path (Get-Location) "results/callbacks/$($case.id).jsonl"
        $out = Join-Path $evidence "$($case.id).stdout"
        $err = Join-Path $evidence "$($case.id).stderr"
        $p = Start-Process -FilePath (Join-Path $evidence 'TrashProbe.exe') -ArgumentList @($case.mode,$case.source,$case.backup,$trace,'RAWNODE_DISPOSABLE_FIXTURE_ACK') -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
        if (!$p.WaitForExit(20000)) { $p.Kill(); $p.WaitForExit(); $observation.timeout=$true }
        $observation.exitCode = $p.ExitCode
        $observation.trace = @()
        if (Test-Path $trace) { $observation.trace = @(Get-Content $trace | ForEach-Object { $_ | ConvertFrom-Json }) }
        $observation.stderr = Get-Content $err -Raw -ErrorAction SilentlyContinue
        $observation.originalExistsAfterOperation = Test-Path -LiteralPath $case.source
        if ($observation.originalExistsAfterOperation) { $observation.originalIntactAfterOperation = (Get-FileHash -LiteralPath $case.source).Hash -eq $case.sha256 }
        $found = @()
        try {
            for ($attempt=0; $attempt -lt 6; ++$attempt) {
                $found = @(Bin-Matches $case.id)
                if ($found.Count -gt 0 -or $observation.originalExistsAfterOperation) { break }
                Start-Sleep -Milliseconds 500
            }
            $observation.binEnumerationSucceeded = $true
            $observation.binBeforeRestore = @($found | Select-Object name,path,deletedFrom)
        } catch { $observation.binEnumerationSucceeded=$false; $observation.binError=$_.Exception.Message }
        if (!$observation.originalExistsAfterOperation -and $found.Count -eq 1) {
            if ($found[0].deletedFrom.TrimEnd('\') -ne (Split-Path $case.source).TrimEnd('\')) { throw 'Bin original-path metadata did not match fixture; no restore invoked' }
            $observation.restoreVerbInvoked = 'undelete'
            $found[0].nativeItem.InvokeVerb('undelete')
            for ($attempt=0; $attempt -lt 30 -and !(Test-Path -LiteralPath $case.source); ++$attempt) { Start-Sleep -Milliseconds 500 }
            $observation.originalExistsAfterRestore = Test-Path -LiteralPath $case.source
            if ($observation.originalExistsAfterRestore) { $observation.restoreSha256Matches = (Get-FileHash -LiteralPath $case.source).Hash -eq $case.sha256 }
            $observation.binAfterRestore = @((Bin-Matches $case.id) | Select-Object name,path,deletedFrom)
        }
        $observation.backupStillVerified = (Get-FileHash -LiteralPath $case.backup).Hash -eq $case.sha256
    } catch { $observation.error=$_.Exception.Message }
    $observations += [pscustomobject]$observation
    Save-Json ([ordered]@{baseline=$baseline; runUrl=$manifest.runUrl; observations=$observations}) 'results/native-observations.json'
    Write-Output "$($case.scenario) / $($case.mode): original=$($observation.originalExistsAfterOperation) bin=$($found.Count) restoreHash=$($observation.restoreSha256Matches) timeout=$($observation.timeout) error=$($observation.error)"
}
