:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
:: Installs Ball Aerospace COSMOS on 64-bit Windows 10+
::
:: This is the Qt6 port of COSMOS 4 (github.com/cwozny/COSMOS), which
:: is not on rubygems.org, so the cosmos gem is built from source.
:: Ruby comes from RubyInstaller. Qt 6 and the compiler that builds the
:: COSMOS extensions against it come from MSYS2 in Vendor\msys64.
::
:: Usage: INSTALL_COSMOS [Install Directory] [COSMOS Git Ref]
::
:: Without a git ref, run from a COSMOS checkout, it installs that
:: checkout. Otherwise it downloads the git ref (branch, tag or commit)
:: of cwozny/COSMOS, by default cosmos4-qt6.
:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

@echo off
SETLOCAL ENABLEEXTENSIONS ENABLEDELAYEDEXPANSION
set START_PATH=!PATH!

:: https is required at this point - do not change
set PROTOCOL=https

:: Change this line if you want to force an architecture
set ARCHITECTURE=%PROCESSOR_ARCHITECTURE%

:: Update this version if making any changes to this script
set INSTALLER_VERSION=3.0

:: Paths and versions for COSMOS dependencies
:: RubyInstaller without the Devkit: the Devkit puts a second MSYS2 inside
:: Vendor\Ruby, where Ruby would look for it first.
set RUBY_INSTALLER=rubyinstaller-2.6.10-1-x64.exe
set RUBY_INSTALLER_PATH=//github.com/oneclick/rubyinstaller2/releases/download/RubyInstaller-2.6.10-1/
set RUBY_ABI_VERSION=2.6.0
:: RubyGems 3.5 and later need Ruby 3
set RUBYGEMS_VERSION=3.4.22
set MSYS2_INSTALLER=msys2-x86_64-latest.sfx.exe
set MSYS2_INSTALLER_PATH=//repo.msys2.org/distrib/
:: MINGW64 is the MSVCRT toolchain RubyInstaller 2.6 is built with
set MSYS2_PACKAGES=make mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf mingw-w64-x86_64-qt6-base
set WKHTMLTOPDF=wkhtmltox-0.12.6-1.msvc2015-win64.exe
set WKHTMLPATHWITHPROTOCOL=https://github.com/wkhtmltopdf/packaging/releases/download/0.12.6-1/
set COSMOS_REPO=//github.com/cwozny/COSMOS
set COSMOS_DEFAULT_REF=cosmos4-qt6
set "TAR=%SYSTEMROOT%\System32\tar.exe"

:: Detect Ball
if "%USERDNSDOMAIN%"=="AERO.BALL.COM" (
  set BALL=1
) else (
  if "%USERDOMAIN%"=="AERO" (
    set BALL=1
  ) else (
    set BALL=0
  )
)

:: Detect if SSL_CERT_FILE is set
if not defined SSL_CERT_FILE (
  if !BALL!==1 (
    echo WARN: Install may fail at Ball because SSL_CERT_FILE environment variable is not set
    echo WARN: In particular, the call to bundle for DART dependencies may fail
    echo Please contact COSMOS@ball.com for assistance
  )
)

:: Detect if user is an admin
%SYSTEMROOT%\System32\whoami /groups | %SYSTEMROOT%\System32\find "S-1-5-32-544" > nul
if not errorlevel 1 (
  set ADMIN=1
) else (
  set ADMIN=0
)

:: Detect if any gem files are present in current folder
if exist *.gem (
  echo WARNING: gem files found in the current directory
  echo WARNING: This can cause the installation to fail or install old gems
  pause
)

::::::::::::::::::::::
:: Parse Parameters
::::::::::::::::::::::

IF [%1]==[] (
  set /p COSMOS_INSTALL="Enter COSMOS Install Directory as an absolute path [C:\COSMOS]: "
  IF "!COSMOS_INSTALL!"=="" (
    set COSMOS_INSTALL=C:\COSMOS
  )
) else (
  set COSMOS_INSTALL=%~1
)
if "!COSMOS_INSTALL!"=="!COSMOS_INSTALL::\=!" (
  echo ERROR: Installation folder must be absolute path: "!COSMOS_INSTALL!"
  echo INSTALL FAILED
  pause
  exit /b 1
)
if not "!COSMOS_INSTALL!"=="!COSMOS_INSTALL: =!" (
  echo ERROR: Installation folder must not include spaces: "!COSMOS_INSTALL!"
  echo INSTALL FAILED
  pause
  exit /b 1
)
set COSMOS_INSTALL_FORWARD=%COSMOS_INSTALL:\=/%

set COSMOS_SOURCE=
set COSMOS_REF=
set COSMOS_ZIP=
IF [%2]==[] (
  if exist "%~dp0..\..\..\cosmos.gemspec" (
    for %%I in ("%~dp0..\..\..") do set "COSMOS_SOURCE=%%~fI"
  ) else (
    set COSMOS_REF=!COSMOS_DEFAULT_REF!
  )
) else (
  set COSMOS_REF=%~2
)
if defined COSMOS_SOURCE (
  echo Using Ball Aerospace COSMOS from !COSMOS_SOURCE!
) else (
  set COSMOS_ZIP=!PROTOCOL!:!COSMOS_REPO!/archive/!COSMOS_REF!.zip
  echo Using Ball Aerospace COSMOS !COSMOS_REF! from !COSMOS_ZIP!
)

::::::::::::::::::::::::::::::::::
:: Create Installation Folder
::::::::::::::::::::::::::::::::::

IF EXIST !COSMOS_INSTALL! (
  echo ERROR: Installation folder already exists: "!COSMOS_INSTALL!"
  echo INSTALL FAILED
  pause
  exit /b 1
) else (
  :: Create the installation folder
  mkdir !COSMOS_INSTALL!
  if errorlevel 1 (
    echo ERROR: Failed to create directory: "!COSMOS_INSTALL!"
    echo INSTALL FAILED
    pause
    exit /b 1
  )
)
mkdir !COSMOS_INSTALL!\tmp > nul 2>&1
if errorlevel 1 (
  echo ERROR: Failed to create directory: "!COSMOS_INSTALL!\tmp"
  echo INSTALL FAILED
  pause
  exit /b 1
)

::::::::::::::::::::::::::::::::::::::::
:: Log our settings to the INSTALL.log
::::::::::::::::::::::::::::::::::::::::

@echo COSMOS Windows Installer Version !INSTALLER_VERSION! > !COSMOS_INSTALL!\INSTALL.log
@echo Installing to: !COSMOS_INSTALL! >> !COSMOS_INSTALL!\INSTALL.log
@echo COSMOS_SOURCE=!COSMOS_SOURCE! >> !COSMOS_INSTALL!\INSTALL.log
@echo COSMOS_REF=!COSMOS_REF! >> !COSMOS_INSTALL!\INSTALL.log
@echo PATH=!START_PATH! >> !COSMOS_INSTALL!\INSTALL.log
@echo PROTOCOL=!PROTOCOL! >> !COSMOS_INSTALL!\INSTALL.log
@echo RUBY_INSTALLER=!RUBY_INSTALLER! >> !COSMOS_INSTALL!\INSTALL.log
@echo RUBY_INSTALLER_PATH=!RUBY_INSTALLER_PATH! >> !COSMOS_INSTALL!\INSTALL.log
@echo RUBY_ABI_VERSION=!RUBY_ABI_VERSION! >> !COSMOS_INSTALL!\INSTALL.log
@echo RUBYGEMS_VERSION=!RUBYGEMS_VERSION! >> !COSMOS_INSTALL!\INSTALL.log
@echo MSYS2_INSTALLER=!MSYS2_INSTALLER! >> !COSMOS_INSTALL!\INSTALL.log
@echo MSYS2_INSTALLER_PATH=!MSYS2_INSTALLER_PATH! >> !COSMOS_INSTALL!\INSTALL.log
@echo MSYS2_PACKAGES=!MSYS2_PACKAGES! >> !COSMOS_INSTALL!\INSTALL.log
@echo WKHTMLTOPDF=!WKHTMLTOPDF! >> !COSMOS_INSTALL!\INSTALL.log
@echo WKHTMLPATHWITHPROTOCOL=!WKHTMLPATHWITHPROTOCOL! >> !COSMOS_INSTALL!\INSTALL.log
@echo USERDNSDOMAIN=%USERDNSDOMAIN% >> !COSMOS_INSTALL!\INSTALL.log
@echo BALL=!BALL! >> !COSMOS_INSTALL!\INSTALL.log
@echo SSL_CERT_FILE=%SSL_CERT_FILE% >> !COSMOS_INSTALL!\INSTALL.log
@echo ADMIN=!ADMIN! >> !COSMOS_INSTALL!\INSTALL.log
@echo PROCESSOR_ARCHITECTURE=!ARCHITECTURE! >> !COSMOS_INSTALL!\INSTALL.log
@echo. >> !COSMOS_INSTALL!\INSTALL.log

::::::::::::::::::::::::
:: Install Ruby
::::::::::::::::::::::::

if !ARCHITECTURE!==x86 (
  echo ERROR: INSTALL_COSMOS.bat no longer supports 32-bit Windows"
  echo INSTALL FAILED
  pause
  exit /b 1
) else (
  echo Downloading 64-bit Ruby
  powershell -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; (New-Object Net.WebClient).DownloadFile('!PROTOCOL!:!RUBY_INSTALLER_PATH!!RUBY_INSTALLER!', '!COSMOS_INSTALL!\tmp\!RUBY_INSTALLER!')"
  if errorlevel 1 (
    echo ERROR: Problem downloading 64-bit Ruby from: !PROTOCOL!:!RUBY_INSTALLER_PATH!!RUBY_INSTALLER!
    echo INSTALL FAILED
    @echo ERROR: Problem downloading 64-bit Ruby from: !PROTOCOL!:!RUBY_INSTALLER_PATH!!RUBY_INSTALLER! >> !COSMOS_INSTALL!\INSTALL.log
    pause
    exit /b 1
  ) else (
    @echo Successfully downloaded 64-bit Ruby from: !PROTOCOL!:!RUBY_INSTALLER_PATH!!RUBY_INSTALLER! >> !COSMOS_INSTALL!\INSTALL.log
  )

  echo Installing 64-bit Ruby
  !COSMOS_INSTALL!\tmp\!RUBY_INSTALLER! /silent /tasks="nomodpath,noassocfiles,noridkinstall" /dir="!COSMOS_INSTALL!\Vendor\Ruby" /components="ruby,rdoc"
  if errorlevel 1 (
    echo ERROR: Problem installing 64-bit Ruby
    echo INSTALL FAILED
    @echo ERROR: Problem installing 64-bit Ruby >> !COSMOS_INSTALL!\INSTALL.log
    pause
    exit /b 1
  ) else (
    @echo Successfully installed 64-bit Ruby >> !COSMOS_INSTALL!\INSTALL.log
  )
)

::::::::::::::::::::::::::::::::::::::::::::
:: Install MSYS2, the compiler and Qt 6
::::::::::::::::::::::::::::::::::::::::::::

:: Ruby finds the MSYS2 next to its own folder before any other MSYS2 on the
:: machine. It builds gems with that MSYS2's tools and loads DLLs from its
:: mingw64\bin, which is where the Qt 6 DLLs are.
echo Downloading MSYS2
powershell -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; (New-Object Net.WebClient).DownloadFile('!PROTOCOL!:!MSYS2_INSTALLER_PATH!!MSYS2_INSTALLER!', '!COSMOS_INSTALL!\tmp\!MSYS2_INSTALLER!')"
if errorlevel 1 (
  echo ERROR: Problem downloading MSYS2 from: !PROTOCOL!:!MSYS2_INSTALLER_PATH!!MSYS2_INSTALLER!
  echo INSTALL FAILED
  @echo ERROR: Problem downloading MSYS2 from: !PROTOCOL!:!MSYS2_INSTALLER_PATH!!MSYS2_INSTALLER! >> !COSMOS_INSTALL!\INSTALL.log
  pause
  exit /b 1
) else (
  @echo Successfully downloaded MSYS2 from: !PROTOCOL!:!MSYS2_INSTALLER_PATH!!MSYS2_INSTALLER! >> !COSMOS_INSTALL!\INSTALL.log
)

echo Installing MSYS2
!COSMOS_INSTALL!\tmp\!MSYS2_INSTALLER! -y -o!COSMOS_INSTALL!\Vendor
if errorlevel 1 (
  echo ERROR: Problem installing MSYS2
  echo INSTALL FAILED
  @echo ERROR: Problem installing MSYS2 >> !COSMOS_INSTALL!\INSTALL.log
  pause
  exit /b 1
) else (
  @echo Successfully installed MSYS2 >> !COSMOS_INSTALL!\INSTALL.log
)

:: The first login shell sets up MSYS2 (the pacman keyring). The first update
:: can update MSYS2's core and then stop, so it runs twice.
SET "MSYS2_BASH=!COSMOS_INSTALL!\Vendor\msys64\usr\bin\bash.exe"
"!MSYS2_BASH!" -lc " "
echo Updating MSYS2
"!MSYS2_BASH!" -lc "pacman --noconfirm -Syuu"
"!MSYS2_BASH!" -lc "pacman --noconfirm -Syuu"
echo Installing the compiler and Qt 6
"!MSYS2_BASH!" -lc "pacman --noconfirm -S --needed !MSYS2_PACKAGES!"
if errorlevel 1 (
  echo ERROR: Problem installing MSYS2 packages: !MSYS2_PACKAGES!
  echo INSTALL FAILED
  @echo ERROR: Problem installing MSYS2 packages: !MSYS2_PACKAGES! >> !COSMOS_INSTALL!\INSTALL.log
  pause
  exit /b 1
) else (
  @echo Successfully installed MSYS2 packages: !MSYS2_PACKAGES! >> !COSMOS_INSTALL!\INSTALL.log
)

:: Give Ruby the newer GCC runtime Qt 6 is built with. RubyInstaller 2.6 binds
:: the copies in bin\ruby_builtin_dlls (from 2022) for the whole process, and
:: with them the Qt6 extension fails to load with "127: The specified
:: procedure could not be found". The newer runtime is backward compatible,
:: so Ruby runs on it as well.
for %%D in (libgcc_s_seh-1.dll libwinpthread-1.dll) do (
  copy /y !COSMOS_INSTALL!\Vendor\msys64\mingw64\bin\%%D !COSMOS_INSTALL!\Vendor\Ruby\bin\ruby_builtin_dlls\%%D > nul
  if errorlevel 1 (
    echo ERROR: Problem copying %%D into Ruby
    echo INSTALL FAILED
    @echo ERROR: Problem copying %%D into Ruby >> !COSMOS_INSTALL!\INSTALL.log
    pause
    exit /b 1
  ) else (
    @echo Successfully copied %%D into Ruby >> !COSMOS_INSTALL!\INSTALL.log
  )
)

::::::::::::::::::::::::
:: Install WkHtmlToPdf
::::::::::::::::::::::::

if !ADMIN!==1 (
  echo Downloading WkHtmlToPdf
  powershell -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; (New-Object Net.WebClient).DownloadFile('!WKHTMLPATHWITHPROTOCOL!!WKHTMLTOPDF!', '!COSMOS_INSTALL!\tmp\!WKHTMLTOPDF!')"
  if errorlevel 1 (
    echo WARNING: Problem downloading WkHtmlToPdf from: !WKHTMLPATHWITHPROTOCOL!!WKHTMLTOPDF!
    echo Please download and install this version to enable making PDF files.
    echo INSTALL WARNING
    @echo WARNING: Problem downloading WkHtmlToPdf from: !WKHTMLPATHWITHPROTOCOL!!WKHTMLTOPDF! >> !COSMOS_INSTALL!\INSTALL.log
    pause
  ) else (
    @echo Successfully downloaded WkHtmlToPdf from: !WKHTMLPATHWITHPROTOCOL!!WKHTMLTOPDF! >> !COSMOS_INSTALL!\INSTALL.log
    echo Installing WkHtmlToPdf
    !COSMOS_INSTALL!\tmp\!WKHTMLTOPDF! /S /D=!COSMOS_INSTALL!\Vendor\wkhtmltopdf
    if errorlevel 1 (
      echo ERROR: Problem installing WkHtmlToPdf
      echo Please download and install this version to enable making PDF files.
      echo !WKHTMLPATHWITHPROTOCOL!!WKHTMLTOPDF!
      @echo ERROR: Problem installing WkHtmlToPdf >> !COSMOS_INSTALL!\INSTALL.log
    ) else (
      @echo Successfully installed WkHtmlToPdf >> !COSMOS_INSTALL!\INSTALL.log
    )
  )

) else (
  echo Skipping WkHtmlToPdf installation because you are not an admin.
  echo Please download and install this version to enable making PDF files.
  echo !WKHTMLPATHWITHPROTOCOL!!WKHTMLTOPDF!
  @echo Skipping WkHtmlToPdf installation because you are not an admin. >> !COSMOS_INSTALL!\INSTALL.log
  @echo Please download and install this version to enable making PDF files. >> !COSMOS_INSTALL!\INSTALL.log
  @echo !WKHTMLPATHWITHPROTOCOL!!WKHTMLTOPDF! >> !COSMOS_INSTALL!\INSTALL.log
)

::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
:: Download and unzip the COSMOS source unless installing a checkout
::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

:: The archive of a git ref unzips to one folder, e.g. COSMOS-cosmos4-qt6
if not defined COSMOS_SOURCE (
  echo Downloading COSMOS !COSMOS_REF!
  powershell -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; (New-Object Net.WebClient).DownloadFile('!COSMOS_ZIP!', '!COSMOS_INSTALL!\tmp\COSMOS.zip')"
  if errorlevel 1 (
    echo ERROR: Problem downloading COSMOS from: !COSMOS_ZIP!
    echo INSTALL FAILED
    @echo ERROR: Problem downloading COSMOS from: !COSMOS_ZIP! >> !COSMOS_INSTALL!\INSTALL.log
    pause
    exit /b 1
  ) else (
    @echo Successfully downloaded COSMOS from: !COSMOS_ZIP! >> !COSMOS_INSTALL!\INSTALL.log
  )

  mkdir !COSMOS_INSTALL!\tmp\src
  "!TAR!" -xf !COSMOS_INSTALL!\tmp\COSMOS.zip -C !COSMOS_INSTALL!\tmp\src
  if errorlevel 1 (
    echo ERROR: Problem unzipping COSMOS
    echo INSTALL FAILED
    @echo ERROR: Problem unzipping COSMOS >> !COSMOS_INSTALL!\INSTALL.log
    pause
    exit /b 1
  ) else (
    @echo Successfully unzipped COSMOS >> !COSMOS_INSTALL!\INSTALL.log
  )
  for /d %%F in (!COSMOS_INSTALL!\tmp\src\*) do set "COSMOS_SOURCE=%%~fF"
)

"!TAR!" -xf "!COSMOS_SOURCE!\vendor\installers\windows\COSMOS_Windows_Install.zip" -C !COSMOS_INSTALL!
if errorlevel 1 (
  echo ERROR: Problem unzipping COSMOS Windows files
  echo INSTALL FAILED
  @echo ERROR: Problem unzipping COSMOS Windows files >> !COSMOS_INSTALL!\INSTALL.log
  pause
  exit /b 1
) else (
  @echo Successfully unzipped COSMOS Windows files >> !COSMOS_INSTALL!\INSTALL.log
)

::::::::::::::::::::::::::::::::::::::::::::
:: Setup gemrc to use the correct protocol
::::::::::::::::::::::::::::::::::::::::::::

mkdir !COSMOS_INSTALL!\Vendor\Ruby\lib\ruby\gems\etc > nul 2>&1
if errorlevel 1 (
  echo ERROR: Failed to create directory: "!COSMOS_INSTALL!\Vendor\Ruby\lib\ruby\gems\etc"
  @echo ERROR: Failed to create directory: "!COSMOS_INSTALL!\Vendor\Ruby\lib\ruby\gems\etc" >> !COSMOS_INSTALL!\INSTALL.log
  echo INSTALL FAILED
  pause
  exit /b 1
)
SET "GEMRC=!COSMOS_INSTALL!\Vendor\Ruby\lib\ruby\gems\etc\gemrc"
@echo install: --no-document >> !GEMRC!

::::::::::::::::::::::::::::
:: Install Gems
::::::::::::::::::::::::::::

:: Set environmental variables
SET "GEM_HOME=!COSMOS_INSTALL!\Vendor\Ruby\lib\ruby\gems\!RUBY_ABI_VERSION!"
SET "GEM_PATH=%GEM_HOME%"

:: Prepend embedded bin to PATH so we prefer those binaries
SET "PATH=!COSMOS_INSTALL!\Vendor\Ruby\bin;%PATH%"

:: Remove RUBYOPT and RUBYLIB, which can cause serious problems.
SET RUBYOPT=
SET RUBYLIB=

:: Build gems with C code as C17. MSYS2's GCC defaults to C23, which rejects
:: the Ruby 2.6 C API's VALUE (*)(). ffi strips any -std= from CFLAGS, so
:: -fpermissive also turns that error back into a warning. mkmf reads
:: CONFIGURE_ARGS in every extconf.rb.
SET "CONFIGURE_ARGS=--with-cflags='$(cflags) -std=gnu17 -fpermissive'"
:: Fail instead of installing COSMOS without its GUI when Qt 6 is not found
SET COSMOS_QT6_REQUIRED=1

:: Update gem
call gem update --system !RUBYGEMS_VERSION!
if errorlevel 1 (
  echo ERROR: Problem updating RubyGems to !RUBYGEMS_VERSION!
  echo INSTALL FAILED
  @echo ERROR: Problem updating RubyGems to !RUBYGEMS_VERSION! >> !COSMOS_INSTALL!\INSTALL.log
  pause
  exit /b 1
) else (
  @echo Successfully updated RubyGems to !RUBYGEMS_VERSION! >> !COSMOS_INSTALL!\INSTALL.log
)

:: Build the COSMOS gem, versioned like lib/cosmos/version.rb
set COSMOS_VERSION=
for /f "tokens=2 delims='" %%V in ('findstr /b /c:"COSMOS_VERSION = " "!COSMOS_SOURCE!\lib\cosmos\version.rb"') do set COSMOS_VERSION=%%V
if not defined COSMOS_VERSION (
  echo ERROR: No COSMOS_VERSION in "!COSMOS_SOURCE!\lib\cosmos\version.rb"
  echo INSTALL FAILED
  @echo ERROR: No COSMOS_VERSION in "!COSMOS_SOURCE!\lib\cosmos\version.rb" >> !COSMOS_INSTALL!\INSTALL.log
  pause
  exit /b 1
)
SET "COSMOS_GEM=!COSMOS_INSTALL!\tmp\cosmos-!COSMOS_VERSION!.gem"
echo Building COSMOS gem !COSMOS_VERSION!...
pushd "!COSMOS_SOURCE!"
SET VERSION=!COSMOS_VERSION!
call gem build cosmos.gemspec --output "!COSMOS_GEM!"
set BUILD_RESULT=!errorlevel!
SET VERSION=
popd
if not !BUILD_RESULT!==0 (
  echo ERROR: Problem building cosmos gem
  echo INSTALL FAILED
  @echo ERROR: Problem building cosmos gem >> !COSMOS_INSTALL!\INSTALL.log
  pause
  exit /b 1
) else (
  @echo Successfully built cosmos gem !COSMOS_VERSION! >> !COSMOS_INSTALL!\INSTALL.log
)

:: install COSMOS gem and dependencies
echo Installing COSMOS gem !COSMOS_VERSION!...
call gem install "!COSMOS_GEM!"
if errorlevel 1 (
  echo ERROR: Problem installing cosmos gem
  echo INSTALL FAILED
  @echo ERROR: Problem installing cosmos gem >> !COSMOS_INSTALL!\INSTALL.log
  pause
  exit /b 1
) else (
  @echo Successfully installed cosmos gem >> !COSMOS_INSTALL!\INSTALL.log
)

:::::::::::::::::::::
:: Fix bin stubs to relative paths
:::::::::::::::::::::
@echo forward_directory = "!COSMOS_INSTALL_FORWARD!/Vendor/Ruby/bin" > !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo back_directory = "!COSMOS_INSTALL:\=\\!\\Vendor\\Ruby\\bin" >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo Dir.foreach(forward_directory) do ^|filename^| >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo   if (File.extname(filename).downcase == '.bat') >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo     print "Patching #{filename}" >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo     data = File.read(File.join(forward_directory, filename)) >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo     result = data.gsub^^!(back_directory + '\\', '') >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo     result2 = data.gsub^^!(forward_directory + '/', '') >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo     if result or result2 >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo       File.write(File.join(forward_directory, filename), data) >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo       puts ": patched" >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo     else >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo       puts ": no change needed" >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo     end >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo   end >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
@echo end >> !COSMOS_INSTALL!\tmp\fix_stubs.rb
ruby !COSMOS_INSTALL!\tmp\fix_stubs.rb
if errorlevel 1 (
  echo ERROR: Problem fixing ruby stubs
  @echo ERROR: Problem fixing ruby stubs >> !COSMOS_INSTALL!\INSTALL.log
  pause
) else (
  @echo Successfully fixed ruby stubs >> !COSMOS_INSTALL!\INSTALL.log
)

:::::::::::::::::::::
:: Setup demo areas
:::::::::::::::::::::

set curdir=%cd%
call cosmos install !COSMOS_INSTALL!\Basic
if errorlevel 1 (
  echo ERROR: Problem creating cosmos Basic
  echo INSTALL FAILED
  @echo ERROR: Problem creating cosmos Basic >> !COSMOS_INSTALL!\INSTALL.log
  pause
  exit /b 1
) else (
  @echo Successfully created cosmos Basic >> !COSMOS_INSTALL!\INSTALL.log
)
cd !COSMOS_INSTALL!\Basic\config\dart && call bundle install
call cosmos demo !COSMOS_INSTALL!\Demo
if errorlevel 1 (
  echo ERROR: Problem creating cosmos Demo
  echo INSTALL FAILED
  @echo ERROR: Problem creating cosmos Demo >> !COSMOS_INSTALL!\INSTALL.log
  pause
  exit /b 1
) else (
  @echo Successfully created cosmos Demo >> !COSMOS_INSTALL!\INSTALL.log
)
cd !COSMOS_INSTALL!\Demo\config\dart && call bundle install
cd %curdir%

:::::::::::::::::::::
:: Perform offline configuration
:::::::::::::::::::::

call !COSMOS_INSTALL!\OFFLINE_CONFIG_COSMOS.bat

ENDLOCAL
