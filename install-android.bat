@echo off
set APK_PATH=%~dp0android-project\app\build\outputs\apk\debug\app-debug.apk

echo [*] Installing APK to device...
adb install -r "%APK_PATH%"
if errorlevel 1 (
    echo [!] Installation failed.
    exit /b 1
)

echo [*] Launching application...
adb shell am start -n com.example.myapp/com.example.myapp.MyActivity