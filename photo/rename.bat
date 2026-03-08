@echo off
setlocal enabledelayedexpansion

:: 第一步：把所有 jpg 临时改名为 tmp_数字.jpg，避免与 plana_数字.jpg 冲突
set i=1
for %%f in (*.jpg) do (
    ren "%%f" "tmp_!i!.jpg"
    set /a i+=1
)

:: 第二步：再把 tmp_数字.jpg 统一改成 plana_数字.jpg，保证 1..N 连续
set i=1
for %%f in (tmp_*.jpg) do (
    ren "%%f" "plana_!i!.jpg"
    set /a i+=1
)

endlocal