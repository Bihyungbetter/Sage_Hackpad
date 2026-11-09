# Streams Spotify's current track to the Sage Hackpad's OLED.
# Reads Windows' media session (the same info as the volume flyout), so no Spotify login is needed.
#   powershell -ExecutionPolicy Bypass -File spotify_oled.ps1

. "$PSScriptRoot\hackpad_link.ps1"

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } |
    Select-Object -First 1

function Wait-WinRT($operation, [Type]$resultType) {
    $task = $asTask.MakeGenericMethod($resultType).Invoke($null, @($operation))
    $null = $task.Wait(-1)
    $task.Result
}

$manager = Wait-WinRT ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()) `
                      ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])

# The OLED font is ASCII only: strip accents and replace anything else.
function ConvertTo-OledText([string]$text) {
    if (-not $text) { return '' }
    $plain = -join ($text.Normalize([Text.NormalizationForm]::FormD).ToCharArray() |
        Where-Object { [Globalization.CharUnicodeInfo]::GetUnicodeCategory($_) -ne 'NonSpacingMark' })
    $plain = -join ($plain.ToCharArray() | ForEach-Object { if ([int]$_ -ge 32 -and [int]$_ -lt 127) { $_ } else { '?' } })
    if ($plain.Length -gt 64) { $plain = $plain.Substring(0, 64) }
    $plain
}

function Send-Text([byte]$command, [string]$text) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($text)
    $offset = 0
    do {
        $count = [Math]::Min(29, $bytes.Length - $offset)
        $packet = New-Object byte[] 32
        $packet[0] = $command; $packet[1] = $offset; $packet[2] = $bytes.Length
        if ($count -gt 0) { [Array]::Copy($bytes, $offset, $packet, 3, $count) }
        if (-not (Send-HackpadPacket $packet)) { return $false }
        $offset += 29
    } while ($offset -lt $bytes.Length)
    return $true
}

$lastText = ''
$lastTextSent = [DateTime]::MinValue
while ($true) {
    $status = 0; $pos = 0; $dur = 0; $title = ''; $artist = ''
    $session = $manager.GetSessions() | Where-Object { $_.SourceAppUserModelId -like '*Spotify*' } | Select-Object -First 1
    if ($session) {
        try {
            $props = Wait-WinRT ($session.TryGetMediaPropertiesAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties])
            $title = ConvertTo-OledText $props.Title
            $artist = ConvertTo-OledText $props.Artist
            $playback = $session.GetPlaybackInfo().PlaybackStatus.ToString()
            $timeline = $session.GetTimelineProperties()
            $position = $timeline.Position
            if ($playback -eq 'Playing') { $position += [DateTimeOffset]::Now - $timeline.LastUpdatedTime }
            $dur = [int][Math]::Min(65535, $timeline.EndTime.TotalSeconds)
            $pos = [int][Math]::Max(0, [Math]::Min($dur, $position.TotalSeconds))
            $status = if ($playback -eq 'Playing') { 2 } elseif ($title) { 1 } else { 0 }
        } catch { $status = 0 }
    }

    $text = "$title`n$artist"
    $ok = $true
    if ($text -ne $lastText -or ([DateTime]::Now - $lastTextSent).TotalSeconds -ge 10) {
        $ok = (Send-Text 1 $title) -and (Send-Text 2 $artist)
        if ($ok) { $lastText = $text; $lastTextSent = [DateTime]::Now }
    }
    if ($ok) {
        $ok = Send-HackpadPacket ([byte[]](3, $status, ($pos -shr 8), ($pos -band 255), ($dur -shr 8), ($dur -band 255)))
    }
    if (-not $ok) { $lastText = ''; Start-Sleep -Seconds 3 } else { Start-Sleep -Seconds 1 }
}
