$ErrorActionPreference = "Stop"

$qtRoot = "C:/Qt/6.10.2/mingw_64"
$mingwBin = "C:/Qt/Tools/mingw1310_64/bin"
$ninjaExe = "C:/Qt/Tools/Ninja/ninja.exe"
$gxxExe = "$mingwBin/g++.exe"

if (!(Test-Path $qtRoot)) { throw "Qt path not found: $qtRoot" }
if (!(Test-Path $mingwBin)) { throw "MinGW bin path not found: $mingwBin" }
if (!(Test-Path $ninjaExe)) { throw "Ninja not found: $ninjaExe" }
if (!(Test-Path $gxxExe)) { throw "g++ not found: $gxxExe" }

$env:Path = "$mingwBin;C:/Qt/Tools/Ninja;" + $env:Path

if (Test-Path build) {
    Remove-Item -Recurse -Force build
}

cmake -S . -B build -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$ninjaExe" `
  "-DCMAKE_CXX_COMPILER=$gxxExe" `
  "-DCMAKE_PREFIX_PATH=$qtRoot"

cmake --build build
