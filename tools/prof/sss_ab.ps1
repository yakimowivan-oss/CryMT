param([string]$Where = 'start', [int]$Width = 2560, [int]$Height = 1440, [int]$Measure = 12)
# Contact shadows A/B in one run (~70 s): village, the level's start (-Where start) or the user's spot (-Where spot),
# then Bend's screen-space shadows (crymt_contact 2), the old 12-step ones (1), none (0): 2 back buffers each
# (logs\play\key_*, in that order), then SSS again for $Measure s of census (GPU time of the "CryMT SSS" pass).
# Then: python tools\prof\sss_compare.py (pixels darker than without, frame-to-frame noise).
$move = ''
if ($Where -eq 'spot') {
    $move = '#g_localActor:EnablePhysics(false);crymt_wait 1;goto 1103 1390 137;crymt_wait 1;' +
            '#g_gameRules.game:MovePlayer(g_localActor.id,{x=1103,y=1390,z=137},{x=-0.0873,y=0,z=-1.117});crymt_wait 1;' +
            '#g_localActor:SetAngles({x=-0.0873,y=0,z=-1.117});'
}
$cmds = "crymt_waitlevel;crymt_wait 2;$move" + 'crymt_wait 6;' +
        'crymt_contact 2;crymt_wait 1;crymt_capture 2;crymt_wait 2;' +
        'crymt_contact 1;crymt_wait 1;crymt_capture 2;crymt_wait 2;' +
        'crymt_contact 0;crymt_wait 1;crymt_capture 2;crymt_wait 2;' +
        "crymt_contact 2;crymt_wait 1;crymt_statsreset;crymt_wait $Measure;quit"
& "$PSScriptRoot\spot_run.ps1" -Variants sss -Tag "sss_$Where" -Width $Width -Height $Height -Commands $cmds
