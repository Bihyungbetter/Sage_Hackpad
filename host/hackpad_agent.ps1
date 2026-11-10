# Background agent for the Sage Hackpad's OLED screens: Spotify now playing, clock and PC stats.
# Spotify info comes from Windows' media session (the same info as the volume flyout), so no login is needed.
#   powershell -WindowStyle Hidden -ExecutionPolicy Bypass -File hackpad_agent.ps1

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

function Send-ClockAndStats([bool]$withStats) {
    $now = Get-Date
    $ok = Send-HackpadPacket ([byte[]](4, $now.Hour, $now.Minute, $now.Second, [int]$now.DayOfWeek, $now.Month, $now.Day))
    if ($ok -and $withStats) {
        $cpu = [int](Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average
        $os = Get-CimInstance Win32_OperatingSystem
        $ram = [int](100 - 100 * $os.FreePhysicalMemory / $os.TotalVisibleMemorySize)
        $ok = Send-HackpadPacket ([byte[]](5, [Math]::Min(100, $cpu), [Math]::Min(100, $ram)))
    }
    return $ok
}

# Counts key presses on every keyboard (laptop, external, ...) so the OLED pets can react.
# Only a running count leaves this class: no key codes or text are stored or sent.
if (-not ('KeyActivity' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class KeyActivity {
    delegate IntPtr HookProc(int code, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public int x, y; }
    [DllImport("user32.dll")] static extern IntPtr SetWindowsHookEx(int id, HookProc fn, IntPtr mod, uint thread);
    [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr hook, int code, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] static extern int GetMessage(out MSG msg, IntPtr hwnd, uint min, uint max);
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandle(string name);

    static readonly HookProc proc = OnKey;   // keep the delegate alive
    static readonly bool[] down = new bool[256];
    static int presses;

    public static int Presses { get { return Volatile.Read(ref presses); } }

    public static void Start() {
        var t = new Thread(() => {
            SetWindowsHookEx(13 /* WH_KEYBOARD_LL */, proc, GetModuleHandle(null), 0);
            MSG msg;
            while (GetMessage(out msg, IntPtr.Zero, 0, 0) > 0) { }
        });
        t.IsBackground = true;
        t.Start();
    }

    static IntPtr OnKey(int code, IntPtr wParam, IntPtr lParam) {
        if (code >= 0) {
            int vk = Marshal.ReadInt32(lParam) & 0xFF;
            int msg = wParam.ToInt32();
            if (msg == 0x100 || msg == 0x104) {          // key down: count it once, ignore auto-repeat
                if (!down[vk]) { down[vk] = true; Interlocked.Increment(ref presses); }
            } else if (msg == 0x101 || msg == 0x105) {   // key up
                down[vk] = false;
            }
        }
        return CallNextHookEx(IntPtr.Zero, code, wParam, lParam);
    }
}
'@
}
[KeyActivity]::Start()

$lastText = ''
$lastTextSent = [DateTime]::MinValue
$lastPresses = [KeyActivity]::Presses
$nextUpdate = [DateTime]::MinValue
$loop = 0
while ($true) {
    # Fast path: forward new key presses right away so Bongo Cat taps in time.
    $presses = [KeyActivity]::Presses
    if ($presses -ne $lastPresses) {
        $null = Send-HackpadPacket ([byte[]](6, [Math]::Min(255, $presses - $lastPresses)))
        $lastPresses = $presses
    }
    if ([DateTime]::Now -lt $nextUpdate) { Start-Sleep -Milliseconds 30; continue }

    $loop++
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
    if ($ok) {
        $ok = Send-ClockAndStats ($loop % 2 -eq 0)
    }
    if (-not $ok) { $lastText = ''; $nextUpdate = [DateTime]::Now.AddSeconds(3) } else { $nextUpdate = [DateTime]::Now.AddSeconds(1) }
}
