CryMT v16 for Crysis (2007): multithreaded rendering, DirectX 12, NVIDIA DLAA, global illumination
==================================================================================================

What it is
----------
CryMT is a runtime modification of Crysis 1.2.1 (64-bit). It was made by reverse engineering
the shipped game binaries, without engine source code.

Bin64\dbghelp.dll loads CryMT.dll whenever Bin64\Crysis.exe starts, and CryMT then changes how
the engine works, in memory only. No game file is replaced. The one exception: with GI on, a
Game\zzCryMT.pak with edits to five shaders is added.

  * Multithreading: the renderer runs on its own thread while the main thread builds the next frame.
  * DirectX 12: its own back end instead of D3D10, with pipeline states created ahead of time
    (no first-use stutter).
  * NVIDIA DLAA (DLSS at native resolution) instead of the stock anti-aliasing. Needs an RTX card.
  * Global illumination: bounced light and occlusion from the surroundings (voxels around the
    camera plus a screen part), GTAO, contact shadows, lit smoke and dust.
  * Launcher: ready-made modes, every setting with an explanation, updates.

Requirements
------------
  * Crysis 1.2.1, 64-bit (build 6156): Steam, GOG or disc with patch 1.2.1.
    Warhead, Wars and Remastered are not supported.
  * c1-launcher as Bin64\Crysis.exe (GOG installs it): https://github.com/ccomrade/c1-launcher/releases
    v7 (GOG's) and v8 both work. Only v8 fixes the 64-bit game's crashes with memory above
    4 GB; with an older one CryMT applies its own fix.
  * Windows 10/11 64-bit. The launcher needs the Microsoft Edge WebView2 Runtime, which is
    already part of an updated Windows 10 or 11.
  * DLAA needs an NVIDIA RTX card. The other modes work on any DirectX 12 card.

Install
-------
  1. Unpack the archive into the game folder (where Bin64 is).
  2. Start CryMT\CryMTLauncher.exe, pick a mode and press Play.

Mods: put the mod into the game's Mods folder and pick it on the launcher's home page (Mod).
Play starts the game with -mod <name>. If the mod changes the shaders the GI patches, the GI
is off for it. The Editor button (with the Mod SDK installed) opens the Sandbox Editor with the
chosen mod and with CryMT (DirectX 12 and the GI in the viewport, no DLAA); the launcher's
"Editor with CryMT" switch turns CryMT off for the editor. An editor started outside the
launcher runs without CryMT.

Starting the game the usual way (Steam, shortcut, a mod's .bat) works too. It then uses
CryMT\CryMT.ini, which the launcher writes.

Updates
-------
The launcher takes builds from the releases of https://github.com/yakimowivan-oss/CryMT-Releases
(Updates tab).
  * Before installing, it checks the signature of the file list and the SHA-256 of every file.
  * Replaced files are kept in CryMT\backup, so "Rollback" brings the previous version back.
  * CryMT.ini is never touched by an update.

Uninstall
---------
Delete Bin64\dbghelp.dll, the CryMT folder and Game\zzCryMT.pak (if GI was on).

License
-------
CryMT is licensed under the GNU GPL 3.0. Source: https://github.com/yakimowivan-oss/CryMT
Third-party components and their licenses are listed in CryMT\licenses.

Crysis is (c) Crytek. The archive contains no game files; you need your own legal copy.
