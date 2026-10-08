@echo off
chcp 65001 >nul
set "ROOT=%~dp0"
set "PY=%ROOT%.venv\Scripts\python.exe"

echo ============================================
echo   GridStarAgent 启动中...
echo ============================================
echo.

:: 启动 MCP 工具服务 (5656)
echo [1/2] 启动 MCP 工具服务 (端口 5656) ...
start "GridStar MCP Server" cmd /k "cd /d "%ROOT%agent" && "%PY%" server.py"

:: 稍等让 MCP 先就绪
timeout /t 3 /nobreak >nul

:: 启动 Agent 后端 + WebUI (1231)
echo [2/2] 启动 Agent 后端 + WebUI (端口 1231) ...
start "GridStar Agent WebUI" cmd /k "cd /d "%ROOT%agent\agent" && "%PY%" app.py --host 127.0.0.1 --port 1231"

:: 打开浏览器
timeout /t 2 /nobreak >nul
start http://127.0.0.1:1231/

echo.
echo ============================================
echo   启动完成！
echo   浏览器已打开 http://127.0.0.1:1231/
echo   关闭两个 cmd 窗口即可停止服务。
echo ============================================
echo.
pause