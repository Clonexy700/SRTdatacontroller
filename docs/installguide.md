# Требования для установки
- CLion или Visual Studio
- pthreads
- OpenSSL (https://slproweb.com/products/Win32OpenSSL.html)
- git
- cmake (https://cmake.org/)
- компилятор с++

# Установка

## На Windows:
`git clone https://github.com/microsoft/vcpkg.git`
`cd vcpkg`
`.\bootstrap-vcpkg.bat`
`\.vcpkg install pthreads --triplet x64-windows`
`\.vcpkg install openssl --triplet x64-windows`
`\.vcpkg integrate install`
`cd SRTdatacontroller`
`md _build`
`cd _build`
`cmake ../ -DCMAKE_TOOLCHAIN_FILE=SRTdatacontroller\vcpkg\scripts\buildsystems\vcpkg.cmake `
`cmake --build ./ --config Release`


## Если при cmake возникает ошибка:
Убедиться, что на диске C:\ установлен C:\pthread-win64 со всем необходимым, если он установлен, но не на диске C:\, то отредактируйте CMAKE файл в submodule\srt