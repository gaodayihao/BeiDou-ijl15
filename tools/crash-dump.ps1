# Crash triage for the client plugin: read a minidump, resolve plugin addresses through the PDB, and
# find every call site of a plugin function in the built DLL. Written for the vacuum module's crashes
# (see MobVac.cpp) but not tied to it.
#
#   .\crash-dump.ps1 -Action dump    -Path <file.crash.dmp>
#   .\crash-dump.ps1 -Action symbols -Dll <ijl15.dll> -Base 0x61810000 -Addr 0x6182c3ac,0x6182c0f8
#   .\crash-dump.ps1 -Action calls   -Dll <ijl15.dll> -Base 0x61810000 -Target GetControllerPos
#
# -Base is the address the DLL was actually loaded at: the module list in the dump prints it (the
# crashed process had ijl15.dll at 0x61810000 even though the file prefers 0x10000000).
# Resolve a dump's addresses against the build that produced it: the PDB next to the DLL is the only
# symbol source, so save ijl15.pdb (or the whole out\Release) of every build you hand out, otherwise a
# later rebuild silently renames the frames - the addresses stay, the code behind them does not.
param(
    [Parameter(Mandatory=$true)][ValidateSet('dump','symbols','calls')][string]$Action,
    [string]$Path,
    [string]$Dll,
    [UInt64]$Base = 0,
    [string[]]$Addr,
    [string]$Target = "GetControllerPos",
    [int]$StackBytes = 256
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CrashSym
{
    [DllImport("dbghelp.dll", SetLastError = true)] static extern bool SymInitialize(IntPtr h, string p, bool i);
    [DllImport("dbghelp.dll", SetLastError = true)] static extern ulong SymLoadModuleEx(IntPtr h, IntPtr f, string n, string m, ulong b, uint s, IntPtr d, uint fl);
    [DllImport("dbghelp.dll", SetLastError = true)] static extern bool SymFromAddr(IntPtr h, ulong a, out ulong d, IntPtr sym);
    [DllImport("dbghelp.dll", SetLastError = true)] static extern bool SymGetLineFromAddr64(IntPtr h, ulong a, out uint d, IntPtr line);
    [DllImport("dbghelp.dll")] static extern uint SymSetOptions(uint o);
    static IntPtr h = new IntPtr(0x4000);
    static IntPtr NewSym()
    {
        IntPtr b = Marshal.AllocHGlobal(4096);
        for (int i = 0; i < 4096; i++) Marshal.WriteByte(b, i, 0);
        Marshal.WriteInt32(b, 0, 88);        // sizeof(SYMBOL_INFO) on x64
        Marshal.WriteInt32(b, 80, 2000);     // MaxNameLen (x64 layout: NameLen@76, Name@84)
        return b;
    }
    public static bool Init(string dll, ulong b)
    {
        SymSetOptions(0x2 | 0x10 | 0x80000000);   // UNDNAME | LOAD_LINES | DEBUG
        if (!SymInitialize(h, System.IO.Path.GetDirectoryName(dll), false)) return false;
        return SymLoadModuleEx(h, IntPtr.Zero, dll, null, b, 0, IntPtr.Zero, 0) != 0;
    }
    public static string ByAddr(ulong a, out ulong disp)
    {
        IntPtr b = NewSym();
        string r; string line = "";
        if (SymFromAddr(h, a, out disp, b))
        {
            r = Marshal.PtrToStringAnsi(new IntPtr(b.ToInt64() + 84), Marshal.ReadInt32(b, 76));
        }
        else { disp = 0; r = null; }
        IntPtr ln = Marshal.AllocHGlobal(64);
        Marshal.WriteInt32(ln, 0, 40);
        uint ld;
        if (SymGetLineFromAddr64(h, a, out ld, ln))
        {
            IntPtr fn = Marshal.ReadIntPtr(ln, 16);
            if (fn != IntPtr.Zero) line = " [" + Marshal.PtrToStringAnsi(fn) + ":" + Marshal.ReadInt32(ln, 8) + "]";
        }
        Marshal.FreeHGlobal(b);
        Marshal.FreeHGlobal(ln);
        return r == null ? null : (r + line);
    }
}
'@

function Read-Pe([string]$file)
{
    $script:img = [System.IO.File]::ReadAllBytes($file)
    function U32([int]$o) { [BitConverter]::ToUInt32($script:img, $o) }
    $pe = U32 0x3C
    $n = [BitConverter]::ToUInt16($script:img, $pe + 6)
    $opt = [BitConverter]::ToUInt16($script:img, $pe + 20)
    $tab = $pe + 24 + $opt
    $secs = @()
    for ($i = 0; $i -lt $n; $i++) {
        $s = $tab + $i * 40
        $secs += @{
            Name = [System.Text.Encoding]::ASCII.GetString($script:img, $s, 8).TrimEnd([char]0)
            Rva = U32 ($s + 12); RawSize = U32 ($s + 16); Raw = U32 ($s + 20)
        }
    }
    return $secs
}

if ($Action -eq 'dump') {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    function U32([int]$o) { [BitConverter]::ToUInt32($bytes, $o) }
    function U64([int]$o) { [BitConverter]::ToUInt64($bytes, $o) }

    $nStreams = U32 8
    $dir = U32 12
    $st = @{}
    for ($i = 0; $i -lt $nStreams; $i++) {
        $o = $dir + $i * 12
        $st[[int](U32 $o)] = @{ Size = (U32 ($o + 4)); Rva = (U32 ($o + 8)) }
    }

    $mods = @()
    $o = $st[4].Rva
    for ($i = 0; $i -lt (U32 $o); $i++) {
        $m = $o + 4 + $i * 108
        $b = U64 $m; $sz = U32 ($m + 8); $nr = U32 ($m + 20)
        $mods += @{ Base = [uint64]$b; End = [uint64]($b + $sz); Name = (Split-Path ([System.Text.Encoding]::Unicode.GetString($bytes, $nr + 4, (U32 $nr))) -Leaf) }
    }
    function Mod([uint64]$va) { foreach ($m in $mods) { if ($va -ge $m.Base -and $va -lt $m.End) { return ("{0}+0x{1:x}" -f $m.Name, ($va - $m.Base)) } } return $null }

    $o = $st[5].Rva
    $ranges = @()
    for ($i = 0; $i -lt (U32 $o); $i++) {
        $d = $o + 4 + $i * 16
        $ranges += @{ Start = [uint64](U64 $d); Size = (U32 ($d + 8)); Rva = (U32 ($d + 12)) }
    }
    function Mem([uint64]$va, [int]$len) {
        foreach ($r in $ranges) {
            if ($va -ge $r.Start -and ($va + $len) -le ($r.Start + $r.Size)) {
                $out = New-Object byte[] $len
                [Array]::Copy($bytes, $r.Rva + [int]($va - $r.Start), $out, 0, $len)
                return $out
            }
        }
        return $null
    }

    $o = $st[6].Rva
    $excAddr = U64 ($o + 24)
    Write-Output ("exception: tid=0x{0:x} code=0x{1:x8} address=0x{2:x8} ({3})" -f (U32 $o), (U32 ($o + 8)), $excAddr, (Mod $excAddr))
    for ($i = 0; $i -lt (U32 ($o + 32)); $i++) { Write-Output ("  info[{0}] = 0x{1:x}" -f $i, (U64 ($o + 40 + $i * 8))) }

    $c = U32 ($o + 164)
    $eip = U32 ($c + 184); $esp = U32 ($c + 196)
    Write-Output ("context  : eip=0x{0:x8} ({1})" -f $eip, (Mod ([uint64]$eip)))
    Write-Output ("           eax={0:x8} ebx={1:x8} ecx={2:x8} edx={3:x8} esi={4:x8} edi={5:x8} ebp={6:x8} esp={7:x8}" -f `
        (U32 ($c + 176)), (U32 ($c + 164)), (U32 ($c + 172)), (U32 ($c + 168)), (U32 ($c + 160)), (U32 ($c + 156)), (U32 ($c + 180)), $esp)

    $s = Mem ([uint64]$esp) $StackBytes
    if ($s -ne $null) {
        Write-Output "stack (words that land in a loaded module):"
        for ($i = 0; $i + 4 -le $s.Length; $i += 4) {
            $v = [BitConverter]::ToUInt32($s, $i)
            if ($v -gt 0x00400000) { $mm = Mod ([uint64]$v); if ($mm -ne $null) { Write-Output ("  esp+0x{0:x3} = 0x{1:x8}  {2}" -f $i, $v, $mm) } }
        }
    }
    else { Write-Output "stack at esp is not in the dump" }
    return
}

$sections = Read-Pe $Dll
if (-not [CrashSym]::Init($Dll, $Base)) { Write-Output "symbol load failed (pdb next to the dll?)"; return }

if ($Action -eq 'symbols') {
    foreach ($a in $Addr) {
        $d = [uint64]0
        $n = [CrashSym]::ByAddr([uint64]$a, [ref]$d)
        Write-Output ("0x{0:x8}  {1}" -f ([uint64]$a), ($(if ($n) { "$n+0x$($d.ToString('x'))" } else { "(no symbol)" })))
    }
    return
}

# calls: name -> base address, discovered by walking .text with SymFromAddr (covers statics)
$text = $sections | Where-Object { $_.Name -eq '.text' }
$bases = @{}
$limit = [Math]::Min($text.RawSize, $img.Length - $text.Raw)
for ($i = 0; $i -lt $limit; $i += 16) {
    $va = $Base + [uint64]($text.Rva + $i)
    $d = [uint64]0
    $n = [CrashSym]::ByAddr($va, [ref]$d)
    if ($n -ne $null) { $n = ($n -split ' \[')[0]; if (-not $bases.ContainsKey($n)) { $bases[$n] = $va - $d } }
}
if (-not $bases.ContainsKey($Target)) { Write-Output "symbol '$Target' not found in $Dll"; return }

$targetRva = [int]($bases[$Target] - $Base)
Write-Output ("$Target @ rva 0x{0:x} - call sites:" -f $targetRva)
foreach ($sec in $sections) {
    if ($sec.Name -ne '.text') { continue }
    $lim = [Math]::Min($sec.RawSize, $img.Length - $sec.Raw)
    for ($i = 0; $i + 5 -le $lim; $i++) {
        if ($img[$sec.Raw + $i] -ne 0xE8) { continue }
        $rel = [BitConverter]::ToInt32($img, $sec.Raw + $i + 1)
        $callerRva = $sec.Rva + $i
        if (($callerRva + 5 + $rel) -ne $targetRva) { continue }
        $va = $Base + [uint64]$callerRva
        $d = [uint64]0
        $n = [CrashSym]::ByAddr($va, [ref]$d)
        Write-Output ("  rva 0x{0:x5}  in {1} +0x{2:x}" -f $callerRva, (($n -split ' \[')[0]), $d)
    }
}
