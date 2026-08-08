[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [switch] $WithoutLisp,
    [switch] $InstallLLVM,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '.')).Path
$out = Join-Path $root (Join-Path 'build\windows' $Configuration)

if ($Clean -and (Test-Path $out)) {
    Remove-Item -Recurse -Force $out
}
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item (Join-Path $out '*.obj'), (Join-Path $out 'kg.exe') `
    -Force -ErrorAction SilentlyContinue

$clang = @(
    (Join-Path ${env:ProgramFiles} 'LLVM\bin\clang-cl.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'LLVM\bin\clang-cl.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $clang -and $InstallLLVM) {
    & winget install --id LLVM.LLVM -e --source winget --silent `
        --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) {
        throw "winget could not install LLVM (exit code $LASTEXITCODE)"
    }
    $clang = @(
        (Join-Path ${env:ProgramFiles} 'LLVM\bin\clang-cl.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'LLVM\bin\clang-cl.exe')
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $clang) {
    throw 'clang-cl.exe was not found. Install LLVM (winget install LLVM.LLVM) or pass -InstallLLVM.'
}

$vcvars = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'),
    (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) {
    throw 'vcvarsall.bat was not found. Install the Visual Studio C++ build tools first.'
}

$lispSources = @(
    'lisp_prelude.c', 'lisp_string.c', 'lisp_buffer.c', 'lisp_word.c',
    'lisp_io.c', 'lisp_cmd.c', 'lisp_obj.c', 'lisp_search.c',
    'lisp_hooks.c', 'lisp_process.c', 'lisp_require.c'
)
$required = @('fe\tiny-regex-c\re.c')
if (-not $WithoutLisp) {
    $required += @('fe\fe.c', 'fe\fe_eval.c', 'fe\fe_run.c')
}
foreach ($path in $required) {
    if (-not (Test-Path (Join-Path $root $path))) {
        throw "Required submodule source is missing: $path. Run git submodule update --init --recursive."
    }
}
$sources = @(Get-ChildItem (Join-Path $root 'src\*.c') |
    Where-Object { (-not $WithoutLisp) -or $_.Name -notin $lispSources } |
    ForEach-Object { $_.FullName })
$sources += Join-Path $root 'fe\tiny-regex-c\re.c'
if (-not $WithoutLisp) {
    $sources += @(
        (Join-Path $root 'fe\fe.c'),
        (Join-Path $root 'fe\fe_eval.c'),
        (Join-Path $root 'fe\fe_run.c')
    )
}

$flags = @(
    '/nologo', '/clang:-std=c2x', '/W3',
    '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_DEPRECATE',
    ('/I"{0}\src"' -f $root), ('/I"{0}\fe\tiny-regex-c"' -f $root)
)
if (-not $WithoutLisp) {
    $flags += '/DKG_USE_LISP=1'
}
if ($Configuration -eq 'Debug') {
    $flags += @('/Od', '/Zi')
}
else {
    $flags += '/O2'
}

$quotedSources = $sources | ForEach-Object { '"' + $_ + '"' }
$quotedFlags = $flags -join ' '
$compile = "call `"$vcvars`" x64 && cd /d `"$out`" && `"$clang`" $quotedFlags /c $($quotedSources -join ' ')"
& cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) {
    throw "C compilation failed (exit code $LASTEXITCODE)"
}

$objects = @(Get-ChildItem (Join-Path $out '*.obj') | ForEach-Object { '"' + $_.FullName + '"' })
$link = "call `"$vcvars`" x64 && cd /d `"$out`" && `"$clang`" /nologo /Fe:kg.exe $($objects -join ' ')"
& cmd.exe /d /s /c $link
if ($LASTEXITCODE -ne 0) {
    throw "Link failed (exit code $LASTEXITCODE)"
}

Write-Host "Built $([IO.Path]::GetFullPath((Join-Path $out 'kg.exe')))"
