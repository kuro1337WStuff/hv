#Requires -Version 5.1

function Get-HvConsoleFrame {
    param([Parameter(Mandatory)][guid]$VmId)

    $namespace = 'root/virtualization/v2'
    $vm = Get-CimInstance -Namespace $namespace -ClassName Msvm_ComputerSystem -Filter "Name='$VmId'" -ErrorAction Stop
    if (!$vm) { throw "No virtual system for VM ID $VmId." }
    # SettingsDefineState identifies the live settings, not a checkpoint's display.
    $settings = @(Get-CimAssociatedInstance -InputObject $vm -Association Msvm_SettingsDefineState -ResultClassName Msvm_VirtualSystemSettingData -ErrorAction Stop)
    if ($settings.Count -ne 1) { throw 'Expected one current virtual system settings instance.' }
    $service = Get-CimInstance -Namespace $namespace -ClassName Msvm_VirtualSystemManagementService -ErrorAction Stop
    $result = Invoke-CimMethod -InputObject $service -MethodName GetVirtualSystemThumbnailImage -Arguments @{
        TargetSystem = $settings[0]; WidthPixels = [uint16]1024; HeightPixels = [uint16]768
    } -ErrorAction Stop
    if ($result.ReturnValue -ne 0) { throw "Hyper-V thumbnail returned $($result.ReturnValue)." }
    if ($result.ImageData -isnot [byte[]] -or $result.ImageData.Length -lt (1024 * 768 * 2)) { throw 'Incomplete RGB565 thumbnail.' }
    [pscustomobject]@{
        VmId = $VmId; CapturedUtc = [datetime]::UtcNow; Width = 1024; Height = 768; ImageData = $result.ImageData
    }
}

function Test-HvConsoleFrame {
    param($Frame)
    if ($null -eq $Frame) { return $false }
    if ($Frame -isnot [System.Collections.IDictionary]) {
        foreach ($field in 'Width', 'Height', 'ImageData') {
            if (!$Frame.PSObject.Properties[$field]) { return $false }
        }
    }
    return ($Frame.Width -eq 1024 -and $Frame.Height -eq 768 -and $Frame.ImageData -is [byte[]] -and $Frame.ImageData.Length -ge (1024 * 768 * 2))
}

# Accept only data that is safe to use in fixed ISO and evidence filenames.
function Read-HvProfiles {
    param([Parameter(Mandatory)][string]$Path)

    $stream = [IO.File]::Open($Path, 'Open', 'Read', 'Read')
    try {
        if ($stream.Length -gt 32768) { throw 'Profile manifest exceeds 32 KiB.' }
        $reader = [IO.StreamReader]::new($stream, [Text.UTF8Encoding]::new($false, $true), $false)
        try { $json = $reader.ReadToEnd().TrimStart([char]0xfeff).Trim() } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
    if (!$json.StartsWith('[')) { throw 'Expected a profile manifest array.' }
    $entries = ConvertFrom-Json -InputObject $json -ErrorAction Stop
    $count = @($entries).Count
    if ($null -eq $entries -or $count -lt 1 -or $count -gt 128) { throw 'Expected 1 to 128 manifest entries.' }
    $map = @{}
    $usedStems = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($entry in $entries) {
        if ($entry -isnot [pscustomobject]) { throw 'Expected a profile manifest object.' }
        $fields = @($entry.PSObject.Properties.Name)
        if ($fields.Count -ne 3 -or $fields -cnotcontains 'profile' -or
            $fields -cnotcontains 'stem' -or $fields -cnotcontains 'pass') { throw 'Invalid profile manifest fields.' }
        $name = $entry.profile
        if ($name -isnot [string] -or $name.Length -gt 48 -or $name -cnotmatch '\A[a-z][a-z0-9]*(?:-[a-z0-9]+)*\z' -or
            $name -cmatch '\A(?:con|prn|aux|nul|com[0-9]|lpt[0-9])\z' -or $map.ContainsKey($name)) {
            throw 'Invalid or duplicate profile name.'
        }
        if ($name -ceq 'probe') {
            if ($null -ne $entry.stem) { throw 'The probe profile is screen-only.' }
        } elseif ($entry.stem -isnot [string] -or $entry.stem.Length -gt 64 -or
            $entry.stem -cnotmatch '\Ahv-[a-z0-9]+(?:-[a-z0-9]+)*\z' -or !$usedStems.Add($entry.stem)) {
            throw 'Invalid or duplicate ISO stem.'
        }
        if ($null -ne $entry.pass -and ($entry.pass -isnot [string] -or $entry.pass -cnotmatch '\A[0-9a-fA-F]{64}\z')) {
            throw 'Expected a reviewed SHA-256 screen digest or null.'
        }
        $map.Add($name, $entry)
    }
    return $map
}

function Get-HvConsoleVerdict {
    param([Parameter(Mandatory)][AllowNull()]$Frame, [Parameter(Mandatory)][string]$Profile, [hashtable]$Profiles)

    # SHA-256 of the reviewed journal/images pixels, not the PNG file. Pack each
    # RGB pixel as little-endian R5:G6:B5 (R >> 3, G >> 2, B >> 3), top row first.
    # UNKNOWN may mean different text, layout, colours, or an unfinished boot;
    # inspect the actual capture. It is neither a firmware failure nor a pass.
    if (!(Test-HvConsoleFrame $Frame)) { return 'UNKNOWN' }
    if ($null -eq $Profiles) { $Profiles = Read-HvProfiles (Join-Path $PSScriptRoot 'hv-profiles.json') }
    if (!$Profiles.ContainsKey($Profile)) { return 'UNKNOWN' }
    $expected = $Profiles[$Profile].pass
    if (!$expected -and $Profile -ne 'npt-cross') { return 'UNKNOWN' }
    # ImageData can have a provider prefix. Only the complete pixel tail is evidence.
    $size = $Frame.Width * $Frame.Height * 2
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $digest = $sha.ComputeHash($Frame.ImageData, ($Frame.ImageData.Length - $size), $size) }
    finally { $sha.Dispose() }
    $actual = [BitConverter]::ToString($digest).Replace('-', '')
    # The original UEFI-only image may still be mounted: its reviewed screen is SKIP, never PASS.
    if ($Profile -eq 'npt-cross' -and $actual -eq 'cb02560cd2e1b5326916326ffd46c3cf91e6cb6238e6153a453648dbb8d3de18') { return 'SKIP' }
    if ($actual -ne $expected) { return 'UNKNOWN' }
    return 'PASS'
}

function Save-HvConsoleFrame {
    param([Parameter(Mandatory)]$Frame, [Parameter(Mandatory)][string]$Path)
    if (!(Test-HvConsoleFrame $Frame)) { throw 'Cannot save an incomplete or unsupported console frame.' }
    Add-Type -AssemblyName PresentationCore -ErrorAction Stop
    $stride = $Frame.Width * 2
    $size = $stride * $Frame.Height
    # WPF's array overload starts at byte zero. Strip any prefix for encoding only;
    # leave the captured bytes untouched and do not reverse the scan lines.
    $pixels = $Frame.ImageData
    if ($pixels.Length -gt $size) {
        $pixels = [byte[]]::new($size)
        [Buffer]::BlockCopy($Frame.ImageData, ($Frame.ImageData.Length - $size), $pixels, 0, $size)
    }
    $source = [Windows.Media.Imaging.BitmapSource]::Create($Frame.Width, $Frame.Height, 96, 96, [Windows.Media.PixelFormats]::Bgr565, $null, $pixels, $stride)
    $encoder = [Windows.Media.Imaging.PngBitmapEncoder]::new()
    $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($source))
    $file = [IO.File]::Open($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path), [IO.FileMode]::CreateNew)
    try { $encoder.Save($file) } finally { $file.Dispose() }
}
