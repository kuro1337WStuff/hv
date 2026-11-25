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

function Get-HvConsoleVerdict {
    param([Parameter(Mandatory)][AllowNull()]$Frame, [Parameter(Mandatory)][string]$Profile)

    # SHA-256 of the reviewed journal/images pixels, not the PNG file. Pack each
    # RGB pixel as little-endian R5:G6:B5 (R >> 3, G >> 2, B >> 3), top row first.
    # UNKNOWN may mean different text, layout, colours, or an unfinished boot;
    # inspect the actual capture. It is neither a firmware failure nor a pass.
    $expected = switch -Exact ($Profile) {
        'probe'     { 'a255bc97bb907bcbf88474a182d6010dae63b696b7d43253efc9325055f26fc1' } # uefi-probe-console.png
        'l2'        { '36bcc11f3dbf69b38bef1466e06eb42ae3fc6b23697b49ee1a947842635164c2' } # npt-005.png
        'cpuid'     { '27b75d52c2332aa952d1f256b59a4104d7cfa4022af87a9027f51b6c3f063cfc' } # npt-006.png
        'npt-write' { '4aceae05f48c8e62541b279e0124295daf6ded6147ece66c928493b42a78030c' } # npt-write-007.png
        'npt-remap' { 'b8a89c55ff3785983d41f30855c7af3e04fc0d5e87cb6122ab62a2c23affc061' } # npt-remap-008.png
        'npt-cross' { 'cb02560cd2e1b5326916326ffd46c3cf91e6cb6238e6153a453648dbb8d3de18' } # npt-cross-010.png
        default     { return 'UNKNOWN' }
    }
    if (!(Test-HvConsoleFrame $Frame)) { return 'UNKNOWN' }
    # ImageData can have a provider prefix. Only the complete pixel tail is evidence.
    $size = $Frame.Width * $Frame.Height * 2
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $digest = $sha.ComputeHash($Frame.ImageData, ($Frame.ImageData.Length - $size), $size) }
    finally { $sha.Dispose() }
    if ([BitConverter]::ToString($digest).Replace('-', '') -ne $expected) { return 'UNKNOWN' }
    if ($Profile -eq 'npt-cross') { return 'SKIP' } # No MP Services; no L2 entry.
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
