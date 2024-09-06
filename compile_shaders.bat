@echo off
cd %~dp0/shaders
for %%f in (*.vert *.frag *.comp) do (
	echo Compiling %%f...
	%VULKAN_SDK%/Bin/glslc.exe "%%f" -o "compiled/%%~f.spv"
)
echo Finished.
pause
endlocal