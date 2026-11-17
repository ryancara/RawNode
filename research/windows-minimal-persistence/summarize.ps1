# Research-only: condense probe JSON lines into one readable line per observation.
param([string]$Evidence, [string]$UserSid)
$names = @{ 'S-1-5-32-544' = 'BA'; 'S-1-5-18' = 'SY'; 'S-1-5-21-1000000001-1000000002-1000000003-1001' = 'FOREIGN' }
if ($UserSid) { $names[$UserSid] = 'U' }
function Short([string]$s) {
  if (-not $s) { return '' }
  foreach ($k in $names.Keys) { $s = $s.Replace($k, $names[$k]) }
  $s = [regex]::Replace($s, 'S-1-5-21-\d+-\d+-\d+-(\d+)', 'S-…-$1')
  return $s
}
function Pair($a, $b) { if ("$a" -eq "$b") { return "$(Short $a)" } else { return "$(Short $a)->$(Short $b)" } }
foreach ($file in Get-ChildItem -Path $Evidence -Filter *.jsonl | Sort-Object Name) {
  "===== $($file.Name) ====="
  foreach ($line in Get-Content -LiteralPath $file.FullName) {
    if (-not $line.StartsWith('{')) { "RAW $line"; continue }
    try { $r = $line | ConvertFrom-Json } catch { "RAW $line"; continue }
    if ($r.before) {
      $b = $r.before; $a = $r.after
      $err = if ($r.error) { ($r.error -replace '^.*?: ', '') } else { '' }
      if ($err.Length -gt 110) { $err = $err.Substring(0, 110) }
      $extra = ''
      if ($r.extraAfter) { $extra = " extra=[$(Pair $r.extraBefore.content $r.extraAfter.content) links $(Pair $r.extraBefore.links $r.extraAfter.links)]" }
      if ($r.control) { $extra += " controlDacl=$(Short $r.control.dacl)" }
      if ($r.heldReaderRead) { $extra += " held=$($r.heldReaderRead)" }
      "{0}|{1}|{2}|ok={3}|unch={4}|left={5}|content={6}|attr={7}|links={8}|owner={9}|group={10}|dacl={11}|label={12}|streams={13}{14}|err={15}" -f `
        $r.phase, $r.case, $r.variant, $r.ok, $r.unchanged, $r.leftovers, (Pair $b.content $a.content), (Pair $b.attributes $a.attributes),
        (Pair $b.links $a.links), (Pair $b.owner $a.owner), (Pair $b.group $a.group), (Pair $b.dacl $a.dacl), (Pair $b.label $a.label),
        (Pair $b.streams $a.streams), $extra, $err
    } elseif ($r.state) {
      $s = $r.state
      "inspect|{0}|exists={1}|owner={2}|dacl={3}|label={4}|sacl={5}|readError={6}|attr={7}" -f $r.path, $s.exists, (Short $s.owner), (Short $s.dacl), $s.label, $s.sacl, $s.readError, $s.attributes
    } else {
      Short $line
    }
  }
}
