# Live control of the Sage Hackpad over raw HID, no reflashing.
#   hackpad.ps1 led-all <b0> <b1> <b2>        every LED gets these wire bytes
#   hackpad.ps1 led <index> <b0> <b1> <b2>    one LED (0-8, chain order) gets these wire bytes
#   hackpad.ps1 led-off                       back to the firmware's own colors
#   hackpad.ps1 hsv <h> <s> <v>               firmware solid color (0-255 each, v max 200)
#   hackpad.ps1 rotate <0-3>                  OLED rotation (0, 90, 180, 270 degrees)
#   hackpad.ps1 boot                          reboot into the UF2 bootloader (RPI-RP2 drive) for flashing
# Wire bytes go to the LED in the order given; the firmware does no color reordering in this mode.

param([Parameter(Mandatory)][string]$Command, [Parameter(ValueFromRemainingArguments)][int[]]$Values)

. "$PSScriptRoot\hackpad_link.ps1"

$packets = switch ($Command) {
    'led-all' { , [byte[]](0x10, 1); , [byte[]](@(0x12) + $Values[0..2]) }
    'led'     { , [byte[]](0x10, 1); , [byte[]](@(0x11) + $Values[0..3]) }
    'led-off' { , [byte[]](0x10, 0) }
    'hsv'     { , [byte[]](@(0x13) + $Values[0..2]) }
    'rotate'  { , [byte[]](0x20, $Values[0]) }
    'boot'    { , [byte[]](0xB0, 0x5A, 0xA5) }
    default   { throw "Unknown command '$Command'" }
}

foreach ($packet in $packets) {
    if (-not (Send-HackpadPacket $packet)) {
        Write-Error 'Hackpad raw HID interface not found. Is the raw HID firmware flashed and the hackpad plugged in?'
        exit 1
    }
}
"sent $Command $($Values -join ' ')"
