@echo off
pushd "%~dp0android-project"
call gradlew.bat assembleDebug
popd