@echo off
rem Quick start: checks tools, finds your game, lifts, builds, makes a shortcut.
rem See README "Getting Started". Safe to run again; it resumes.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup.ps1" %*
pause
