# Minimal JVM class-file reader: prints methods and disassembles their bytecode so
# vanilla constants (swing ticks, first-person hand transforms) can be read from the
# user's own client JAR without a decompiler or a JDK.
# Usage: powershell -File tools/parity/classdump.ps1 <class-file> [method-name-filter]
param([Parameter(Mandatory=$true)][string]$Path, [string]$Filter = '')

$b = [System.IO.File]::ReadAllBytes((Resolve-Path $Path))
$p = 0
function U1 { $v = $b[$script:p]; $script:p += 1; $v }
function U2 { $v = ($b[$script:p] * 256) + $b[$script:p + 1]; $script:p += 2; $v }
function U4 { $v = ([uint32]$b[$script:p] * 16777216) + ($b[$script:p+1] * 65536) + ($b[$script:p+2] * 256) + $b[$script:p+3]; $script:p += 4; $v }
function I2 { $v = U2; if ($v -ge 32768) { $v - 65536 } else { $v } }
function I4 { $v = U4; if ($v -ge 2147483648) { [int64]$v - 4294967296 } else { [int64]$v } }
function F4 { $a = [byte[]]($b[$script:p+3], $b[$script:p+2], $b[$script:p+1], $b[$script:p]); $script:p += 4; [BitConverter]::ToSingle($a, 0) }
function D8 { $a = [byte[]]($b[($script:p+7)..$script:p]); $script:p += 8; [BitConverter]::ToDouble($a, 0) }
function I8 { $a = [byte[]]($b[($script:p+7)..$script:p]); $script:p += 8; [BitConverter]::ToInt64($a, 0) }

if ((U4) -ne [uint32]3405691582) { throw 'not a class file' }
U2 | Out-Null; U2 | Out-Null
$cpCount = U2
$cp = @{}
for ($i = 1; $i -lt $cpCount; $i++) {
    $tag = U1
    switch ($tag) {
        1 { $n = U2; $cp[$i] = @{ tag = 1; s = [Text.Encoding]::UTF8.GetString($b, $p, $n) }; $p += $n }
        3 { $cp[$i] = @{ tag = 3; v = I4 } }
        4 { $cp[$i] = @{ tag = 4; v = F4 } }
        5 { $cp[$i] = @{ tag = 5; v = I8 }; $i++ }
        6 { $cp[$i] = @{ tag = 6; v = D8 }; $i++ }
        { $_ -in 7,8,16,19,20 } { $cp[$i] = @{ tag = $tag; r = U2 } }
        { $_ -in 9,10,11,12,17,18 } { $cp[$i] = @{ tag = $tag; a = U2; b = U2 } }
        15 { $k = U1; $cp[$i] = @{ tag = 15; k = $k; r = U2 } }
        default { throw "unknown cp tag $tag at $i" }
    }
}
function Utf($i) { if ($cp.ContainsKey($i)) { $cp[$i].s } else { "#$i" } }
function Cls($i) { Utf $cp[$i].r }
function Ref($i) {
    $e = $cp[$i]; if (-not $e) { return "#$i" }
    $c = $cp[[int]$e.a]; $n = $cp[[int]$e.b]
    $cn = if ($c -and $c.ContainsKey('r')) { Utf ([int]$c.r) } else { '' }
    $nn = if ($n -and $n.ContainsKey('a')) { Utf ([int]$n.a) } else { '' }
    "$cn.$nn"
}
function Lit($i) {
    $e = $cp[$i]; if (-not $e) { return "#$i" }
    switch ($e.tag) {
        4 { "$($e.v)f" } 3 { "$($e.v)" } 6 { "$($e.v)d" } 5 { "$($e.v)L" }
        8 { '"' + (Utf $e.r) + '"' } 7 { Cls $i } default { Ref $i }
    }
}

U2 | Out-Null; U2 | Out-Null; U2 | Out-Null
$ifCount = U2; for ($i = 0; $i -lt $ifCount; $i++) { U2 | Out-Null }
function SkipMembers {
    $n = U2
    for ($i = 0; $i -lt $n; $i++) {
        U2 | Out-Null; U2 | Out-Null; U2 | Out-Null
        $an = U2
        for ($j = 0; $j -lt $an; $j++) { U2 | Out-Null; $len = U4; $script:p += $len }
    }
}
SkipMembers

$len = New-Object 'int[]' 256
foreach ($r in @(@(0x10,0x10,1),@(0x11,0x11,2),@(0x12,0x12,1),@(0x13,0x14,2),@(0x15,0x19,1),
    @(0x36,0x3a,1),@(0x84,0x84,2),@(0x99,0xa8,2),@(0xa9,0xa9,1),@(0xb2,0xb8,2),@(0xbb,0xbb,2),
    @(0xbc,0xbc,1),@(0xbd,0xbd,2),@(0xc0,0xc1,2),@(0xc5,0xc5,3),@(0xc6,0xc7,2),@(0xc8,0xc9,4),@(0xb9,0xba,4))) {
    for ($o = $r[0]; $o -le $r[1]; $o++) { $len[$o] = $r[2] }
}
$names = @{}
foreach ($k in @{0x00='nop';0x01='aconst_null';0x10='bipush';0x11='sipush';0x12='ldc';0x13='ldc_w';
    0x14='ldc2_w';0x15='iload';0x16='lload';0x17='fload';0x18='dload';0x19='aload';0x36='istore';
    0x37='lstore';0x38='fstore';0x39='dstore';0x3a='astore';0x57='pop';0x58='pop2';0x59='dup';
    0x5a='dup_x1';0x5b='dup_x2';0x5c='dup2';0x84='iinc';0x99='ifeq';0x9a='ifne';0x9b='iflt';
    0x9c='ifge';0x9d='ifgt';0x9e='ifle';0x9f='if_icmpeq';0xa0='if_icmpne';0xa1='if_icmplt';
    0xa2='if_icmpge';0xa3='if_icmpgt';0xa4='if_icmple';0xa5='if_acmpeq';0xa6='if_acmpne';
    0xa7='goto';0xac='ireturn';0xad='lreturn';0xae='freturn';0xaf='dreturn';0xb0='areturn';
    0xb1='return';0xb2='getstatic';0xb3='putstatic';0xb4='getfield';0xb5='putfield';
    0xb6='invokevirtual';0xb7='invokespecial';0xb8='invokestatic';0xb9='invokeinterface';
    0xba='invokedynamic';0xbb='new';0xbc='newarray';0xbd='anewarray';0xbe='arraylength';
    0xbf='athrow';0xc0='checkcast';0xc1='instanceof';0xc2='monitorenter';0xc3='monitorexit';
    0xc6='ifnull';0xc7='ifnonnull';0xc8='goto_w';0xc9='jsr_w' }.GetEnumerator()) { $names[[int]$k.Key] = $k.Value }
foreach ($r in @(@(0x0b,'fconst',3),@(0x1a,'iload',4),@(0x1e,'lload',4),
    @(0x22,'fload',4),@(0x26,'dload',4),@(0x2a,'aload',4),@(0x3b,'istore',4),@(0x3f,'lstore',4),
    @(0x43,'fstore',4),@(0x47,'dstore',4),@(0x4b,'astore',4))) {
    for ($k = 0; $k -lt $r[2]; $k++) { $names[$r[0] + $k] = "$($r[1])_$k" }
}
# iconst, array access, arithmetic and conversions have no regular suffix pattern
$exact = @{0x02='iconst_m1';0x03='iconst_0';0x04='iconst_1';0x05='iconst_2';0x06='iconst_3';
    0x07='iconst_4';0x08='iconst_5';0x09='lconst_0';0x0a='lconst_1';0x0e='dconst_0';0x0f='dconst_1';
    0x2e='iaload';0x2f='laload';0x30='faload';0x31='daload';0x32='aaload';0x33='baload';
    0x34='caload';0x35='saload';0x4f='iastore';0x50='lastore';0x51='fastore';0x52='dastore';
    0x53='aastore';0x54='bastore';0x55='castore';0x56='sastore';
    0x60='iadd';0x61='ladd';0x62='fadd';0x63='dadd';0x64='isub';0x65='lsub';0x66='fsub';0x67='dsub';
    0x68='imul';0x69='lmul';0x6a='fmul';0x6b='dmul';0x6c='idiv';0x6d='ldiv';0x6e='fdiv';0x6f='ddiv';
    0x70='irem';0x71='lrem';0x72='frem';0x73='drem';0x74='ineg';0x75='lneg';0x76='fneg';0x77='dneg';
    0x78='ishl';0x79='lshl';0x7a='ishr';0x7b='lshr';0x7c='iushr';0x7d='lushr';
    0x7e='iand';0x7f='land';0x80='ior';0x81='lor';0x82='ixor';0x83='lxor';
    0x85='i2l';0x86='i2f';0x87='i2d';0x88='l2i';0x89='l2f';0x8a='l2d';0x8b='f2i';0x8c='f2l';
    0x8d='f2d';0x8e='d2i';0x8f='d2l';0x90='d2f';0x91='i2b';0x92='i2c';0x93='i2s';
    0x94='lcmp';0x95='fcmpl';0x96='fcmpg';0x97='dcmpl';0x98='dcmpg'}
foreach ($k in $exact.GetEnumerator()) { $names[[int]$k.Key] = $k.Value }
$branchOps = @(0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f,0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xc6,0xc7)
$refOps = @(0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xbb,0xbd,0xc0,0xc1)

function CodeI4($arr, $o) {
    $v = ([uint32]$arr[$o] * 16777216) + ($arr[$o+1] * 65536) + ($arr[$o+2] * 256) + $arr[$o+3]
    if ($v -ge 2147483648) { [int64]$v - 4294967296 } else { [int64]$v }
}

function Disasm($code) {
    $out = New-Object System.Collections.Generic.List[string]
    $i = 0
    while ($i -lt $code.Length) {
        $at = $i; $op = $code[$i]; $i += 1
        $text = if ($names.ContainsKey([int]$op)) { $names[[int]$op] } else { "op_$($op.ToString('x2'))" }
        $arg = ''
        if ($op -eq 0xaa -or $op -eq 0xab) {
            while (($i % 4) -ne 0) { $i += 1 }
            $def = CodeI4 $code $i; $i += 4
            if ($op -eq 0xaa) {
                $lo = CodeI4 $code $i; $hi = CodeI4 $code ($i + 4); $i += 8 + 4 * ($hi - $lo + 1)
                $arg = "$lo..$hi default $($at + $def)"
            } else {
                $n = CodeI4 $code $i; $i += 4; $pairs = @()
                for ($k = 0; $k -lt $n; $k++) {
                    $key = CodeI4 $code $i; $tgt = CodeI4 $code ($i + 4); $i += 8
                    $pairs += "$key`:$($at + $tgt)"
                }
                $arg = "{$($pairs -join ',')} default $($at + $def)"
            }
        } elseif ($op -eq 0xc4) {
            $wop = $code[$i]; $i += 1
            $idx = ($code[$i] * 256) + $code[$i+1]; $i += 2
            $text = "wide $(if ($names.ContainsKey([int]$wop)) { $names[[int]$wop] } else { $wop })"
            $arg = "$idx"
            if ($wop -eq 0x84) { $arg += " $(($code[$i] * 256) + $code[$i+1])"; $i += 2 }
        } else {
            $n = $len[$op]
            $raw = 0
            if ($n -eq 1) { $raw = [int]$code[$i] }
            elseif ($n -eq 2) { $raw = ($code[$i] * 256) + $code[$i+1] }
            elseif ($n -eq 4) { $raw = ([uint32]$code[$i] * 16777216) + ($code[$i+1] * 65536) + ($code[$i+2] * 256) + $code[$i+3] }
            $i += $n
            if ($op -in 0x12,0x13,0x14) { $arg = Lit $raw }
            elseif ($op -in $refOps) { $arg = Ref $raw }
            elseif ($op -in $branchOps) { $arg = "-> $($at + $raw)" }
            elseif ($op -eq 0x10) { $v = $code[$at+1]; $arg = if ($v -ge 128) { $v - 256 } else { $v } }
            elseif ($op -eq 0x11) { $v = ($code[$at+1] * 256) + $code[$at+2]; $arg = if ($v -ge 32768) { $v - 65536 } else { $v } }
            elseif ($n -gt 0) { $arg = "$raw" }
        }
        $out.Add(("  {0,4}: {1}{2}" -f $at, $text, $(if ($arg) { " $arg" } else { '' })))
    }
    $out -join "`n"
}

$methodCount = U2
for ($m = 0; $m -lt $methodCount; $m++) {
    $access = U2; $name = Utf (U2); $desc = Utf (U2); $an = U2
    $code = $null
    for ($j = 0; $j -lt $an; $j++) {
        $aname = Utf (U2); $alen = U4; $start = $p
        if ($aname -eq 'Code') {
            U2 | Out-Null; U2 | Out-Null
            $cl = U4
            $code = New-Object 'byte[]' $cl
            [Array]::Copy($b, $p, $code, 0, $cl); $p += $cl
            $et = U2; $p += $et * 8
            $ca = U2
            for ($k = 0; $k -lt $ca; $k++) { U2 | Out-Null; $l = U4; $p += $l }
        } else { $p = $start + $alen }
    }
    if ($Filter -and $name -notlike "*$Filter*") { continue }
    Write-Output "`n=== $name$desc (access 0x$($access.ToString('x'))) ==="
    if ($code) { Write-Output (Disasm $code) }
}