@echo off
rem Opens the setup window: builds Rock Band Blitz on this PC from your own copy of the game.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0setup\Setup.ps1"
