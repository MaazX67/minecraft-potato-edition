# Phase 0: build the vanilla 26.3 class/method index from the user's own client JAR.
#
# Never commit the generated ledger.json: it is derived from Mojang's jar and stays local.
# Only aggregate counts (numbers) go into PARITY.md.
#
# Usage:
#   pwsh -File tools/parity/ledger.ps1                 # auto-detect newest version jar
#   pwsh -File tools/parity/ledger.ps1 -Jar path.jar   # explicit jar
#   pwsh -File tools/parity/ledger.ps1 -SummaryOnly    # print the area table only

param(
    [string]$Jar,
    [string]$Out = (Join-Path $PSScriptRoot "ledger.json"),
    [switch]$SummaryOnly
)

$ErrorActionPreference = "Stop"

if (-not $Jar) {
    $versions = Join-Path $env:APPDATA "Migurinth\meta\versions"
    if (-not (Test-Path $versions)) { throw "No Minecraft versions folder at $versions" }
    $Jar = Get-ChildItem $versions -Recurse -Filter "*.jar" -File |
        Sort-Object Length -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not (Test-Path -LiteralPath $Jar)) { throw "Client jar not found: $Jar" }

$refs = @()
foreach ($asm in "System.IO.Compression", "System.IO.Compression.FileSystem") {
    try { Add-Type -AssemblyName $asm -ErrorAction Stop; $refs += $asm } catch {}
}

Add-Type -ReferencedAssemblies $refs -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;

public class VanillaClass {
    public string Name;
    public string Super;
    public List<string> Methods = new List<string>();
}

public static class VanillaJar {
    public static List<string> Errors = new List<string>();
    public static List<VanillaClass> Scan(string jarPath) {
        var list = new List<VanillaClass>();
        using (var zip = ZipFile.OpenRead(jarPath)) {
            foreach (var entry in zip.Entries) {
                if (!entry.FullName.EndsWith(".class", StringComparison.Ordinal)) continue;
                try {
                    using (var s = entry.Open())
                    using (var ms = new MemoryStream()) {
                        s.CopyTo(ms);
                        ms.Position = 0;
                        var c = Parse(ms);
                        if (c != null && c.Name != null) list.Add(c);
                    }
                } catch (Exception ex) {
                    Errors.Add(entry.FullName + " :: " + ex.Message);
                }
            }
        }
        return list;
    }

    static VanillaClass Parse(Stream s) {
        var r = new BinaryReader(s);
        if (U4(r) != 0xCAFEBABE) return null;
        U2(r); U2(r);
        int cpCount = U2(r);
        var utf8 = new string[cpCount];
        var classIdx = new int[cpCount];
        for (int i = 1; i < cpCount; i++) {
            byte tag = r.ReadByte();
            switch (tag) {
                case 1: utf8[i] = Encoding.UTF8.GetString(r.ReadBytes(U2(r))); break;
                case 3: case 4: r.ReadBytes(4); break;
                case 5: case 6: r.ReadBytes(8); i++; break;
                case 7: classIdx[i] = U2(r); break;
                case 8: case 16: case 19: case 20: U2(r); break;
                case 9: case 10: case 11: case 12: case 17: case 18: r.ReadBytes(4); break;
                case 15: r.ReadBytes(3); break;
                default: throw new Exception("bad constant pool tag " + tag);
            }
        }
        U2(r);
        int thisClass = U2(r);
        int superClass = U2(r);
        var info = new VanillaClass();
        if (thisClass > 0 && classIdx[thisClass] > 0) info.Name = utf8[classIdx[thisClass]];
        if (superClass > 0 && classIdx[superClass] > 0) info.Super = utf8[classIdx[superClass]];
        int ifCount = U2(r);
        r.ReadBytes(ifCount * 2);
        int fieldCount = U2(r);
        for (int i = 0; i < fieldCount; i++) { U2(r); U2(r); U2(r); SkipAttrs(r); }
        int methodCount = U2(r);
        for (int i = 0; i < methodCount; i++) {
            U2(r);
            int nameIdx = U2(r);
            int descIdx = U2(r);
            info.Methods.Add(utf8[nameIdx] + utf8[descIdx]);
            SkipAttrs(r);
        }
        return info;
    }

    static ushort U2(BinaryReader r) {
        var b = r.ReadBytes(2);
        if (b.Length < 2) throw new EndOfStreamException();
        return (ushort)((b[0] << 8) | b[1]);
    }

    static uint U4(BinaryReader r) {
        var b = r.ReadBytes(4);
        if (b.Length < 4) throw new EndOfStreamException();
        return ((uint)b[0] << 24) | ((uint)b[1] << 16) | ((uint)b[2] << 8) | b[3];
    }

    static void SkipAttrs(BinaryReader r) {
        int count = U2(r);
        for (int i = 0; i < count; i++) {
            U2(r);
            int len = (int)U4(r);
            r.ReadBytes(len);
        }
    }
}
"@ -Language CSharp

# Area routing: first match wins, so keep specific paths before broad ones.
$areaRules = [ordered]@{
    "worldgen"      = '^net/minecraft/world/level/levelgen|^net/minecraft/world/gen'
    "world-data"    = '^net/minecraft/world/level/chunk|^net/minecraft/world/level/storage'
    "blocks"        = '^net/minecraft/world/level/block'
    "entities"      = '^net/minecraft/world/entity|^net/minecraft/world/item'
    "render"        = '^net/minecraft/client/renderer|^com/mojang/blaze3d'
    "ui"            = '^net/minecraft/client/gui'
    "commands-chat" = '^net/minecraft/commands|^net/minecraft/client/multiplayer/chat'
    "network"       = '^net/minecraft/network|^net/minecraft/server'
    "audio"         = '^net/minecraft/sounds|^net/minecraft/client/sounds'
    "assets"        = '^net/minecraft/resources|^net/minecraft/client/resources|^net/minecraft/core/registries'
    "core"          = '.'
}

Write-Host "Scanning $Jar ..."
$classes = [VanillaJar]::Scan($Jar)
if ([VanillaJar]::Errors.Count -gt 0) {
    Write-Host ("Skipped {0} unreadable classes, first 5:" -f [VanillaJar]::Errors.Count) -ForegroundColor Yellow
    [VanillaJar]::Errors | Select-Object -First 5 | ForEach-Object { Write-Host "  $_" }
}
if ($classes.Count -eq 0) { throw "No classes parsed from $Jar" }

$rows = foreach ($c in $classes) {
    $area = "core"
    foreach ($k in $areaRules.Keys) {
        if ($c.Name -match $areaRules[$k]) { $area = $k; break }
    }
    [pscustomobject]@{ Area = $area; Name = $c.Name; Methods = $c.Methods }
}

$summary = $rows | Group-Object Area | ForEach-Object {
    [pscustomobject]@{
        Area    = $_.Name
        Classes = $_.Count
        Methods = ($_.Group | ForEach-Object { $_.Methods.Count } | Measure-Object -Sum).Sum
    }
} | Sort-Object Methods -Descending

$totalClasses = $rows.Count
$totalMethods = ($rows | ForEach-Object { $_.Methods.Count } | Measure-Object -Sum).Sum

if (-not $SummaryOnly) {
    $doc = [ordered]@{
        jar     = (Split-Path $Jar -Leaf)
        scanned = (Get-Date).ToString("s")
        totals  = [ordered]@{ classes = $totalClasses; methods = $totalMethods }
        areas   = @($summary)
        classes = @($rows | ForEach-Object { [ordered]@{ name = $_.Name; area = $_.Area; methods = $_.Methods } })
    }
    $doc | ConvertTo-Json -Depth 6 -Compress | Set-Content -LiteralPath $Out -Encoding UTF8
    Write-Host "Wrote $Out"
}

""
"{0,-14} {1,8} {2,10}" -f "AREA", "CLASSES", "METHODS"
"{0,-14} {1,8} {2,10}" -f "----", "-------", "-------"
foreach ($r in $summary) { "{0,-14} {1,8} {2,10}" -f $r.Area, $r.Classes, $r.Methods }
"{0,-14} {1,8} {2,10}" -f "TOTAL", $totalClasses, $totalMethods
