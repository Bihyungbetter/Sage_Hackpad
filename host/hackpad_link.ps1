# Raw HID link to the Sage Hackpad (QMK raw HID: usage page 0xFF60, usage 0x61).
# Dot-source this file, then call Send-HackpadPacket with up to 32 bytes.

if (-not ('HackpadHid' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class HackpadHid {
    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid InterfaceClassGuid; public int Flags; public IntPtr Reserved; }

    [StructLayout(LayoutKind.Sequential)]
    struct HIDP_CAPS {
        public ushort Usage, UsagePage, InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes, NumberInputButtonCaps, NumberInputValueCaps, NumberInputDataIndices,
                      NumberOutputButtonCaps, NumberOutputValueCaps, NumberOutputDataIndices,
                      NumberFeatureButtonCaps, NumberFeatureValueCaps, NumberFeatureDataIndices;
    }

    [DllImport("hid.dll")] static extern void HidD_GetHidGuid(out Guid guid);
    [DllImport("hid.dll")] static extern bool HidD_GetPreparsedData(SafeFileHandle h, out IntPtr data);
    [DllImport("hid.dll")] static extern bool HidD_FreePreparsedData(IntPtr data);
    [DllImport("hid.dll")] static extern int HidP_GetCaps(IntPtr data, ref HIDP_CAPS caps);
    [DllImport("setupapi.dll", SetLastError = true)] static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr e, IntPtr hwnd, int flags);
    [DllImport("setupapi.dll", SetLastError = true)] static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr info, ref Guid g, int index, ref SP_DEVICE_INTERFACE_DATA data);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set, ref SP_DEVICE_INTERFACE_DATA data, IntPtr detail, int size, out int required, IntPtr info);
    [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);

    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, SHARE_RW = 3, OPEN_EXISTING = 3;

    public static string FindRawPath() {
        Guid g; HidD_GetHidGuid(out g);
        IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, 0x12); // PRESENT | DEVICEINTERFACE
        try {
            var data = new SP_DEVICE_INTERFACE_DATA { cbSize = Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA)) };
            for (int i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref data); i++) {
                int size;
                SetupDiGetDeviceInterfaceDetail(set, ref data, IntPtr.Zero, 0, out size, IntPtr.Zero);
                IntPtr detail = Marshal.AllocHGlobal(size);
                try {
                    Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                    if (!SetupDiGetDeviceInterfaceDetail(set, ref data, detail, size, out size, IntPtr.Zero)) continue;
                    string path = Marshal.PtrToStringUni(new IntPtr(detail.ToInt64() + 4));
                    if (path.IndexOf("vid_feed&pid_0000", StringComparison.OrdinalIgnoreCase) < 0) continue;
                    using (var h = CreateFile(path, 0, SHARE_RW, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero)) {
                        if (h.IsInvalid) continue;
                        IntPtr pre;
                        if (!HidD_GetPreparsedData(h, out pre)) continue;
                        var caps = new HIDP_CAPS();
                        HidP_GetCaps(pre, ref caps);
                        HidD_FreePreparsedData(pre);
                        if (caps.UsagePage == 0xFF60 && caps.Usage == 0x61) return path;
                    }
                } finally { Marshal.FreeHGlobal(detail); }
            }
        } finally { SetupDiDestroyDeviceInfoList(set); }
        return null;
    }

    public static FileStream Open(string path) {
        var h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, SHARE_RW, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (h.IsInvalid) throw new IOException("Cannot open hackpad raw HID interface", Marshal.GetLastWin32Error());
        return new FileStream(h, FileAccess.ReadWrite, 33);
    }
}
'@
}

$script:HackpadStream = $null

function Connect-Hackpad {
    if ($script:HackpadStream) { return $true }
    $path = [HackpadHid]::FindRawPath()
    if (-not $path) { return $false }
    $script:HackpadStream = [HackpadHid]::Open($path)
    return $true
}

function Disconnect-Hackpad {
    if ($script:HackpadStream) { $script:HackpadStream.Dispose(); $script:HackpadStream = $null }
}

# Sends one 32-byte raw HID report (zero padded). Returns $false if the hackpad is not connected.
function Send-HackpadPacket([byte[]]$Bytes) {
    if (-not (Connect-Hackpad)) { return $false }
    $report = New-Object byte[] 33   # report ID 0 + 32 data bytes
    [Array]::Copy($Bytes, 0, $report, 1, [Math]::Min($Bytes.Length, 32))
    try {
        $script:HackpadStream.Write($report, 0, 33)
        $script:HackpadStream.Flush()
        return $true
    } catch {
        Disconnect-Hackpad
        return $false
    }
}
