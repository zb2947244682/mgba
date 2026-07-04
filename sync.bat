@echo off
chcp 65001 >nul
echo ==========================================
echo   Git Sync Tool
echo ==========================================
echo.
git pull
git add .
git commit -m "update: %date% %time%"
git push
echo.
echo Sync completed!
echo ==========================================