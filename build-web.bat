@echo off
pushd "%~dp0dist\web"
cmake --build . --parallel
popd

if not exist "%~dp0public\js" mkdir "%~dp0public\js"
if exist "%~dp0dist\web\app.data" copy /y "%~dp0dist\web\app.data" "%~dp0public\"
copy /y "%~dp0dist\web\app.wasm" "%~dp0public\js\"
copy /y "%~dp0dist\web\app.js" "%~dp0public\js\"