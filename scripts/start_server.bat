@echo off
echo Starting Team IQ Attendance Server...
cd /d %~dp0..
C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe main\attendance_server.py
pause
