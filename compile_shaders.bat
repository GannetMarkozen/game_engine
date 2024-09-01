@echo off
cd %~dp0/shaders
REM for %%i in (*vert *.frag *comp) do %VULKAN_SDK%/Bin/glslc.exe "%%i" -o "%%~ni.spv"
for %%f in (*.vert *.frag *.comp) do (
	echo Compiling %%f...
	%VULKAN_SDK%/Bin/glslc.exe "%%f" -o "compiled/%%~nf.spv"
)

echo Finished.
pause
endlocal