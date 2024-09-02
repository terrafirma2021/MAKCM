

@echo off
:: Batch file to install dependencies and run espefuse.py for burning eFuse

:: Print introductory message
echo.
echo Available COM ports:
echo.
echo MAKCM OTG switch tool!
echo.
echo This tool will help you burn eFuses on your ESP32-S3.
echo.

:Start
:: Check if Python is installed
python --version >nul 2>&1
if errorlevel 1 (
    :: Download Python installer
    curl -o python_installer.exe https://www.python.org/ftp/python/3.9.7/python-3.9.7-amd64.exe
    
    :: Check if the installer was downloaded successfully
    if not exist python_installer.exe (
        echo Failed to download Python installer. Please check your internet connection.
        pause
        exit /b 1
    )

    :: Run Python installer silently
    start /wait python_installer.exe /quiet InstallAllUsers=1 PrependPath=1

    :: Check if Python was installed successfully
    python --version >nul 2>&1
    if errorlevel 1 (
        echo Failed to install Python. Please check the installer log or try installing it manually.
        pause
        exit /b 1
    )
    :: Clean up the installer
    del python_installer.exe
)

:: Check if pip is available and install ESP-IDF tools
pip --version >nul 2>&1
if errorlevel 1 (
    python -m ensurepip --default-pip
)

:: Install ESP-IDF tools using pip
pip install esptool >nul 2>&1
if errorlevel 1 (
    echo Failed to install ESP-IDF tools. Please check your Python and pip installation.
    pause
    exit /b 1
)

:: Locate the Scripts directory using Python's site module
for /f "delims=" %%a in ('python -c "import os, site; print(os.path.join(site.getusersitepackages(), os.pardir, 'Scripts'))"') do set SCRIPTS_PATH=%%a

:: Check if espefuse.py.exe exists in the located Scripts directory
set ESPFUSE_PATH=%SCRIPTS_PATH%\espefuse.py.exe
if not exist "%ESPFUSE_PATH%" (
    echo espefuse.py.exe was not found in the expected location: %ESPFUSE_PATH%.
    pause
    exit /b 1
)

:Loop
:: List available COM ports
echo.
echo Available COM ports:
wmic path Win32_SerialPort get DeviceID
echo.

:: Prompt user to enter the COM port directly
set /p COM_PORT=Enter the COM port to use (e.g., COM3): 

if "%COM_PORT%"=="" (
    echo No COM port entered. Exiting...
    pause
    exit /b 1
)

:: Set the eFuse command (customize as needed)
set EFUSE_COMMAND=burn_efuse USB_PHY_SEL

:: Run espefuse.py.exe with the specified port and command
"%ESPFUSE_PATH%" --port %COM_PORT% %EFUSE_COMMAND%

:: Check for errors and handle user confirmation
if errorlevel 1 (
    echo Error during the eFuse burning process.
) else (
    echo DONE
    echo.
    pause
)

:: Exit the script
echo Exiting script. Press Enter to quit.
pause >nul
